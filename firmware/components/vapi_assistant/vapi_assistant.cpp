#include "vapi_assistant.h"

#ifdef USE_ESP_IDF

#include <cstring>

#include "esphome/components/audio/audio.h"
#include "esphome/core/log.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"

namespace esphome {
namespace vapi_assistant {

static const char *const TAG = "vapi_assistant";

/* Vapi's reply to POST /call is small, but assistant config echoes back in it,
 * so leave room rather than truncating the JSON and failing to find the URL. */
static const size_t HTTP_RX_MAX = 8192;
static const size_t WS_RX_BUFFER = 4096;
static const int WS_SEND_TIMEOUT_MS = 200;

void VapiAssistant::setup() {
  this->error_mutex_ = xSemaphoreCreateMutex();

  this->frame_bytes_ = (this->sample_rate_ / 1000) * this->frame_ms_ * sizeof(int16_t);
  this->tx_buffer_.reserve(this->frame_bytes_ * 2);

  if (this->speaker_ != nullptr) {
    this->speaker_->set_volume(this->volume_);
    /* Vapi sends 16 kHz mono s16le. A `resampler` speaker downstream takes it
     * to the 48 kHz the hardware runs at, so nothing is resampled here. */
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, this->sample_rate_));
  }

  if (this->mic_ != nullptr) {
    this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
  }
}

