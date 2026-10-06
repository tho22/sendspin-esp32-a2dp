#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>
#include <esp_gap_bt_api.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

/// Streams 44.1 kHz 16 bit stereo PCM to a Bluetooth Classic A2DP sink.
///
/// Audio written with write() goes into a ring buffer in internal RAM. The Bluedroid media task pulls it through the
/// A2DP data callback. Playback progress (frames, timestamp) is reported from a separate task through the progress
/// callback, so the time-critical Bluetooth task never runs listener code.
class A2dpOutput {
 public:
  static constexpr uint32_t SAMPLE_RATE = 44100;
  static constexpr size_t BYTES_PER_FRAME = 4;

  struct Config {
    std::string local_name;   // Name of the ESP as a Bluetooth device
    std::string device_name;  // Sink to discover by name when no paired device is known
    std::string pin_code{"0000"};
    uint32_t buffer_ms{200};
    uint32_t reconnect_interval_ms{15000};
    bool keep_alive{true};  // Stream silence while idle so sinks that power off without audio stay on
    bool test_tone{false};  // Diagnostic: 441 Hz tone instead of idle silence
  };

  using ProgressCallback = std::function<void(uint32_t frames, int64_t timestamp_us)>;

  /// Commands from the speaker's buttons (AVRCP passthrough)
  enum class RemoteCommand : uint8_t { NONE, PLAY, PAUSE, STOP, NEXT, PREVIOUS };

  bool begin(const Config &config, ProgressCallback progress);

  /// Writes PCM, blocking up to @p timeout. Returns the bytes accepted. Without a connected sink the audio is
  /// discarded at real-time pace (and reported as played) so the source keeps running.
  size_t write(const uint8_t *data, size_t length, TickType_t timeout);

  /// Starts/stops consuming the ring buffer. While stopped the sink gets silence (keep alive).
  void start();
  void stop();

  /// Sets the playback volume. With AVRCP absolute volume the speaker applies it, otherwise it is digital gain.
  void set_volume(uint8_t volume_percent);
  void set_muted(bool muted);

  /// Fetches a pending button press and/or a volume change made on the speaker (volume < 0: none).
  /// Call from the thread that owns the consumer (e.g. the Sendspin client loop).
  void poll_remote(RemoteCommand &command, int &volume_percent);

  bool is_connected() const { return this->link_state_.load() == LinkState::CONNECTED; }

 protected:
  enum class LinkState : uint8_t { IDLE, DISCOVERING, CONNECTING, CONNECTED };
  enum class MediaState : uint8_t { SUSPENDED, STARTING, STARTED, SUSPENDING };

  static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  static void avrc_ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
  static void avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
  void handle_avrc_ct_event_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
  void handle_avrc_tg_event_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
  void apply_volume_setting_();
  static int32_t data_callback(uint8_t *data, int32_t len);
  static void control_task(void *params);
  static void report_task(void *params);

  void handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  void handle_a2dp_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  int32_t fill_audio_(uint8_t *data, int32_t len);

  bool init_bluetooth_();
  void restart_bluetooth_(const char *reason);
  void recover_from_stall_(const char *reason);
  void connect_();
  void request_media_(esp_a2d_media_ctrl_t ctrl);
  void control_loop_();
  void log_stats_(uint32_t elapsed_ms);
  void log_task_load_();
  void drain_ring_buffer_();
  size_t read_ring_buffer_(uint8_t *data, size_t length);
  size_t write_(const uint8_t *data, size_t length, TickType_t timeout);
  void apply_volume_(uint8_t *data, size_t length);
  void save_address_();
  bool load_address_();

  static A2dpOutput *instance_;

  Config config_;
  ProgressCallback progress_;
  RingbufHandle_t ring_buffer_{nullptr};
  size_t ring_buffer_size_{0};

  esp_bd_addr_t remote_address_{};
  bool has_address_{false};

  std::atomic<LinkState> link_state_{LinkState::IDLE};
  std::atomic<MediaState> media_state_{MediaState::SUSPENDED};
  std::atomic<bool> media_ctrl_pending_{false};
  std::atomic<bool> device_found_{false};
  std::atomic<bool> discovery_stopped_{false};
  std::atomic<bool> address_dirty_{false};
  std::atomic<bool> streaming_{false};
  std::atomic<int32_t> q15_volume_{32767};
  std::atomic<uint32_t> failed_connects_{0};
  std::atomic<uint32_t> rejected_connects_{0};  // The link came up but the speaker refused A2DP
  std::atomic<bool> acl_up_{false};             // A baseband link to the speaker exists for the current attempt

  std::atomic<uint8_t> volume_percent_{100};
  std::atomic<bool> muted_{false};

  // AVRCP: the speaker's buttons and absolute volume
  std::atomic<int8_t> abs_volume_{-1};  // Speaker supports absolute volume: -1 unknown, 0 no, 1 yes
  std::atomic<bool> avrc_ct_connected_{false};
  esp_avrc_rn_evt_cap_mask_t peer_rn_cap_{};
  uint8_t peer_volume_{0};  // 0..127
  uint32_t ct_connected_ms_{0};
  uint32_t tg_connected_ms_{0};
  std::atomic<RemoteCommand> pending_command_{RemoteCommand::NONE};
  std::atomic<int> pending_volume_{-1};

  // Control task state
  bool attempted_once_{false};
  uint32_t last_attempt_ms_{0};
  uint32_t connect_started_ms_{0};
  uint32_t last_media_ctrl_ms_{0};
  std::atomic<uint32_t> last_pull_ms_{0};  // Last data callback (BTC task), for the stream watchdog
  uint32_t stall_disconnect_ms_{0};          // Disconnect requested because of a stall, 0 = none
  uint32_t last_stall_ms_{0};

  // Smoothed playback clock, only touched by the data callback (BTC task)
  int64_t clock_anchor_us_{0};
  int64_t clock_frames_{0};
  bool clock_valid_{false};
  std::atomic<bool> clock_reset_{false};
  // Written by the BTC task, read and reset by the stats log (approximate is fine for diagnostics)
  int64_t clock_error_min_us_{INT64_MAX};
  int64_t clock_error_max_us_{INT64_MIN};
  size_t test_tone_phase_{0};

  // Playback progress handed from the data callback to report_task()
  portMUX_TYPE report_lock_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t report_frames_{0};
  int64_t report_timestamp_{0};

  // Statistics
  std::atomic<uint32_t> underrun_bytes_{0};
  std::atomic<uint32_t> pulled_frames_{0};
  std::atomic<uint32_t> offered_bytes_{0};
  std::atomic<int32_t> peak_level_{0};
  std::atomic<int32_t> input_peak_{0};
  std::atomic<uint32_t> accepted_bytes_{0};
  std::atomic<uint32_t> clock_resyncs_{0};
  std::atomic<uint32_t> cb_calls_{0};
  std::atomic<uint32_t> cb_time_us_{0};
  std::atomic<uint32_t> cb_max_us_{0};
};
