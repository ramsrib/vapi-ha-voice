/* Vapi voice assistant, running on the device itself.
 *
 * Opens a Vapi call over the `vapi.websocket` transport and pumps raw PCM both
 * ways between ESPHome's microphone and speaker abstractions. No Home Assistant
 * and no laptop are involved once this is flashed.
 *
 * The audio contract is a straight pipe: the XMOS XU316 on this board hands the
 * ESP32 16 kHz mono audio that has already been through hardware AEC,
 * beamforming and noise suppression, and 16 kHz mono s16le is exactly what Vapi
 * carries. Nothing is transcoded on the way in. On the way out a `resampler`
 * speaker takes the 16 kHz stream up to the 48 kHz the hardware runs at.
 */

#pragma once

#ifdef USE_ESP_IDF

#include <atomic>
#include <string>
#include <vector>

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace esphome {
namespace vapi_assistant {

enum class VapiState : uint8_t {
  IDLE = 0,
  STARTING,  // HTTP POST /call is in flight
  ACTIVE,    // websocket open, audio flowing
  STOPPING,
};

class VapiAssistant : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  /* The call needs the network, so come up after it. */
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_microphone_source(microphone::MicrophoneSource *mic) { this->mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
  void set_api_key(const std::string &v) { this->api_key_ = v; }
  void set_api_url(const std::string &v) { this->api_url_ = v; }
  void set_assistant_id(const std::string &v) { this->assistant_id_ = v; }
  void set_first_message(const std::string &v) { this->first_message_ = v; }
  void set_sample_rate(uint32_t v) { this->sample_rate_ = v; }
  void set_frame_ms(uint16_t v) { this->frame_ms_ = v; }
  void set_volume(float v) { this->volume_ = v; }
  void set_silence_timeout(uint32_t v) { this->silence_timeout_ = v; }
  void set_wake_silence_timeout(uint32_t v) { this->wake_silence_timeout_ = v; }
  void set_max_duration(uint32_t v) { this->max_duration_ = v; }
  void set_stop_speaking_num_words(uint8_t v) { this->stop_speaking_num_words_ = v; }
  void set_system_prompt(const std::string &v) { this->system_prompt_ = v; }
  void set_model_provider(const std::string &v) { this->model_provider_ = v; }
  void set_model(const std::string &v) { this->model_ = v; }
  void set_voice_provider(const std::string &v) { this->voice_provider_ = v; }
  void set_voice_id(const std::string &v) { this->voice_id_ = v; }
  void set_transcriber_provider(const std::string &v) { this->transcriber_provider_ = v; }
  void set_transcriber_model(const std::string &v) { this->transcriber_model_ = v; }
  void set_end_call_message(const std::string &v) { this->end_call_message_ = v; }
  void add_end_call_phrase(const std::string &v) { this->end_call_phrases_.push_back(v); }
  void set_voice_model(const std::string &v) { this->voice_model_ = v; }
  void set_stability(float v) { this->stability_ = v; }
  void set_similarity_boost(float v) { this->similarity_boost_ = v; }
  void set_style(float v) { this->style_ = v; }
  void set_use_speaker_boost(bool v) { this->use_speaker_boost_ = v; }

  /**
   * @brief  Place a call. Safe to call from an automation; returns immediately.
   *
   * @param from_wake_word  Use the shorter wake-word silence timeout. A wake
   *   word can fire on a passing "happy" or on the television, and on this
   *   device a call costs money — so a call nobody asked for should hang up in
   *   seconds. A call started deliberately, by the button, gets the full
   *   timeout because a long pause there is a person thinking.
   */
  void start_call(bool from_wake_word = false);
  /** Hang up. Safe to call when no call is running. */
  void stop_call();

  bool is_active() const { return this->state_.load() == VapiState::ACTIVE; }
  bool is_idle() const { return this->state_.load() == VapiState::IDLE; }

  void add_on_start_callback(std::function<void()> &&cb) { this->on_start_.add(std::move(cb)); }
  void add_on_end_callback(std::function<void()> &&cb) { this->on_end_.add(std::move(cb)); }
  void add_on_error_callback(std::function<void(std::string)> &&cb) { this->on_error_.add(std::move(cb)); }

 protected:
  /* Runs on its own task: the REST call that creates the Vapi call blocks for
   * as long as TLS plus Vapi's own latency takes, which is far too long for
   * loop(). */
  static void call_task_(void *param);
  void run_call_task_();
  bool create_call_(std::string &ws_url_out);
  bool open_websocket_(const std::string &ws_url);
  void close_websocket_();
  void on_mic_data_(const std::vector<uint8_t> &data);
  /* Writes every byte to the speaker, rather than however many it happened to
   * accept on the first try. */
  void write_pcm_(const uint8_t *data, size_t len);
  static void ws_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
  void handle_ws_event_(int32_t event_id, void *event_data);
  void fail_(const std::string &reason);

  microphone::MicrophoneSource *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  std::string api_key_;
  std::string api_url_{"https://api.vapi.ai"};
  std::string assistant_id_;
  std::string first_message_;
  uint32_t sample_rate_{16000};
  uint16_t frame_ms_{20};
  /* Below 1.0 by default. At full scale the amplifier clips on loud speech,
   * and clipping is not just ugly — it makes the echo nonlinear, which is
   * exactly what the XMOS canceller cannot remove, so the device stops hearing
   * anyone while it is talking. */
  float volume_{0.6f};

  /* Vapi's default silence timeout is short enough that a pause in
   * conversation ends the call. */
  uint32_t silence_timeout_{300};
  uint32_t wake_silence_timeout_{20};
  /* Which timeout the in-flight call should use. Written before the call task
   * starts and read by it, never concurrently. */
  bool call_is_from_wake_word_{false};
  uint32_t max_duration_{1800};
  /* How many words must be transcribed before the assistant lets itself be
   * interrupted. Zero — Vapi's default — means any voice activity stops it,
   * and residual echo of its own voice is voice activity, so it talks over
   * itself. Requiring real words filters that out without killing barge-in. */
  uint8_t stop_speaking_num_words_{2};

  /* Transient assistant: defined inline on each call rather than referenced by
   * id. Nothing on the dashboard is read or written, so the device cannot be
   * surprised by a saved assistant's system prompt outranking what it sends. */
  std::string system_prompt_;
  std::string model_provider_{"anthropic"};
  std::string model_{"claude-haiku-4-5-20251001"};
  std::string voice_provider_{"11labs"};
  std::string voice_id_{"jsCqWAovK2LkecY7zXl4"};  // Freya
  std::string transcriber_provider_{"deepgram"};
  std::string transcriber_model_{"nova-3"};

  /* Saying "goodbye" should end the call rather than leaving it running until a
   * silence timeout, which bills for the silence and leaves the ring lit. */
  std::vector<std::string> end_call_phrases_;
  std::string end_call_message_;

  /* ElevenLabs delivery. These matter more than which voice is chosen: at high
   * stability every premade voice reads flat and businesslike. Lower stability
   * lets the delivery vary; style exaggerates it. Only sent for the 11labs
   * provider — Vapi's own voices do not take them. */
  std::string voice_model_{"eleven_turbo_v2_5"};
  float stability_{0.3f};
  float similarity_boost_{0.75f};
  float style_{0.6f};
  bool use_speaker_boost_{true};

  esp_websocket_client_handle_t ws_{nullptr};
  std::atomic<VapiState> state_{VapiState::IDLE};

  /* Mic bytes accumulate here until a whole frame is ready. Touched by the
   * microphone task and drained by the same callback, so no lock is needed —
   * but it must not be touched once the websocket is gone. */
  std::vector<uint8_t> tx_buffer_;
  size_t frame_bytes_{640};

  /* Set by the websocket task, acted on by loop(), because ESPHome callbacks
   * must fire on the main task. */
  std::atomic<bool> pending_started_{false};
  std::atomic<bool> pending_ended_{false};
  std::string pending_error_;
  SemaphoreHandle_t error_mutex_{nullptr};

  /* Audio that the speaker could not take. Non-zero here is the difference
   * between "the network is slow" and "the buffers are too small", and is
   * otherwise indistinguishable from distortion by ear. */
  std::atomic<uint32_t> dropped_bytes_{0};
  uint32_t last_reported_drops_{0};

  CallbackManager<void()> on_start_;
  CallbackManager<void()> on_end_;
  CallbackManager<void(std::string)> on_error_;
};

template<typename... Ts> class StartCallAction : public Action<Ts...>, public Parented<VapiAssistant> {
  void play(const Ts &...x) override { this->parent_->start_call(false); }
};

template<typename... Ts> class StartFromWakeWordAction : public Action<Ts...>, public Parented<VapiAssistant> {
  void play(const Ts &...x) override { this->parent_->start_call(true); }
};

template<typename... Ts> class StopCallAction : public Action<Ts...>, public Parented<VapiAssistant> {
  void play(const Ts &...x) override { this->parent_->stop_call(); }
};

template<typename... Ts> class IsActiveCondition : public Condition<Ts...>, public Parented<VapiAssistant> {
  bool check(const Ts &...x) override { return this->parent_->is_active(); }
};

class StartTrigger : public Trigger<> {
 public:
  explicit StartTrigger(VapiAssistant *parent) {
    parent->add_on_start_callback([this]() { this->trigger(); });
  }
};

class EndTrigger : public Trigger<> {
 public:
  explicit EndTrigger(VapiAssistant *parent) {
    parent->add_on_end_callback([this]() { this->trigger(); });
  }
};

class ErrorTrigger : public Trigger<std::string> {
 public:
  explicit ErrorTrigger(VapiAssistant *parent) {
    parent->add_on_error_callback([this](std::string err) { this->trigger(std::move(err)); });
  }
};

}  // namespace vapi_assistant
}  // namespace esphome

#endif  // USE_ESP_IDF
