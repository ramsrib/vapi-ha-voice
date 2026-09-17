#include "mic_tap.h"

#include "esphome/core/log.h"

#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <esp_heap_caps.h>
#include <cstring>

namespace esphome {
namespace mic_tap {

static const char *const TAG = "mic_tap";

static const uint8_t FRAME_PCM = 0;
static const uint8_t FRAME_LABEL = 1;

static const size_t MAX_PAYLOAD = 1024;
/* Four seconds of 16 kHz mono. The network only has to keep up *on average*;
 * this absorbs the bursts, and a WiFi stall shorter than this is invisible in
 * the recording rather than fatal to it. */
static const size_t RING_SIZE = 128 * 1024;
/* Ceiling on how much one loop() iteration pushes, so draining a backlog never
 * starves the rest of the component's work. At 60 Hz this is far above the
 * 32 KB/s the microphone actually produces. */
static const size_t MAX_FLUSH_PER_LOOP = 8192;

void MicTap::setup() {
  if (this->mic_ == nullptr) {
    this->mark_failed();
    return;
  }
  this->ring_ = (uint8_t *) heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
  if (this->ring_ == nullptr)
    this->ring_ = (uint8_t *) malloc(RING_SIZE);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "cannot allocate %u byte buffer", (unsigned) RING_SIZE);
    this->mark_failed();
    return;
  }
  this->ring_size_ = RING_SIZE;

  /* A passive source receives audio whenever the hardware microphone is running
   * and never starts or stops it, so this costs the wake word nothing — the
   * callback simply does nothing while the tap is idle. */
  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
}

void MicTap::start() {
  if (this->streaming_.load()) {
    ESP_LOGW(TAG, "already streaming");
    return;
  }

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", this->port_);
  if (getaddrinfo(this->host_.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
    ESP_LOGE(TAG, "cannot resolve %s", this->host_.c_str());
    return;
  }

  int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    freeaddrinfo(res);
    return;
  }

  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (::connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
    ESP_LOGE(TAG, "connect to %s:%u failed: errno %d — is the receiver running?", this->host_.c_str(), this->port_,
             errno);
    ::close(sock);
    freeaddrinfo(res);
    return;
  }
  freeaddrinfo(res);

  int one = 1;
  /* Without this, lwip holds small segments back waiting for an ACK. The tap
   * emits ~60 small writes a second, so Nagle turns a steady trickle into a
   * backlog and the send buffer fills for no good reason. */
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  int sndbuf = 32768;
  setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  /* Announce the format rather than letting the receiver assume it: a silent
   * sample-rate mismatch produces audio that sounds fine and trains a model
   * that does not work. */
  auto info = this->mic_->get_audio_stream_info();
  uint8_t header[16];
  memcpy(header, "MICTAP01", 8);
  uint32_t rate = info.get_sample_rate();
  uint16_t channels = info.get_channels();
  uint16_t bits = info.get_bits_per_sample();
  memcpy(header + 8, &rate, 4);
  memcpy(header + 12, &channels, 2);
  memcpy(header + 14, &bits, 2);
  if (::send(sock, header, sizeof(header), 0) != (int) sizeof(header)) {
    ESP_LOGE(TAG, "header write failed: errno %d", errno);
    ::close(sock);
    return;
  }

  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  portENTER_CRITICAL(&this->ring_mux_);
  this->head_ = this->tail_ = this->fill_ = 0;
  portEXIT_CRITICAL(&this->ring_mux_);

  this->sent_bytes_ = 0;
  this->dropped_bytes_ = 0;
  this->reported_dropped_ = 0;
  this->sock_ = sock;
  this->streaming_.store(true);
  ESP_LOGI(TAG, "streaming %" PRIu32 " Hz %u-bit mono to %s:%u", rate, bits, this->host_.c_str(), this->port_);
}

void MicTap::close_socket_() {
  if (this->sock_ >= 0) {
    ::close(this->sock_);
    this->sock_ = -1;
  }
}

void MicTap::stop() {
  if (!this->streaming_.load())
    return;
  this->streaming_.store(false);  // audio stops queueing immediately
  this->flush_();                 // best effort: push whatever is still buffered
  this->close_socket_();
  ESP_LOGI(TAG, "stopping: %" PRIu32 " bytes sent, %" PRIu32 " dropped", this->sent_bytes_.load(),
           this->dropped_bytes_.load());
}

void MicTap::mark(const std::string &label) {
  if (!this->streaming_.load())
    return;
  if (this->queue_frame_(FRAME_LABEL, (const uint8_t *) label.data(), label.size()))
    ESP_LOGD(TAG, "marked '%s'", label.c_str());
}

