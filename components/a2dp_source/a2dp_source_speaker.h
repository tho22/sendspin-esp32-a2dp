#pragma once

#ifdef USE_ESP32

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>
#include <esp_gap_bt_api.h>

#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome::a2dp_source {

class A2DPDeviceSelect;

enum class LinkState : uint8_t {
  IDLE = 0,     // Not connected, waiting for the next connection attempt
  DISCOVERING,  // Inquiry scan running to find the sink by name
  CONNECTING,   // esp_a2d_source_connect() issued
  CONNECTED,    // A2DP signalling channel established, media stream suspended
};

enum class MediaState : uint8_t {
  SUSPENDED = 0,
  STARTING,  // ESP_A2D_MEDIA_CTRL_START sent, waiting for the sink to start the stream
  STARTED,   // Stack pulls PCM through the data callback
  SUSPENDING,
};

/// Speaker that encodes its audio as SBC and streams it to a Bluetooth Classic A2DP sink.
///
/// Audio written with play() goes into a ring buffer. The Bluedroid media task pulls PCM from it through the
/// A2DP data callback, which also reports the played frames through the speaker's audio output callback so that
/// timing-aware sources (e.g. Sendspin) can keep the stream synchronized.
class A2DPSourceSpeaker : public speaker::Speaker, public Component {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_address(uint64_t address);
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_local_name(const std::string &name) { this->local_name_ = name; }
  void set_pin_code(const std::string &pin) { this->pin_code_ = pin; }
  void set_buffer_duration(uint32_t ms) { this->buffer_duration_ms_ = ms; }
  void set_reconnect_interval(uint32_t ms) { this->reconnect_interval_ms_ = ms; }
  void set_keep_alive(bool keep_alive) { this->keep_alive_ = keep_alive; }
  void set_test_tone(bool test_tone) { this->test_tone_ = test_tone; }
  void set_device_select(A2DPDeviceSelect *device_select) { this->device_select_ = device_select; }

  // Speaker interface
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;
  void set_pause_state(bool pause_state) override { this->pause_state_ = pause_state; }
  bool get_pause_state() const override { return this->pause_state_; }
  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

  bool is_connected() const { return this->link_state_.load() == LinkState::CONNECTED; }
  /// Human readable link/pairing state, e.g. for a text sensor (main loop only)
  const std::string &get_status_text() const { return this->status_text_; }
  /// Drops the pairing and the stored address and searches for the speaker by name (main loop only)
  void repair();
  /// Searches for speakers ready to pair (disconnects the current one for the search); the result goes to the device
  /// select (main loop only)
  void scan();
  /// Pairs the speaker at `index` of the last scan result, dropping the old pairing (main loop only)
  void pair_found_device(size_t index);
  /// Disconnects, forgets the speaker and stops connecting until the next pairing (main loop only)
  void unpair();

  // Speaker buttons (AVRCP), fired from the main loop
  Trigger<> *get_play_pause_trigger() { return &this->play_pause_trigger_; }
  Trigger<> *get_stop_trigger() { return &this->stop_trigger_; }
  Trigger<> *get_next_trigger() { return &this->next_trigger_; }
  Trigger<> *get_previous_trigger() { return &this->previous_trigger_; }
  /// Volume (0..1) changed on the speaker
  Trigger<float> *get_volume_trigger() { return &this->volume_trigger_; }

 protected:
  // Bluedroid callbacks, called from the BTC task
  static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  static int32_t data_callback(uint8_t *data, int32_t len);
  static void report_task(void *params);
  static void avrc_ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
  static void avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
  // Event handlers, BTC task
  void handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  void handle_a2dp_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  void handle_connection_state_(esp_a2d_connection_state_t state, esp_bd_addr_t remote_bda);
  void handle_avrc_ct_event_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
  void handle_avrc_tg_event_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
  void handle_speaker_button_(uint8_t key);
  void add_scan_device_(const uint8_t *address, const char *name);

  // Audio path, data callback (BTC task)
  int32_t fill_audio_(uint8_t *data, int32_t len);
  void advance_playback_clock_(uint32_t frames);
  void fill_gap_(uint8_t *data, size_t length);
  void apply_software_volume_(uint8_t *data, size_t length);

  // Main loop
  void manage_idle_link_(uint32_t now);
  void manage_discovery_(uint32_t now);
  void manage_connecting_link_(uint32_t now);
  void manage_connected_link_(uint32_t now);
  void handle_remote_commands_(uint32_t now);
  void log_diagnostics_(uint32_t now);
  void log_task_load_();
  void update_status_text_();
  void apply_volume_setting_();
  void drain_ring_buffer_();

  bool init_bluetooth_();
  void restart_bluetooth_(const char *reason);
  void recover_from_stall_(const char *reason);
  void connect_();
  // Quick retries right after a lost connection, otherwise the configured interval
  uint32_t reconnect_interval_() const;
  bool start_discovery_();
  void start_scan_();
  void finish_scan_();
  void request_media_(esp_a2d_media_ctrl_t ctrl);
  void reset_media_state_();

  // Pairing and persisted state
  void forget_speaker_();
  void set_pairing_enabled_(bool enabled);
  void save_address_();
  void clear_saved_address_();
  void set_remote_name_(const std::string &name);
  /// Copies the configured PIN into `pin_code` and returns its length
  uint8_t copy_pin_code_(esp_bt_pin_code_t pin_code) const;

