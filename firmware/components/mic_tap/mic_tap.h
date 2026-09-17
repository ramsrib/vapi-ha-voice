#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/microphone/microphone_source.h"

#include <atomic>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>

namespace esphome {
namespace mic_tap {

/* Streams the microphone to a host on the LAN so training data can be collected
 * from the signal the wake-word engine actually sees.
 *
 * Framed, because the stream carries two kinds of message: the audio, and
 * labels marking the instants the wake word fired. Injecting labels into the
 * PCM itself would corrupt the audio, and sending them out of band would lose
 * the alignment that makes them worth having.
 *
 *   uint8  type    0 = PCM (s16le mono), 1 = label
 *   uint8  _pad
 *   uint16 length  payload bytes, little endian
 *   uint8  payload[length]
 *
 * The session header is sent once on connect, before any frame:
 *   "MICTAP01" uint32 sample_rate  uint16 channels  uint16 bits_per_sample
 */
class MicTap : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_microphone_source(microphone::MicrophoneSource *mic) { this->mic_ = mic; }
  void set_host(const std::string &host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }

  void start();
  void stop();
  void mark(const std::string &label);
  bool is_streaming() const { return this->streaming_.load(); }

 protected:
  void on_mic_data_(const std::vector<uint8_t> &data);
  /// Enqueue a complete frame. All-or-nothing: a frame that does not fit is
  /// dropped whole, so what reaches the socket is always frame-aligned.
  bool queue_frame_(uint8_t type, const uint8_t *payload, size_t len);
  void flush_();
  void close_socket_();

  microphone::MicrophoneSource *mic_{nullptr};
  std::string host_;
  uint16_t port_{9000};

  /* The microphone callback runs on the I2S task while start()/stop() run on
   * the main loop, so the socket is shared across two contexts. Closing it out
   * from under an in-flight send() would be a use-after-free on a reused fd, so
   * the close is deferred to loop() and only happens once no send is active. */
  int sock_{-1};  // only touched on the main loop now
  std::atomic<bool> streaming_{false};

  /* The socket is written from loop(), never from the audio path. Audio only
   * ever copies into this ring, so a slow or stalled network costs buffered
   * seconds instead of microphone data — and cannot kill the stream. Writing
   * to a socket from the mic task is what made captures die after 30-45
   * seconds on the first two attempts. */
  uint8_t *ring_{nullptr};
  size_t ring_size_{0};
  size_t head_{0}, tail_{0}, fill_{0};
  portMUX_TYPE ring_mux_ = portMUX_INITIALIZER_UNLOCKED;

  std::atomic<uint32_t> sent_bytes_{0};
  std::atomic<uint32_t> dropped_bytes_{0};
  uint32_t last_report_{0};
  uint32_t reported_dropped_{0};
};

template<typename... Ts> class StartAction : public Action<Ts...> {
 public:
  explicit StartAction(MicTap *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->start(); }

 protected:
  MicTap *parent_;
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(MicTap *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->stop(); }

 protected:
  MicTap *parent_;
};

template<typename... Ts> class MarkAction : public Action<Ts...> {
 public:
  explicit MarkAction(MicTap *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, label)
  void play(const Ts &...x) override { this->parent_->mark(this->label_.value(x...)); }

 protected:
  MicTap *parent_;
};

template<typename... Ts> class IsStreamingCondition : public Condition<Ts...> {
 public:
  explicit IsStreamingCondition(MicTap *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->is_streaming(); }

 protected:
  MicTap *parent_;
};

}  // namespace mic_tap
}  // namespace esphome
