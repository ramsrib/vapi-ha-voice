#include "mic_tap.h"

#include "esphome/core/log.h"

#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <cstring>

namespace esphome {
namespace mic_tap {

static const char *const TAG = "mic_tap";

static const uint8_t FRAME_PCM = 0;
static const uint8_t FRAME_LABEL = 1;

void MicTap::setup() {
  if (this->mic_ == nullptr) {
    this->mark_failed();
    return;
  }
  /* A passive source receives audio whenever the hardware microphone is running
   * and never starts or stops it, so registering this callback costs the wake
   * word nothing — it just never fires while the tap is idle. */
  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
}

void MicTap::start() {
  if (this->streaming_.load() || this->close_pending_.load()) {
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

  /* Announce the format rather than letting the receiver assume it. A silent
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

  // Non-blocking from here: this socket is written from the audio path, where a
  // block would stall the microphone and cost the wake word real audio.
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  this->sent_bytes_ = 0;
  this->dropped_bytes_ = 0;
  this->reported_dropped_ = 0;
  this->sock_.store(sock);
  this->streaming_.store(true);
  ESP_LOGI(TAG, "streaming %" PRIu32 " Hz %u-bit mono to %s:%u", rate, bits, this->host_.c_str(), this->port_);
}

void MicTap::stop() {
  if (!this->streaming_.load())
    return;
  this->streaming_.store(false);
  this->close_pending_.store(true);  // loop() closes once no send is in flight
  ESP_LOGI(TAG, "stopping: %" PRIu32 " bytes sent, %" PRIu32 " dropped", this->sent_bytes_.load(),
           this->dropped_bytes_.load());
}

void MicTap::mark(const std::string &label) {
  if (!this->streaming_.load())
    return;
  if (this->send_frame_(FRAME_LABEL, (const uint8_t *) label.data(), label.size())) {
    ESP_LOGD(TAG, "marked '%s'", label.c_str());
  }
}

bool MicTap::send_frame_(uint8_t type, const uint8_t *payload, size_t len) {
  if (len > 0xFFFF)
    return false;

  this->in_send_.fetch_add(1);
  int sock = this->sock_.load();
  bool ok = false;

  if (sock >= 0 && this->streaming_.load()) {
    uint8_t head[4] = {type, 0, (uint8_t) (len & 0xFF), (uint8_t) (len >> 8)};
    struct iovec iov[2];
    iov[0].iov_base = head;
    iov[0].iov_len = sizeof(head);
    iov[1].iov_base = (void *) payload;
    iov[1].iov_len = len;
    struct msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = len > 0 ? 2 : 1;

    /* Whole frames only. A partial write would desynchronise the framing for
     * the rest of the session, so a frame that does not fit is dropped entirely
     * and counted — the same lesson the speaker path taught, where ignoring a
     * short write was heard as distortion rather than as a gap. */
    int sent = ::sendmsg(sock, &msg, MSG_DONTWAIT);
    if (sent == (int) (sizeof(head) + len)) {
      this->sent_bytes_.fetch_add(len);
      ok = true;
    } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      this->dropped_bytes_.fetch_add(len);
    } else {
      this->dropped_bytes_.fetch_add(len);
      this->close_pending_.store(true);
      this->streaming_.store(false);
    }
  }

  this->in_send_.fetch_sub(1);
  return ok;
}

void MicTap::on_mic_data_(const std::vector<uint8_t> &data) {
  if (!this->streaming_.load() || data.empty())
    return;
  // Chunks arrive well under the 64 KB frame limit; split defensively anyway.
  size_t offset = 0;
  while (offset < data.size()) {
    size_t chunk = std::min<size_t>(data.size() - offset, 0xF000);
    if (!this->send_frame_(FRAME_PCM, data.data() + offset, chunk))
      break;
    offset += chunk;
  }
}

void MicTap::loop() {
  if (this->close_pending_.load() && this->in_send_.load() == 0) {
    int sock = this->sock_.exchange(-1);
    if (sock >= 0)
      ::close(sock);
    this->close_pending_.store(false);
    ESP_LOGI(TAG, "stream closed");
    return;
  }

  if (!this->streaming_.load())
    return;

  uint32_t now = millis();
  if (now - this->last_report_ >= 5000) {
    this->last_report_ = now;
    uint32_t dropped = this->dropped_bytes_.load();
    if (dropped != this->reported_dropped_) {
      /* Dropping means the network could not keep up with 32 KB/s, which leaves
       * gaps in the recording. Worth knowing before training on it. */
      ESP_LOGW(TAG, "dropped %" PRIu32 " bytes (%" PRIu32 " total) — recording has gaps",
               dropped - this->reported_dropped_, dropped);
      this->reported_dropped_ = dropped;
    } else {
      ESP_LOGD(TAG, "streaming, %" PRIu32 " KB sent", this->sent_bytes_.load() / 1024);
    }
  }
}

void MicTap::dump_config() {
  ESP_LOGCONFIG(TAG, "Microphone Tap:");
  ESP_LOGCONFIG(TAG, "  Destination: %s:%u", this->host_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Streaming: %s", YESNO(this->streaming_.load()));
}

}  // namespace mic_tap
}  // namespace esphome