bool MicTap::queue_frame_(uint8_t type, const uint8_t *payload, size_t len) {
  if (len > MAX_PAYLOAD || this->ring_ == nullptr)
    return false;
  const size_t total = 4 + len;
  const uint8_t head[4] = {type, 0, (uint8_t) (len & 0xFF), (uint8_t) (len >> 8)};

  portENTER_CRITICAL(&this->ring_mux_);
  if (this->ring_size_ - this->fill_ < total) {
    /* All-or-nothing. A partly written frame would desynchronise the framing
     * for the rest of the session, so an overrun drops the whole frame and the
     * stream stays valid — just missing that slice of audio. */
    portEXIT_CRITICAL(&this->ring_mux_);
    this->dropped_bytes_.fetch_add(len);
    return false;
  }
  for (size_t i = 0; i < 4; i++) {
    this->ring_[this->head_] = head[i];
    this->head_ = (this->head_ + 1) % this->ring_size_;
  }
  size_t first = std::min(len, this->ring_size_ - this->head_);
  memcpy(this->ring_ + this->head_, payload, first);
  if (len > first)
    memcpy(this->ring_, payload + first, len - first);
  this->head_ = (this->head_ + len) % this->ring_size_;
  this->fill_ += total;
  portEXIT_CRITICAL(&this->ring_mux_);
  return true;
}

void MicTap::on_mic_data_(const std::vector<uint8_t> &data) {
  if (!this->streaming_.load() || data.empty())
    return;
  size_t offset = 0;
  while (offset < data.size()) {
    size_t chunk = std::min<size_t>(data.size() - offset, MAX_PAYLOAD);
    this->queue_frame_(FRAME_PCM, data.data() + offset, chunk);
    offset += chunk;
  }
}

void MicTap::flush_() {
  if (this->sock_ < 0)
    return;
  size_t pushed = 0;
  while (pushed < MAX_FLUSH_PER_LOOP) {
    portENTER_CRITICAL(&this->ring_mux_);
    size_t fill = this->fill_, tail = this->tail_;
    portEXIT_CRITICAL(&this->ring_mux_);
    if (fill == 0)
      break;

    /* Send from the contiguous span only; the producer writes at head_ and
     * never into this region, so no lock is needed around the send itself. */
    size_t span = std::min(fill, this->ring_size_ - tail);
    span = std::min(span, MAX_FLUSH_PER_LOOP - pushed);
    int n = ::send(this->sock_, this->ring_ + tail, span, MSG_DONTWAIT);
    if (n > 0) {
      portENTER_CRITICAL(&this->ring_mux_);
      this->tail_ = (this->tail_ + (size_t) n) % this->ring_size_;
      this->fill_ -= (size_t) n;
      portEXIT_CRITICAL(&this->ring_mux_);
      this->sent_bytes_.fetch_add((uint32_t) n);
      pushed += (size_t) n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;  // socket full; the ring holds the backlog, try again next loop
    ESP_LOGE(TAG, "send failed: errno %d — ending stream", errno);
    this->streaming_.store(false);
    this->close_socket_();
    break;
  }
}

void MicTap::loop() {
  if (this->streaming_.load())
    this->flush_();
  else if (this->sock_ >= 0)
    this->close_socket_();

  if (!this->streaming_.load())
    return;

  uint32_t now = millis();
  if (now - this->last_report_ >= 5000) {
    this->last_report_ = now;
    uint32_t dropped = this->dropped_bytes_.load();
    portENTER_CRITICAL(&this->ring_mux_);
    size_t fill = this->fill_;
    portEXIT_CRITICAL(&this->ring_mux_);
    if (dropped != this->reported_dropped_) {
      ESP_LOGW(TAG, "dropped %" PRIu32 " bytes (%" PRIu32 " total) — recording has gaps",
               dropped - this->reported_dropped_, dropped);
      this->reported_dropped_ = dropped;
    } else {
      ESP_LOGD(TAG, "streaming, %" PRIu32 " KB sent, %u%% buffered", this->sent_bytes_.load() / 1024,
               (unsigned) (100 * fill / this->ring_size_));
    }
  }
}

void MicTap::dump_config() {
  ESP_LOGCONFIG(TAG, "Microphone Tap:");
  ESP_LOGCONFIG(TAG, "  Destination: %s:%u", this->host_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Buffer: %u KB", (unsigned) (RING_SIZE / 1024));
  ESP_LOGCONFIG(TAG, "  Streaming: %s", YESNO(this->streaming_.load()));
}

}  // namespace mic_tap
}  // namespace esphome