void VapiAssistant::dump_config() {
  ESP_LOGCONFIG(TAG, "Vapi Assistant:");
  ESP_LOGCONFIG(TAG, "  API URL: %s", this->api_url_.c_str());
  if (this->assistant_id_.empty()) {
    ESP_LOGCONFIG(TAG, "  Assistant: transient (%s / %s, voice %s:%s)", this->model_provider_.c_str(),
                  this->model_.c_str(), this->voice_provider_.c_str(), this->voice_id_.c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Assistant ID: %s", this->assistant_id_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Audio: %" PRIu32 " Hz mono s16le, %u ms frames (%u bytes)", this->sample_rate_,
                this->frame_ms_, (unsigned) this->frame_bytes_);
  /* The API key is deliberately not logged: ESPHome logs go to the serial
   * console and over the API to anything subscribed. */
  ESP_LOGCONFIG(TAG, "  Volume: %.2f", this->volume_);
  if (this->voice_provider_ == "11labs") {
    ESP_LOGCONFIG(TAG, "  Delivery: %s stability=%.2f style=%.2f", this->voice_model_.c_str(), this->stability_,
                  this->style_);
  }
  ESP_LOGCONFIG(TAG, "  Silence timeout: %" PRIu32 " s (%" PRIu32 " s when started by wake word)",
                this->silence_timeout_, this->wake_silence_timeout_);
  ESP_LOGCONFIG(TAG, "  Max duration: %" PRIu32 " s", this->max_duration_);
  ESP_LOGCONFIG(TAG, "  Interrupt after: %u transcribed words", this->stop_speaking_num_words_);
  ESP_LOGCONFIG(TAG, "  End-call phrases: %u", (unsigned) this->end_call_phrases_.size());
  ESP_LOGCONFIG(TAG, "  API key: %s", this->api_key_.empty() ? "MISSING" : "set");
}

void VapiAssistant::start_call(bool from_wake_word) {
  /* The Watcher firmware once placed four billable calls in six seconds
   * because "is a call active" is false during the couple of seconds the REST
   * request takes. Claim the transition atomically instead of checking a flag
   * that is not yet true. */
  VapiState expected = VapiState::IDLE;
  if (!this->state_.compare_exchange_strong(expected, VapiState::STARTING)) {
    ESP_LOGW(TAG, "call already %s — ignoring start",
             expected == VapiState::STARTING ? "starting" : "running");
    return;
  }

  this->call_is_from_wake_word_ = from_wake_word;

  if (this->api_key_.empty()) {
    this->fail_("no API key configured");
    return;
  }

  /* 6 KB of stack: TLS handshake plus cJSON parsing of the response. */
  if (xTaskCreate(VapiAssistant::call_task_, "vapi_call", 6144, this, tskIDLE_PRIORITY + 5, nullptr) != pdPASS) {
    this->fail_("could not start call task");
  }
}

void VapiAssistant::call_task_(void *param) {
  static_cast<VapiAssistant *>(param)->run_call_task_();
  vTaskDelete(nullptr);
}

void VapiAssistant::run_call_task_() {
  std::string ws_url;
  if (!this->create_call_(ws_url)) {
    this->fail_("could not create Vapi call");
    return;
  }
  if (!this->open_websocket_(ws_url)) {
    this->fail_("could not open the Vapi websocket");
    return;
  }
  /* ACTIVE is set by the websocket CONNECTED event, not here — audio must not
   * be sent before the socket is actually up. */
}

bool VapiAssistant::create_call_(std::string &ws_url_out) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return false;
  }
  /* Either reference a saved assistant, or define one inline for this call. */
  cJSON *tuning = nullptr;
  if (!this->assistant_id_.empty()) {
    cJSON_AddStringToObject(root, "assistantId", this->assistant_id_.c_str());
    tuning = cJSON_AddObjectToObject(root, "assistantOverrides");
  } else {
    cJSON *a = cJSON_AddObjectToObject(root, "assistant");

    cJSON *model = cJSON_AddObjectToObject(a, "model");
    cJSON_AddStringToObject(model, "provider", this->model_provider_.c_str());
    cJSON_AddStringToObject(model, "model", this->model_.c_str());
    cJSON *messages = cJSON_AddArrayToObject(model, "messages");
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", this->system_prompt_.c_str());
    cJSON_AddItemToArray(messages, sys);

    cJSON *voice = cJSON_AddObjectToObject(a, "voice");
    cJSON_AddStringToObject(voice, "provider", this->voice_provider_.c_str());
    cJSON_AddStringToObject(voice, "voiceId", this->voice_id_.c_str());
    if (this->voice_provider_ == "11labs") {
      cJSON_AddStringToObject(voice, "model", this->voice_model_.c_str());
      cJSON_AddNumberToObject(voice, "stability", this->stability_);
      cJSON_AddNumberToObject(voice, "similarityBoost", this->similarity_boost_);
      cJSON_AddNumberToObject(voice, "style", this->style_);
      cJSON_AddBoolToObject(voice, "useSpeakerBoost", this->use_speaker_boost_);
    }

    cJSON *tr = cJSON_AddObjectToObject(a, "transcriber");
    cJSON_AddStringToObject(tr, "provider", this->transcriber_provider_.c_str());
    cJSON_AddStringToObject(tr, "model", this->transcriber_model_.c_str());

    /* On a transient assistant the tuning belongs in the assistant itself;
     * assistantOverrides is only meaningful against a saved one. */
    tuning = a;
  }

  cJSON *transport = cJSON_AddObjectToObject(root, "transport");
  cJSON_AddStringToObject(transport, "provider", "vapi.websocket");
  cJSON *fmt = cJSON_AddObjectToObject(transport, "audioFormat");
  cJSON_AddStringToObject(fmt, "format", "pcm_s16le");
  cJSON_AddStringToObject(fmt, "container", "raw");
  cJSON_AddNumberToObject(fmt, "sampleRate", this->sample_rate_);

  if (!this->first_message_.empty()) {
    cJSON_AddStringToObject(tuning, "firstMessage", this->first_message_.c_str());
  }
  const uint32_t silence =
      this->call_is_from_wake_word_ ? this->wake_silence_timeout_ : this->silence_timeout_;
  cJSON_AddNumberToObject(tuning, "silenceTimeoutSeconds", silence);
  cJSON_AddNumberToObject(tuning, "maxDurationSeconds", this->max_duration_);

  if (!this->end_call_phrases_.empty()) {
    cJSON_AddBoolToObject(tuning, "endCallFunctionEnabled", true);
    cJSON *phrases = cJSON_AddArrayToObject(tuning, "endCallPhrases");
    for (const auto &phrase : this->end_call_phrases_) {
      cJSON_AddItemToArray(phrases, cJSON_CreateString(phrase.c_str()));
    }
    if (!this->end_call_message_.empty()) {
      cJSON_AddStringToObject(tuning, "endCallMessage", this->end_call_message_.c_str());
    }
  }

  cJSON *stop_plan = cJSON_AddObjectToObject(tuning, "stopSpeakingPlan");
  cJSON_AddNumberToObject(stop_plan, "numWords", this->stop_speaking_num_words_);

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (body == nullptr) {
    return false;
  }

  std::string url = this->api_url_ + "/call";
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  cfg.buffer_size_tx = 2048;

  esp_http_client_handle_t http = esp_http_client_init(&cfg);
  if (http == nullptr) {
    cJSON_free(body);
    return false;
  }

  std::string auth = "Bearer " + this->api_key_;
  esp_http_client_set_header(http, "Authorization", auth.c_str());
  esp_http_client_set_header(http, "Content-Type", "application/json");

  bool ok = false;
  std::string response;
  const int body_len = (int) strlen(body);

  if (esp_http_client_open(http, body_len) == ESP_OK) {
    if (esp_http_client_write(http, body, body_len) == body_len) {
      esp_http_client_fetch_headers(http);
      const int status = esp_http_client_get_status_code(http);

      char chunk[512];
      int n;
      while ((n = esp_http_client_read(http, chunk, sizeof(chunk))) > 0) {
        response.append(chunk, n);
        if (response.size() > HTTP_RX_MAX) {
          break;
        }
      }

      if (status == 200 || status == 201) {
        ok = true;
      } else {
        ESP_LOGE(TAG, "POST /call returned %d: %s", status, response.c_str());
      }
    }
  }
  esp_http_client_close(http);
  esp_http_client_cleanup(http);
  cJSON_free(body);

  if (!ok) {
    return false;
  }

  cJSON *resp = cJSON_Parse(response.c_str());
  if (resp == nullptr) {
    ESP_LOGE(TAG, "could not parse the call response");
    return false;
  }
  cJSON *transport_obj = cJSON_GetObjectItemCaseSensitive(resp, "transport");
  cJSON *url_item =
      transport_obj ? cJSON_GetObjectItemCaseSensitive(transport_obj, "websocketCallUrl") : nullptr;
  if (cJSON_IsString(url_item) && url_item->valuestring != nullptr) {
    ws_url_out = url_item->valuestring;
  }
  cJSON_Delete(resp);

  if (ws_url_out.empty()) {
    ESP_LOGE(TAG, "no transport.websocketCallUrl in the response");
    return false;
  }
  return true;
}

