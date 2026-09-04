#include "intercom_remote.h"
#include "esphome/core/log.h"
#include <WiFi.h>
#include <math.h>
#include <esp_timer.h>

// ---------------------------------------------------------------------------
// This component is a deliberately "dumb terminal": there is NO VOX, NLMS,
// or AEC here. While the session is active, the module transmits and
// receives PCM in full duplex, without deciding anything about the audio
// content. Any echo/noise cancellation, silence detection, or "who's
// talking now" logic is the responsibility of the central gateway — see
// homeassistant/addons/intercom_gateway/server.py.
//
// This design exists because doing this well on a classic ESP32 always
// means giving something up (VOX = switched half-duplex; NLMS = audible
// residual echo; hardware AEC = S3-only and still incomplete). A server
// with real CPU solves this far more robustly (e.g. the WebRTC Audio
// Processing Module), so it doesn't make sense to spend microcontroller
// cycles trying to approximate that.
// ---------------------------------------------------------------------------

namespace esphome {
namespace intercom_remote {

static const char *TAG = "intercom_remote";

// Arduino-ESP32 runs the WiFi/lwIP stack's own tasks on core 0 (PRO_CPU);
// the main Arduino loop() runs on core 1 (APP_CPU). Capture/playback need
// tight, jitter-free sample timing, so they're kept on core 1, away from
// WiFi-driven interrupts and tasks. The network task talks to lwIP sockets
// directly and tolerates more jitter, so it stays on core 0 next to that
// stack instead of competing with capture/playback for core 1 time.
static constexpr BaseType_t AUDIO_TIMING_CORE = 1;
static constexpr BaseType_t NETWORK_CORE = 0;
static constexpr size_t RING_BYTES = 8 * 1024;
static constexpr size_t CAPTURE_FRAME_MAX = 480;
static constexpr UBaseType_t TASK_PRIO_CAPTURE = 6;
static constexpr UBaseType_t TASK_PRIO_PLAYBACK = 6;
static constexpr UBaseType_t TASK_PRIO_NETWORK = 5;

void IntercomRemote::setup() {
  analogReadResolution(12);
  analogSetPinAttenuation(mic_pin_, ADC_11db);
  pinMode(dac_pin_, OUTPUT);
  dacWrite(dac_pin_, 128);  // rest at mid-scale (silence)

  if (amp_enable_ != nullptr) {
    amp_enable_->turn_off();
  }

  // Port scheme: A=6056, B=6057, C=6058, D=6059... derived directly from
  // the endpoint letter, which allows any number of internal modules
  // without hardcoding each one. The gateway uses the same formula (see
  // port_for() in server.py) to know where to send audio back.
  local_rx_port_ = 6056 + (endpoint_[0] - 'A');
  udp_rx_.begin(local_rx_port_);
  WiFi.hostByName(gateway_host_.c_str(), gateway_ip_);

  rx_ring_ = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_BYTEBUF);
  tx_ring_ = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_BYTEBUF);
  if (rx_ring_ == nullptr || tx_ring_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate audio ring buffers");
    this->mark_failed();
    return;
  }

  xTaskCreatePinnedToCore(playback_task_trampoline, "aud_play", 4096, this,
                           TASK_PRIO_PLAYBACK, &playback_task_handle_, AUDIO_TIMING_CORE);
  xTaskCreatePinnedToCore(capture_task_trampoline, "aud_cap", 4096, this,
                           TASK_PRIO_CAPTURE, &capture_task_handle_, AUDIO_TIMING_CORE);
  xTaskCreatePinnedToCore(network_task_trampoline, "aud_net", 4096, this,
                           TASK_PRIO_NETWORK, &network_task_handle_, NETWORK_CORE);
}

void IntercomRemote::dump_config() {
  ESP_LOGCONFIG(TAG, "Intercom Remote (endpoint %s) — dumb terminal, no local processing", endpoint_.c_str());
  ESP_LOGCONFIG(TAG, "  Sample rate: %u Hz, frame %u ms", sample_rate_, frame_ms_);
  ESP_LOGCONFIG(TAG, "  Gateway: %s:%u  |  Local RX: %u", gateway_host_.c_str(),
                gateway_port_, local_rx_port_);
  ESP_LOGCONFIG(TAG, "  Mic: GPIO%d (bias %dmV, gain %.1f)", mic_pin_, mic_bias_mv_, input_gain_);
  ESP_LOGCONFIG(TAG, "  DAC: GPIO%d (output gain %.2f)", dac_pin_, output_gain_);
  if (amp_enable_ == nullptr) {
    ESP_LOGW(TAG, "  No amp_enable configured: amplifier will stay on permanently.");
  }
}

