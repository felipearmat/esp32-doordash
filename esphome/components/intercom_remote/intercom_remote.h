#pragma once
#include "esphome/core/component.h"
#include "esphome/components/output/binary_output.h"
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

namespace esphome {
namespace intercom_remote {

// Network frame: must match what the gateway (server.py) expects.
#pragma pack(push, 1)
struct Header {
  char magic[2];   // "IC"
  char endpoint;   // 'A', 'B', 'C', ...
  uint8_t version;
  uint16_t seq;
  uint16_t samples;
};
#pragma pack(pop)

// IMPORTANT: this component is deliberately "dumb". It does NOT decide when
// to talk (no VOX), does NOT cancel echo (no NLMS/AEC), and has NO call
// logic. It only does three things:
//   1. Reads the button pin (done by ESPHome's binary_sensor, outside this
//      component) and lets HA decide what that means.
//   2. While session_active_ == true: mic -> UDP and UDP -> DAC, continuously.
//   3. Plays a local "ding" on command, even outside of a session.
// All the intelligence (when to open a session, who to route to, echo/noise
// cancellation) lives on the central gateway (HA). See the project README.
class IntercomRemote : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_microphone_pin(int pin) { mic_pin_ = pin; }
  void set_i2s_bclk_pin(int pin) { i2s_bclk_pin_ = pin; }
  void set_i2s_lrc_pin(int pin) { i2s_lrc_pin_ = pin; }
  void set_i2s_din_pin(int pin) { i2s_din_pin_ = pin; }
  void set_endpoint(const std::string &v) { endpoint_ = v; }
  void set_gateway_host(const std::string &v) { gateway_host_ = v; }
  void set_gateway_port(uint16_t v) { gateway_port_ = v; }
  void set_sample_rate(uint32_t v) { sample_rate_ = v; }
  void set_frame_ms(uint32_t v) { frame_ms_ = v; }
  void set_mic_bias_mv(int v) { mic_bias_mv_ = v; }
  void set_input_gain(float v) { input_gain_ = v; }
  void set_output_gain(float v) { output_gain_ = v; }
  void set_amp_enable(output::BinaryOutput *amp) { amp_enable_ = amp; }
  void set_amp_warmup_ms(uint32_t v) { amp_warmup_ms_ = v; }
  void set_amp_idle_timeout_ms(uint32_t v) { amp_idle_timeout_ms_ = v; }

  // The only audio gate that exists in this component: turns capture AND
  // playback on/off together. Only the "Audio Active" switch in the YAML
  // calls this, which in turn is only triggered by HA's automations/scripts.
  void set_session_active(bool on);
  bool is_session_active() const { return session_active_; }

  // Plays a two-tone "ding" without blocking the rest of the device, even
  // while the session is closed — used for the local physical chime.
  void ring();

 protected:
  // ---- Configuration (set in setup() from the YAML) ----
  int mic_pin_{34};
  int i2s_bclk_pin_{26};
  int i2s_lrc_pin_{25};
  int i2s_din_pin_{33};
  std::string endpoint_{"A"};
  std::string gateway_host_;
  uint16_t gateway_port_{6055};
  uint32_t sample_rate_{8000};
  uint32_t frame_ms_{20};
  int mic_bias_mv_{1650};
  float input_gain_{3.0f};
  float output_gain_{0.7f};
  uint32_t amp_warmup_ms_{10};
  uint32_t amp_idle_timeout_ms_{400};

  // ---- Networking ----
  WiFiUDP udp_tx_;
  WiFiUDP udp_rx_;
  IPAddress gateway_ip_;
  uint16_t local_rx_port_{0};
  uint16_t seq_{0};

  // ---- Amplifier (physical power-gate, not "processing") ----
  output::BinaryOutput *amp_enable_{nullptr};
  volatile bool amp_on_{false};
  volatile uint32_t last_rx_sample_ms_{0};

  // ---- Call session (external gate, 100% controlled by HA) ----
  volatile bool session_active_{false};

  // ---- Ring buffers (producer/consumer via FreeRTOS) ----
  RingbufHandle_t rx_ring_{nullptr};  // network -> playback (int16 samples)
  RingbufHandle_t tx_ring_{nullptr};  // capture -> network (int16 samples)

  TaskHandle_t capture_task_handle_{nullptr};
  TaskHandle_t playback_task_handle_{nullptr};
  TaskHandle_t network_task_handle_{nullptr};

  static void capture_task_trampoline(void *arg);
  static void playback_task_trampoline(void *arg);
  static void network_task_trampoline(void *arg);
  void capture_task_();
  void playback_task_();
  void network_task_();

  void ensure_amp_on_();
  void maybe_amp_off_();
};

}  // namespace intercom_remote
}  // namespace esphome