bool VapiAssistant::open_websocket_(const std::string &ws_url) {
  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = ws_url.c_str();
  ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  ws_cfg.buffer_size = WS_RX_BUFFER;
  ws_cfg.task_stack = 8192;
  ws_cfg.reconnect_timeout_ms = 2000;
  ws_cfg.network_timeout_ms = 10000;
  /* Vapi ends a call by closing the socket; reconnecting would silently place
   * the device back into a call that the other end considers over. */
  ws_cfg.disable_auto_reconnect = true;

  this->ws_ = esp_websocket_client_init(&ws_cfg);
  if (this->ws_ == nullptr) {
    return false;
  }
  esp_websocket_register_events(this->ws_, WEBSOCKET_EVENT_ANY, VapiAssistant::ws_event_handler_, this);
  if (esp_websocket_client_start(this->ws_) != ESP_OK) {
    esp_websocket_client_destroy(this->ws_);
    this->ws_ = nullptr;
    return false;
  }
  return true;
}

void VapiAssistant::ws_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  static_cast<VapiAssistant *>(handler_args)->handle_ws_event_(event_id, event_data);
}

void VapiAssistant::handle_ws_event_(int32_t event_id, void *event_data) {
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "websocket connected");
      this->state_.store(VapiState::ACTIVE);
      this->pending_started_.store(true);
      break;

    case WEBSOCKET_EVENT_DATA:
      if (data->op_code == 0x02) {  // binary: PCM from the assistant
        if (this->speaker_ != nullptr && data->data_len > 0) {
          this->write_pcm_(reinterpret_cast<const uint8_t *>(data->data_ptr), data->data_len);
        }
      } else if (data->op_code == 0x01 && data->data_len > 0) {  // text: control channel
        ESP_LOGD(TAG, "control: %.*s", data->data_len, data->data_ptr);
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      ESP_LOGI(TAG, "websocket closed");
      if (this->state_.load() != VapiState::IDLE) {
        this->state_.store(VapiState::STOPPING);
        this->pending_ended_.store(true);
      }
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(TAG, "websocket error");
      break;

    default:
      break;
  }
}