// ESPHome loop(): only light work (amplifier timeout), never processes
// audio sample by sample.
void IntercomRemote::loop() { maybe_amp_off_(); }

// Opens/closes the call session. This is the ONLY audio control this
// component exposes — the central gateway (HA) decides when to call this,
// never the module itself.
void IntercomRemote::set_session_active(bool on) {
  session_active_ = on;
  if (!on) {
    dacWrite(dac_pin_, 128);
    // rx_ring_ is only created in setup(), which runs at AFTER_WIFI priority
    // — later than the "Audio Active" switch's own setup(). That switch's
    // restore_mode: ALWAYS_OFF calls this at boot to enforce the initial
    // state, before rx_ring_ exists, so it must be guarded here.
    if (rx_ring_ != nullptr) {
      void *stale;
      size_t sz;
      while ((stale = xRingbufferReceive(rx_ring_, &sz, 0)) != nullptr) {
        vRingbufferReturnItem(rx_ring_, stale);
      }
    }
  }
}

// Generates a simple "ding-dong" and injects it into the playback ring
// buffer, even outside of a session — the local chime doesn't depend on
// call state.
void IntercomRemote::ring() {
  // The chime shares rx_ring_ with network-sourced call audio. Playing it
  // while a session is active would interleave a "ding" into a live
  // conversation, so skip it rather than corrupt the call audio.
  if (session_active_) {
    ESP_LOGW(TAG, "ring() ignored: audio session is active");
    return;
  }

  static constexpr float FREQS[] = {880.0f, 660.0f};
  static constexpr uint32_t TONE_MS = 220;

  for (float freq : FREQS) {
    uint32_t n = (sample_rate_ * TONE_MS) / 1000;
    int16_t chunk[64];
    size_t pos = 0;
    for (uint32_t i = 0; i < n; i++) {
      float t = (float) i / sample_rate_;
      float env = 1.0f;
      if (i < sample_rate_ / 100) env = (float) i / (sample_rate_ / 100);
      uint32_t remaining = n - i;
      if (remaining < sample_rate_ / 100) env = (float) remaining / (sample_rate_ / 100);

      chunk[pos++] = (int16_t) (env * 6000.0f * sinf(2.0f * (float) M_PI * freq * t));
      if (pos == 64 || i == n - 1) {
        xRingbufferSend(rx_ring_, chunk, pos * sizeof(int16_t), pdMS_TO_TICKS(50));
        pos = 0;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Capture: mic -> tx_ring_. Full duplex whenever the session is open — no
// "talk or not" decision happens here. This is pure real-time sampling
// work, with no content analysis.
// ---------------------------------------------------------------------------
void IntercomRemote::capture_task_trampoline(void *arg) {
  static_cast<IntercomRemote *>(arg)->capture_task_();
}

void IntercomRemote::capture_task_() {
  const uint32_t period_us = 1000000UL / sample_rate_;
  const size_t frame_samples = (sample_rate_ * frame_ms_) / 1000;
  int16_t frame[CAPTURE_FRAME_MAX];

  for (;;) {
    if (!session_active_) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    size_t pos = 0;
    uint64_t next = esp_timer_get_time();
    while (pos < frame_samples && pos < CAPTURE_FRAME_MAX) {
      int raw = analogRead(mic_pin_);
      float mv = (raw / 4095.0f) * 3300.0f;
      float centered = (mv - mic_bias_mv_) * input_gain_;
      if (centered > 32767.f) centered = 32767.f;
      if (centered < -32768.f) centered = -32768.f;
      frame[pos++] = (int16_t) centered;

      next += period_us;
      int64_t wait = (int64_t) next - (int64_t) esp_timer_get_time();
      if (wait > 0) {
        if (wait > 1000) {
          vTaskDelay(pdMS_TO_TICKS(wait / 1000));
        } else {
          ets_delay_us((uint32_t) wait);
        }
      }
    }

    if (!session_active_) continue;  // session may have closed mid-frame

    // No VOX, no NLMS: the whole frame goes to the network. The central
    // gateway decides what to do with it (mix, apply AEC, route).
    BaseType_t ok = xRingbufferSend(tx_ring_, frame, pos * sizeof(int16_t), pdMS_TO_TICKS(20));
    if (ok != pdTRUE) {
      ESP_LOGW(TAG, "tx_ring full, dropping capture frame");
    }
  }
}

// ---------------------------------------------------------------------------
// Playback: rx_ring_ -> DAC. Plays back whatever the gateway sends, unfiltered.
// ---------------------------------------------------------------------------
void IntercomRemote::playback_task_trampoline(void *arg) {
  static_cast<IntercomRemote *>(arg)->playback_task_();
}

void IntercomRemote::playback_task_() {
  const uint32_t period_us = 1000000UL / sample_rate_;

  for (;;) {
    size_t item_size = 0;
    void *item = xRingbufferReceive(rx_ring_, &item_size, pdMS_TO_TICKS(200));
    if (item == nullptr) continue;

    ensure_amp_on_();
    last_rx_sample_ms_ = millis();

    const int16_t *samples = static_cast<const int16_t *>(item);
    size_t count = item_size / sizeof(int16_t);
    uint64_t next = esp_timer_get_time();

    for (size_t i = 0; i < count; i++) {
      int v = 128 + (int) ((samples[i] / 256.0f) * output_gain_);
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      dacWrite(dac_pin_, v);

      next += period_us;
      int64_t wait = (int64_t) next - (int64_t) esp_timer_get_time();
      if (wait > 0) ets_delay_us((uint32_t) wait);

      if ((i & 0x3F) == 0) taskYIELD();
    }

    vRingbufferReturnItem(rx_ring_, item);
  }
}

// ---------------------------------------------------------------------------
// Networking: sole owner of the UDP sockets. Frame format must match what
// the gateway expects.
// ---------------------------------------------------------------------------
void IntercomRemote::network_task_trampoline(void *arg) {
  static_cast<IntercomRemote *>(arg)->network_task_();
}

void IntercomRemote::network_task_() {
  uint8_t rx_buf[1400];

  for (;;) {
    bool did_work = false;

    size_t item_size = 0;
    void *item = xRingbufferReceive(tx_ring_, &item_size, 0);
    if (item != nullptr) {
      did_work = true;
      Header h{{'I', 'C'}, endpoint_[0], 1, seq_++, (uint16_t) (item_size / 2)};
      udp_tx_.beginPacket(gateway_ip_, gateway_port_);
      udp_tx_.write((uint8_t *) &h, sizeof(h));
      udp_tx_.write((uint8_t *) item, item_size);
      udp_tx_.endPacket();
      vRingbufferReturnItem(tx_ring_, item);
    }

    int psize = udp_rx_.parsePacket();
    if (psize > 0) {
      did_work = true;
      int n = udp_rx_.read(rx_buf, sizeof(rx_buf));
      if (n >= 2) {
        // The only filter that exists here: the session must be open.
        // Without this, the gateway could never accidentally play audio on
        // a module outside a call — but within the session, everything
        // that arrives is played, with no local half-duplex switching.
        if (session_active_) {
          if (xRingbufferSend(rx_ring_, rx_buf, n - (n % 2), pdMS_TO_TICKS(20)) != pdTRUE) {
            ESP_LOGW(TAG, "rx_ring full, audio packet dropped");
          }
        }
      }
    }

    if (!did_work) vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ---------------------------------------------------------------------------
// Amplifier power-gate (physical switching, not "processing").
// ---------------------------------------------------------------------------
void IntercomRemote::ensure_amp_on_() {
  if (amp_enable_ == nullptr || amp_on_) return;
  amp_enable_->turn_on();
  amp_on_ = true;
  vTaskDelay(pdMS_TO_TICKS(amp_warmup_ms_));
}

void IntercomRemote::maybe_amp_off_() {
  if (amp_enable_ == nullptr || !amp_on_) return;
  if (millis() - last_rx_sample_ms_ > amp_idle_timeout_ms_) {
    amp_enable_->turn_off();
    amp_on_ = false;
    dacWrite(dac_pin_, 128);
  }
}

}  // namespace intercom_remote
}  // namespace esphome