  static A2DPSourceSpeaker *instance_;

  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;

  esp_bd_addr_t remote_address_{};
  esp_bd_addr_t connected_address_{};       // Peer of the current link (BTC task, published by link_state_)
  std::atomic<bool> address_chosen_{false};  // remote_address_ chosen in the web interface, do not adopt another peer
  bool has_address_{false};
  bool address_configured_{false};  // From YAML; a discovered address is persisted in address_pref_ instead
  ESPPreferenceObject address_pref_;
  bool pairing_enabled_{true};  // false after unpair(): no discovery, no connects
  ESPPreferenceObject pairing_enabled_pref_;
  bool forget_pending_{false};  // unpair(): drop the bonds once the link is down
  uint32_t leave_link_ms_{0};   // Disconnect requested for unpair()/scan(), 0 = none
  bool scan_requested_{false};
  std::atomic<bool> scan_only_{false};  // The running discovery collects speakers instead of pairing by name

  struct FoundDevice {
    esp_bd_addr_t address;
    std::string name;
  };
  std::vector<FoundDevice> scan_devices_;   // Written by the BTC task during a scan, guarded by remote_name_mutex_
  std::vector<FoundDevice> found_devices_;  // Result of the last scan, offered by device_select_ (main loop)
  A2DPDeviceSelect *device_select_{nullptr};
  std::string status_text_;
  std::string remote_name_;  // Written by the BTC task
  std::mutex remote_name_mutex_;
  std::string device_name_;
  std::string local_name_;
  std::string pin_code_;
  uint32_t buffer_duration_ms_{500};
  uint32_t reconnect_interval_ms_{15000};
  bool keep_alive_{true};
  bool test_tone_{false};
  size_t test_tone_phase_{0};

  std::atomic<LinkState> link_state_{LinkState::IDLE};
  std::atomic<MediaState> media_state_{MediaState::SUSPENDED};
  std::atomic<bool> media_ctrl_pending_{false};
  std::atomic<bool> device_found_{false};
  std::atomic<bool> discovery_stopped_{false};
  std::atomic<bool> pause_state_{false};
  std::atomic<uint32_t> failed_connects_{0};
  std::atomic<uint32_t> rejected_connects_{0};  // The link came up but the speaker refused A2DP
  std::atomic<bool> acl_up_{false};             // A baseband link to the speaker exists for the current attempt
  std::atomic<bool> address_dirty_{false};  // remote_address_ changed by the BTC task, persist in loop()
  std::atomic<bool> streaming_{false};  // Data callback may consume the ring buffer
  std::atomic<int32_t> q31_volume_factor_{INT32_MAX};

  // AVRCP: the speaker's buttons and absolute volume
  enum class RemoteCommand : uint8_t { NONE, PLAY_PAUSE, STOP, NEXT, PREVIOUS };
  std::atomic<uint8_t> volume_percent_{100};
  std::atomic<int8_t> abs_volume_{-1};  // Speaker supports absolute volume: -1 unknown, 0 no, 1 yes
  std::atomic<bool> avrc_ct_connected_{false};
  esp_avrc_rn_evt_cap_mask_t peer_rn_cap_{};
  uint32_t ct_connected_ms_{0};
  uint32_t tg_connected_ms_{0};
  std::atomic<RemoteCommand> pending_command_{RemoteCommand::NONE};
  std::atomic<int> pending_volume_{-1};
  Trigger<> play_pause_trigger_;
  Trigger<> stop_trigger_;
  Trigger<> next_trigger_;
  Trigger<> previous_trigger_;
  Trigger<float> volume_trigger_;
  std::atomic<uint32_t> underrun_bytes_{0};

  // Smoothed playback clock, only touched by the data callback (BTC task)
  int64_t clock_anchor_us_{0};
  int64_t clock_frames_{0};
  bool clock_valid_{false};
  std::atomic<uint32_t> clock_resyncs_{0};
  std::atomic<uint32_t> pulled_frames_{0};
  // Playback progress handed from the data callback to report_task()
  portMUX_TYPE report_lock_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t report_frames_{0};
  int64_t report_timestamp_{0};

  // Data callback profiling
  std::atomic<uint32_t> cb_calls_{0};
  std::atomic<uint32_t> cb_time_us_{0};
  std::atomic<uint32_t> cb_max_us_{0};
  uint32_t last_stats_log_ms_{0};
  uint32_t last_load_log_ms_{0};

  std::atomic<bool> finishing_{false};
  bool attempted_once_{false};
  uint32_t last_attempt_ms_{0};
  std::atomic<uint8_t> quick_reconnects_{0};  // Quick retries left after a lost connection (set by the BTC task)
  uint32_t play_pause_due_ms_{0};  // Pending play/pause, fired only if still connected (main loop)
  uint32_t connect_started_ms_{0};
  uint32_t last_media_ctrl_ms_{0};
  uint32_t last_underrun_log_ms_{0};
  std::atomic<uint32_t> last_pull_ms_{0};  // Last data callback (BTC task), for the stream watchdog
  uint32_t stall_disconnect_ms_{0};          // Disconnect requested because of a stall, 0 = none
  std::atomic<uint32_t> stream_started_ms_{0};  // last_pull_ms_ when the stream started (BTC task)
};

}  // namespace esphome::a2dp_source

#endif  // USE_ESP32