void VapiAssistant::write_pcm_(const uint8_t *data, size_t len) {
  size_t written = 0;
  /* `play()` reports how much it actually took. Ignoring that return value
   * silently discards the rest whenever the buffer is momentarily full, which
   * is heard as distortion rather than as a clean gap — it was the cause of the
   * first working build sounding rough.
   *
   * The retry budget is bounded by wall clock rather than by attempts so a
   * speaker that has stopped draining can never stall the websocket task. */
  const int64_t deadline = esp_timer_get_time() + 250000;  // 250 ms
  while (written < len && esp_timer_get_time() < deadline) {
    written += this->speaker_->play(data + written, len - written, pdMS_TO_TICKS(30));
  }
  if (written < len) {
    this->dropped_bytes_.fetch_add(len - written);
  }
}

void VapiAssistant::on_mic_data_(const std::vector<uint8_t> &data) {
  if (this->state_.load() != VapiState::ACTIVE || this->ws_ == nullptr) {
    return;
  }
  this->tx_buffer_.insert(this->tx_buffer_.end(), data.begin(), data.end());

  /* Send whole frames only. Vapi accepts any chunk size, but fixed 20 ms frames
   * keep pacing predictable and match what the other two devices send. */
  while (this->tx_buffer_.size() >= this->frame_bytes_) {
    if (esp_websocket_client_send_bin(this->ws_, reinterpret_cast<const char *>(this->tx_buffer_.data()),
                                      this->frame_bytes_, pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS)) < 0) {
      ESP_LOGW(TAG, "websocket send failed");
      /* Drop the frame rather than growing without bound — stale microphone
       * audio is worse than none. */
    }
    this->tx_buffer_.erase(this->tx_buffer_.begin(), this->tx_buffer_.begin() + this->frame_bytes_);
  }
}

void VapiAssistant::stop_call() {
  const VapiState state = this->state_.load();
  if (state == VapiState::IDLE) {
    return;
  }
  this->state_.store(VapiState::STOPPING);
  this->pending_ended_.store(true);
}

void VapiAssistant::close_websocket_() {
  if (this->ws_ == nullptr) {
    return;
  }
  esp_websocket_client_close(this->ws_, pdMS_TO_TICKS(1000));
  esp_websocket_client_destroy(this->ws_);
  this->ws_ = nullptr;
}

void VapiAssistant::fail_(const std::string &reason) {
  ESP_LOGE(TAG, "%s", reason.c_str());
  if (xSemaphoreTake(this->error_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    this->pending_error_ = reason;
    xSemaphoreGive(this->error_mutex_);
  }
  this->state_.store(VapiState::STOPPING);
  this->pending_ended_.store(true);
}

void VapiAssistant::loop() {
  /* Everything below runs on the main task on purpose: the websocket and call
   * tasks must not fire ESPHome automations. */
  if (this->pending_started_.exchange(false)) {
    /* The microphone is not started here: the source is passive and
     * micro_wake_word keeps the hardware running. */
    if (this->speaker_ != nullptr) {
      this->speaker_->start();
    }
    this->on_start_.call();
  }

  /* Report dropped audio once a second while a call is up, so "it sounds bad"
   * can be attributed rather than guessed at. */
  if (this->state_.load() == VapiState::ACTIVE) {
    const uint32_t dropped = this->dropped_bytes_.load();
    if (dropped != this->last_reported_drops_) {
      static uint32_t last_log_ms = 0;
      const uint32_t now = millis();
      if (now - last_log_ms > 1000) {
        last_log_ms = now;
        ESP_LOGW(TAG, "speaker dropped %" PRIu32 " bytes total — buffers too small or network bursty",
                 dropped);
        this->last_reported_drops_ = dropped;
      }
    }
  }

  if (this->pending_ended_.exchange(false)) {
    if (this->speaker_ != nullptr) {
      this->speaker_->finish();
    }
    this->close_websocket_();
    this->tx_buffer_.clear();
    this->dropped_bytes_.store(0);
    this->last_reported_drops_ = 0;

    std::string error;
    if (xSemaphoreTake(this->error_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
      error.swap(this->pending_error_);
      xSemaphoreGive(this->error_mutex_);
    }
    this->state_.store(VapiState::IDLE);

    if (!error.empty()) {
      this->on_error_.call(error);
    }
    this->on_end_.call();
  }
}

}  // namespace vapi_assistant
}  // namespace esphome

#endif  // USE_ESP_IDF
