#include "a2dp_output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <nvs.h>

static const char *const TAG = "a2dp";

static constexpr uint32_t MEDIA_CTRL_RETRY_MS = 1000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
static constexpr uint32_t MAX_FAILED_CONNECTS = 4;    // Then restart the Bluetooth stack
static constexpr uint32_t MAX_REJECTED_CONNECTS = 2;  // Then drop the stored pairing and pair again
static constexpr uint8_t DISCOVERY_DURATION = 10;   // x 1.28 s
static constexpr uint32_t DISCARD_CHUNK_MS = 20;    // Granularity of real-time discarding while disconnected

// Sendspin hard-syncs at 5 ms error, so the reported playback clock must be steady (see fill_audio_())
static constexpr int64_t CLOCK_RESYNC_US = 2000000;

static constexpr uint32_t CONTROL_INTERVAL_MS = 50;

// AVRCP: some speakers (e.g. Sony SRS-XB100) send play and their own volume right after connecting; ignore that
static constexpr uint32_t AVRC_CONNECT_GRACE_MS = 3000;
static constexpr uint8_t AVRC_VOLUME_STEP = 5;  // % per volume button press without absolute volume
static constexpr uint8_t AVRC_TL_GET_CAPS = 0;
static constexpr uint8_t AVRC_TL_VOLUME = 1;

static uint8_t percent_to_avrc(uint8_t percent) { return static_cast<uint8_t>((percent * 127 + 50) / 100); }
static uint8_t avrc_to_percent(uint8_t volume) { return static_cast<uint8_t>((volume * 100 + 63) / 127); }
static constexpr uint32_t STATS_INTERVAL_MS = 30000;
static constexpr uint32_t UNDERRUN_LOG_INTERVAL_MS = 5000;

// Playback progress is reported from this task, off the time-critical Bluetooth task (core 0)
static constexpr uint32_t REPORT_INTERVAL_MS = 5;

// Same software volume curve as ESPHome: (0, 1) maps linearly to [-49, 0] dB
static constexpr float VOLUME_MIN_DB = -49.0f;

static constexpr const char *NVS_NAMESPACE = "a2dp";
static constexpr const char *NVS_KEY_ADDRESS = "addr";

// 441 Hz test tone: exactly 100 samples per period at 44.1 kHz, -12 dBFS
static constexpr size_t TEST_TONE_PERIOD = 100;
static const std::array<int16_t, TEST_TONE_PERIOD> TEST_TONE = [] {
  std::array<int16_t, TEST_TONE_PERIOD> table{};
  for (size_t i = 0; i < TEST_TONE_PERIOD; i++) {
    table[i] = static_cast<int16_t>(8192.0f * sinf(2.0f * static_cast<float>(M_PI) * i / TEST_TONE_PERIOD));
  }
  return table;
}();

A2dpOutput *A2dpOutput::instance_ = nullptr;

static uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

static void format_bda(const uint8_t *bda, char *out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

bool A2dpOutput::begin(const Config &config, ProgressCallback progress) {
  instance_ = this;
  this->config_ = config;
  this->progress_ = std::move(progress);

  // Internal RAM: the data callback reads it from the time-critical Bluetooth task, and PSRAM is slow on ESP32 rev1
  this->ring_buffer_size_ = this->config_.buffer_ms * SAMPLE_RATE / 1000 * BYTES_PER_FRAME;
  this->ring_buffer_ = xRingbufferCreateWithCaps(this->ring_buffer_size_, RINGBUF_TYPE_BYTEBUF,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u byte ring buffer", static_cast<unsigned>(this->ring_buffer_size_));
    return false;
  }

  this->has_address_ = this->load_address_();

  if (!this->init_bluetooth_()) {
    return false;
  }

  xTaskCreatePinnedToCore(A2dpOutput::report_task, "a2dp_report", 3072, this, 5, nullptr, 1);
  xTaskCreatePinnedToCore(A2dpOutput::control_task, "a2dp_ctrl", 4096, this, 3, nullptr, 1);
  return true;
}

bool A2dpOutput::init_bluetooth_() {
  esp_err_t err;
  static bool ble_memory_released = false;
  if (!ble_memory_released) {
    ble_memory_released = true;
    if ((err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE)) != ESP_OK) {
      ESP_LOGW(TAG, "Releasing BLE controller memory failed: %s", esp_err_to_name(err));
    }
  }

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
    ESP_LOGE(TAG, "Controller init failed: %s", esp_err_to_name(err));
    return false;
  }
  if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
    ESP_LOGE(TAG, "Controller enable failed: %s", esp_err_to_name(err));
    return false;
  }
  if ((err = esp_bluedroid_init()) != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(err));
    return false;
  }
  if ((err = esp_bluedroid_enable()) != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(err));
    return false;
  }

  esp_bt_gap_set_device_name(this->config_.local_name.c_str());
  esp_bt_gap_register_callback(A2dpOutput::gap_callback);

  // Secure Simple Pairing without user interaction ("just works")
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

  // Fixed PIN for sinks that only support legacy pairing
  esp_bt_pin_code_t pin_code;
  const size_t pin_len = std::min(this->config_.pin_code.size(), sizeof(pin_code));
  memcpy(pin_code, this->config_.pin_code.data(), pin_len);
  esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, pin_len, pin_code);

  // Sinks open an AVRCP channel after connecting and may drop the link if it is refused
  esp_avrc_ct_register_callback(A2dpOutput::avrc_ct_callback);
  esp_avrc_ct_init();
  esp_avrc_tg_register_callback(A2dpOutput::avrc_tg_callback);
  esp_avrc_tg_init();
  // The speaker may register for our volume (absolute volume, target side)
  esp_avrc_rn_evt_cap_mask_t evt_set = {0};
  esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
  esp_avrc_tg_set_rn_evt_cap(&evt_set);

  esp_a2d_register_callback(A2dpOutput::a2dp_callback);
  esp_a2d_source_register_data_callback(A2dpOutput::data_callback);
  if ((err = esp_a2d_source_init()) != ESP_OK) {
    ESP_LOGE(TAG, "A2DP source init failed: %s", esp_err_to_name(err));
    return false;
  }

  // Bonds live in NVS: fall back to an already paired sink when no address is known
  int bond_count = esp_bt_gap_get_bond_device_num();
  if (bond_count > 0) {
    esp_bd_addr_t bonds[8];
    bond_count = std::min(bond_count, 8);
    esp_bt_gap_get_bond_device_list(&bond_count, bonds);
    for (int i = 0; i < bond_count; i++) {
      char addr[18];
      format_bda(bonds[i], addr);
      ESP_LOGI(TAG, "Paired device: %s", addr);
    }
    if (!this->has_address_) {
      memcpy(this->remote_address_, bonds[0], ESP_BD_ADDR_LEN);
      this->has_address_ = true;
    }
  }

  // Only the ESP connects out. Accepting a sink that reconnects on its own collides with our outgoing connect and
  // leaves a half-open ACL link ("Conn Exists").
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  return true;
}

void A2dpOutput::restart_bluetooth_(const char *reason) {
  // Clears controller state that survives reconnects: a stale ACL link ("Conn Exists") or reduced throughput after
  // an inquiry
  ESP_LOGW(TAG, "%s", reason);
  this->failed_connects_ = 0;
  esp_a2d_source_deinit();
  esp_avrc_tg_deinit();
  esp_avrc_ct_deinit();
  esp_bluedroid_disable();
  esp_bluedroid_deinit();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  this->media_state_ = MediaState::SUSPENDED;
  this->media_ctrl_pending_ = false;
  if (!this->init_bluetooth_()) {
    ESP_LOGE(TAG, "Bluetooth restart failed");
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Address persistence
// ---------------------------------------------------------------------------------------------------------------

void A2dpOutput::save_address_() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_blob(handle, NVS_KEY_ADDRESS, this->remote_address_, ESP_BD_ADDR_LEN);
    nvs_commit(handle);
    nvs_close(handle);
  }
}

bool A2dpOutput::load_address_() {
  nvs_handle_t handle;
  bool ok = false;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    size_t len = ESP_BD_ADDR_LEN;
    ok = nvs_get_blob(handle, NVS_KEY_ADDRESS, this->remote_address_, &len) == ESP_OK && len == ESP_BD_ADDR_LEN;
    nvs_close(handle);
  }
  return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Control task: connection management, media control, statistics
// ---------------------------------------------------------------------------------------------------------------

void A2dpOutput::control_task(void *params) {
  auto *self = static_cast<A2dpOutput *>(params);
  uint32_t last_stats = now_ms();
  uint32_t last_underrun_log = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(CONTROL_INTERVAL_MS));
    self->control_loop_();

    const uint32_t now = now_ms();
    if (now - last_stats >= STATS_INTERVAL_MS) {
      self->log_stats_(now - last_stats);
      last_stats = now;
    }
    const uint32_t underruns = self->underrun_bytes_.load();
    if (underruns > 0 && now - last_underrun_log >= UNDERRUN_LOG_INTERVAL_MS) {
      last_underrun_log = now;
      self->underrun_bytes_ -= underruns;
      ESP_LOGW(TAG, "Buffer underrun: %u ms of silence inserted",
               static_cast<unsigned>(underruns / BYTES_PER_FRAME * 1000 / SAMPLE_RATE));
    }
  }
}

void A2dpOutput::control_loop_() {
  const uint32_t now = now_ms();

  if (this->address_dirty_.exchange(false)) {
    this->has_address_ = true;
    this->save_address_();
  }

  switch (this->link_state_.load()) {
    case LinkState::IDLE:
      if (!this->attempted_once_ || now - this->last_attempt_ms_ >= this->config_.reconnect_interval_ms) {
        this->attempted_once_ = true;
        this->last_attempt_ms_ = now;
        if (this->rejected_connects_.load() >= MAX_REJECTED_CONNECTS) {
          // The speaker answers but refuses the A2DP channel, typically because it no longer knows our link key
          // (it was paired again with another firmware or device). Drop the stale pairing so the next connect pairs
          // again.
          ESP_LOGW(TAG, "Speaker rejects the connection, removing the stored pairing; if it does not pair again, "
                        "put it into pairing mode");
          esp_bt_gap_remove_bond_device(this->remote_address_);
          this->rejected_connects_ = 0;
        }
        if (this->failed_connects_.load() >= MAX_FAILED_CONNECTS) {
          this->restart_bluetooth_("Repeated connection failures, restarting Bluetooth");
        }
        if (this->has_address_) {
          this->connect_();
        } else if (!this->config_.device_name.empty()) {
          ESP_LOGI(TAG, "Searching for '%s'", this->config_.device_name.c_str());
          this->device_found_ = false;
          this->discovery_stopped_ = false;
          this->link_state_ = LinkState::DISCOVERING;
          if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, DISCOVERY_DURATION, 0) != ESP_OK) {
            this->link_state_ = LinkState::IDLE;
          }
        }
      }
      break;
    case LinkState::DISCOVERING:
      if (this->discovery_stopped_.load()) {
        if (this->device_found_.load()) {
          this->has_address_ = true;
          this->save_address_();
          // After an inquiry the controller kept delivering 1-4 % less than real time until the next reboot
          // (Sendspin's buffer then overflows, whole FLAC blocks drop out); a fresh stack connects cleanly
          this->restart_bluetooth_("Speaker found, restarting Bluetooth before connecting");
          this->connect_();
        } else {
          ESP_LOGW(TAG, "'%s' not found", this->config_.device_name.c_str());
          this->last_attempt_ms_ = now;
          this->link_state_ = LinkState::IDLE;
        }
      }
      break;
    case LinkState::CONNECTING:
      // The stack does not always report a failed connect (e.g. when it collides with the sink connecting to us)
      if (now - this->connect_started_ms_ >= CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Connection attempt timed out");
        this->failed_connects_++;
        this->last_attempt_ms_ = now;
        this->link_state_ = LinkState::IDLE;
      }
      break;
    case LinkState::CONNECTED: {
      const MediaState media = this->media_state_.load();
      // With keep_alive the stream (silence while idle) runs for the whole connection: some sinks, e.g. the Sony
      // SRS-XB100, power off after a while when they receive no audio even though they stay connected
      const bool want_stream = this->config_.keep_alive || this->streaming_.load();
      const bool can_send =
          !this->media_ctrl_pending_.load() && now - this->last_media_ctrl_ms_ >= MEDIA_CTRL_RETRY_MS;
      if (can_send) {
        if (want_stream && media == MediaState::SUSPENDED) {
          this->request_media_(ESP_A2D_MEDIA_CTRL_START);
        } else if (!want_stream && media == MediaState::STARTED) {
          this->request_media_(ESP_A2D_MEDIA_CTRL_SUSPEND);
        }
      }
      break;
    }
  }
}

void A2dpOutput::connect_() {
  char addr[18];
  format_bda(this->remote_address_, addr);
  ESP_LOGI(TAG, "Connecting to %s", addr);
  this->connect_started_ms_ = now_ms();
  this->acl_up_ = false;
  this->link_state_ = LinkState::CONNECTING;
  if (esp_a2d_source_connect(this->remote_address_) != ESP_OK) {
    ESP_LOGW(TAG, "Connect failed");
    this->last_attempt_ms_ = now_ms();
    this->link_state_ = LinkState::IDLE;
  }
}

void A2dpOutput::request_media_(esp_a2d_media_ctrl_t ctrl) {
  this->last_media_ctrl_ms_ = now_ms();
  this->media_ctrl_pending_ = true;
  this->media_state_ = ctrl == ESP_A2D_MEDIA_CTRL_START ? MediaState::STARTING : MediaState::SUSPENDING;
  ESP_LOGD(TAG, "Requesting media %s", ctrl == ESP_A2D_MEDIA_CTRL_START ? "start" : "suspend");
  if (esp_a2d_media_ctrl(ctrl) != ESP_OK) {
    this->media_ctrl_pending_ = false;
    this->media_state_ = ctrl == ESP_A2D_MEDIA_CTRL_START ? MediaState::SUSPENDED : MediaState::STARTED;
  }
}

void A2dpOutput::log_stats_(uint32_t elapsed_ms) {
  const uint32_t pulled = this->pulled_frames_.exchange(0);
  const uint32_t calls = this->cb_calls_.exchange(0);
  const uint32_t cb_time = this->cb_time_us_.exchange(0);
  // Pull time minus playback clock: positive = the stack pulls late (stall), negative = it pulls ahead (burst)
  const bool have_error = this->clock_error_min_us_ <= this->clock_error_max_us_;
  const int err_min = have_error ? static_cast<int>(this->clock_error_min_us_ / 1000) : 0;
  const int err_max = have_error ? static_cast<int>(this->clock_error_max_us_ / 1000) : 0;
  this->clock_error_min_us_ = INT64_MAX;
  this->clock_error_max_us_ = INT64_MIN;
  ESP_LOGI(TAG,
           "Stats: pull rate %u Hz, clock error %d..%d ms, %u clock resyncs, callback %u/s avg %u us max %u us, "
           "internal heap %u free (min %u), input %u of %u bytes/s accepted (peak %d dBFS), output peak %d dBFS, volume %u%%%s",
           static_cast<unsigned>(static_cast<uint64_t>(pulled) * 1000 / elapsed_ms),
           err_min, err_max,
           static_cast<unsigned>(this->clock_resyncs_.exchange(0)), static_cast<unsigned>(calls * 1000 / elapsed_ms),
           static_cast<unsigned>(calls ? cb_time / calls : 0), static_cast<unsigned>(this->cb_max_us_.exchange(0)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(this->accepted_bytes_.exchange(0) * 1000ULL / elapsed_ms),
           static_cast<unsigned>(this->offered_bytes_.exchange(0) * 1000ULL / elapsed_ms),
           static_cast<int>(std::lround(20.0 * std::log10(std::max<int32_t>(this->input_peak_.exchange(0), 1) / 32768.0))),
           static_cast<int>(std::lround(20.0 * std::log10(std::max<int32_t>(this->peak_level_.exchange(0), 1) / 32768.0))),
           static_cast<unsigned>(this->volume_percent_.load()), this->muted_.load() ? " (muted)" : "");
  this->log_task_load_();
}

void A2dpOutput::log_task_load_() {
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
  // CPU share of the busiest tasks since the last call, in % of one core
  static constexpr size_t MAX_TASKS = 48;
  static TaskHandle_t prev_handles[MAX_TASKS];
  static configRUN_TIME_COUNTER_TYPE prev_counters[MAX_TASKS];
  static size_t prev_count = 0;
  static configRUN_TIME_COUNTER_TYPE prev_total = 0;

  auto *tasks = static_cast<TaskStatus_t *>(heap_caps_malloc(MAX_TASKS * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
  if (tasks == nullptr) {
    return;
  }
  configRUN_TIME_COUNTER_TYPE total = 0;
  const size_t count = uxTaskGetSystemState(tasks, MAX_TASKS, &total);
  const configRUN_TIME_COUNTER_TYPE total_delta = total - prev_total;

  struct Load {
    const char *name;
    uint32_t permille;
    int core;
  };
  Load loads[MAX_TASKS];
  size_t n = 0;
  for (size_t i = 0; i < count && total_delta > 0; i++) {
    configRUN_TIME_COUNTER_TYPE prev = 0;
    for (size_t j = 0; j < prev_count; j++) {
      if (prev_handles[j] == tasks[i].xHandle) {
        prev = prev_counters[j];
        break;
      }
    }
    loads[n++] = {tasks[i].pcTaskName,
                  static_cast<uint32_t>((tasks[i].ulRunTimeCounter - prev) * 1000ULL / total_delta),
                  tasks[i].xCoreID == tskNO_AFFINITY ? 9 : static_cast<int>(tasks[i].xCoreID)};
  }
  std::sort(loads, loads + n, [](const Load &a, const Load &b) { return a.permille > b.permille; });
  std::string line;
  for (size_t i = 0; i < std::min<size_t>(n, 10); i++) {
    char buf[48];
    snprintf(buf, sizeof(buf), " %s[c%d]=%u.%u%%", loads[i].name, loads[i].core,
             static_cast<unsigned>(loads[i].permille / 10), static_cast<unsigned>(loads[i].permille % 10));
    line += buf;
  }
  if (prev_total != 0) {
    ESP_LOGI(TAG, "Load:%s", line.c_str());
  }

  prev_count = count;
  for (size_t i = 0; i < count; i++) {
    prev_handles[i] = tasks[i].xHandle;
    prev_counters[i] = tasks[i].ulRunTimeCounter;
  }
  prev_total = total;
  heap_caps_free(tasks);
#endif
}

void A2dpOutput::report_task(void *params) {
  auto *self = static_cast<A2dpOutput *>(params);
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(REPORT_INTERVAL_MS));
    portENTER_CRITICAL(&self->report_lock_);
    const uint32_t frames = self->report_frames_;
    const int64_t timestamp = self->report_timestamp_;
    self->report_frames_ = 0;
    portEXIT_CRITICAL(&self->report_lock_);
    if (frames > 0 && self->progress_) {
      self->progress_(frames, timestamp);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Audio input
// ---------------------------------------------------------------------------------------------------------------

size_t A2dpOutput::write(const uint8_t *data, size_t length, TickType_t timeout) {
  // Diagnostic: peak level of what the source hands over
  const auto *in = reinterpret_cast<const int16_t *>(data);
  int32_t in_peak = this->input_peak_.load();
  for (size_t i = 0; i < length / sizeof(int16_t); i++) {
    in_peak = std::max<int32_t>(in_peak, std::abs(static_cast<int32_t>(in[i])));
  }
  this->input_peak_ = in_peak;
  const size_t accepted = this->write_(data, length, timeout);
  this->offered_bytes_ += length;
  this->accepted_bytes_ += accepted;
  return accepted;
}

size_t A2dpOutput::write_(const uint8_t *data, size_t length, TickType_t timeout) {
  length -= length % BYTES_PER_FRAME;
  if (length == 0) {
    return 0;
  }

  if (!this->is_connected()) {
    // No sink to play to: discard at real-time pace and report it as played, so the source neither stalls nor runs
    // ahead
    const size_t max_bytes = DISCARD_CHUNK_MS * SAMPLE_RATE / 1000 * BYTES_PER_FRAME;
    const size_t discard = std::min(length, max_bytes);
    vTaskDelay(std::max<TickType_t>(pdMS_TO_TICKS(discard / BYTES_PER_FRAME * 1000 / SAMPLE_RATE), 1));
    if (this->progress_) {
      this->progress_(discard / BYTES_PER_FRAME, esp_timer_get_time());
    }
    return discard;
  }

  // Only whole frames, and only as much as fits, so the data callback never sees a split frame
  size_t free_bytes = xRingbufferGetCurFreeSize(this->ring_buffer_);
  if (free_bytes < BYTES_PER_FRAME) {
    vTaskDelay(timeout > 0 ? timeout : 1);
    free_bytes = xRingbufferGetCurFreeSize(this->ring_buffer_);
  }
  size_t to_write = std::min(length, free_bytes);
  to_write -= to_write % BYTES_PER_FRAME;
  if (to_write == 0 || xRingbufferSend(this->ring_buffer_, data, to_write, 0) != pdTRUE) {
    return 0;
  }
  return to_write;
}

void A2dpOutput::start() {
  // A new stream always starts a new playback timeline, even if the previous one never ended
  this->clock_reset_ = true;
  if (this->streaming_.exchange(true)) {
    ESP_LOGI(TAG, "Playback restarted");
    return;
  }
  this->drain_ring_buffer_();
  ESP_LOGI(TAG, "Playback started");
}

void A2dpOutput::stop() {
  if (!this->streaming_.exchange(false)) {
    return;
  }
  this->drain_ring_buffer_();
  ESP_LOGI(TAG, "Playback stopped");
}

void A2dpOutput::drain_ring_buffer_() {
  size_t size = 0;
  void *item;
  while ((item = xRingbufferReceiveUpTo(this->ring_buffer_, &size, 0, this->ring_buffer_size_)) != nullptr) {
    vRingbufferReturnItem(this->ring_buffer_, item);
  }
}

size_t A2dpOutput::read_ring_buffer_(uint8_t *data, size_t length) {
  // A byte buffer returns at most the contiguous part up to the wrap point, so read twice
  size_t total = 0;
  for (int i = 0; i < 2 && total < length; i++) {
    size_t size = 0;
    void *item = xRingbufferReceiveUpTo(this->ring_buffer_, &size, 0, length - total);
    if (item == nullptr) {
      break;
    }
    memcpy(data + total, item, size);
    vRingbufferReturnItem(this->ring_buffer_, item);
    total += size;
  }
  return total;
}

void A2dpOutput::set_volume(uint8_t volume_percent) {
  this->volume_percent_ = std::min<uint8_t>(volume_percent, 100);
  this->apply_volume_setting_();
}

void A2dpOutput::set_muted(bool muted) {
  this->muted_ = muted;
  this->apply_volume_setting_();
}

void A2dpOutput::apply_volume_setting_() {
  const uint8_t percent = this->volume_percent_.load();
  if (this->abs_volume_.load() == 1) {
    // The speaker applies the volume: full digital level, so its range and quality are not reduced twice
    this->q15_volume_ = this->muted_.load() ? 0 : 32767;
    if (this->avrc_ct_connected_.load()) {
      esp_avrc_ct_send_set_absolute_volume_cmd(AVRC_TL_VOLUME, percent_to_avrc(percent));
    }
  } else if (this->muted_.load() || percent == 0) {
    this->q15_volume_ = 0;
  } else if (percent >= 100) {
    this->q15_volume_ = 32767;
  } else {
    const float db = VOLUME_MIN_DB * (1.0f - percent / 100.0f);
    this->q15_volume_ = static_cast<int32_t>(32767.0f * powf(10.0f, db / 20.0f));
  }
}

void A2dpOutput::poll_remote(RemoteCommand &command, int &volume_percent) {
  command = this->pending_command_.exchange(RemoteCommand::NONE);
  volume_percent = this->pending_volume_.exchange(-1);
}

void A2dpOutput::apply_volume_(uint8_t *data, size_t length) {
  const int32_t factor = this->q15_volume_.load();
  if (factor >= 32767) {
    return;
  }
  auto *samples = reinterpret_cast<int16_t *>(data);
  for (size_t i = 0; i < length / sizeof(int16_t); i++) {
    samples[i] = static_cast<int16_t>((samples[i] * factor) >> 15);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Bluedroid callbacks (BTC task context)
// ---------------------------------------------------------------------------------------------------------------

void A2dpOutput::gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->handle_gap_event_(event, param);
  }
}

void A2dpOutput::a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->handle_a2dp_event_(event, param);
  }
}

void A2dpOutput::avrc_ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->handle_avrc_ct_event_(event, param);
  }
}

void A2dpOutput::avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->handle_avrc_tg_event_(event, param);
  }
}

// Controller side: the speaker is the AVRCP target for absolute volume
void A2dpOutput::handle_avrc_ct_event_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
      ESP_LOGD(TAG, "AVRCP controller %s", param->conn_stat.connected ? "connected" : "disconnected");
      this->ct_connected_ms_ = now_ms();
      this->avrc_ct_connected_ = param->conn_stat.connected;
      if (param->conn_stat.connected) {
        // Which notifications does the speaker support? Absolute volume = volume change notifications
        esp_avrc_ct_send_get_rn_capabilities_cmd(AVRC_TL_GET_CAPS);
      } else {
        this->peer_rn_cap_.bits = 0;
        this->abs_volume_ = -1;
      }
      break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
      this->peer_rn_cap_.bits = param->get_rn_caps_rsp.evt_set.bits;
      const bool abs = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &this->peer_rn_cap_,
                                                          ESP_AVRC_RN_VOLUME_CHANGE);
      this->abs_volume_ = abs ? 1 : 0;
      ESP_LOGI(TAG, "Speaker %s absolute volume", abs ? "supports" : "does not support");
      // Now that it is known who applies the volume, apply ours (on the speaker or as digital gain)
      this->apply_volume_setting_();
      if (abs) {
        esp_avrc_ct_send_register_notification_cmd(AVRC_TL_VOLUME, ESP_AVRC_RN_VOLUME_CHANGE, 0);
      }
      break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
      if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        this->peer_volume_ = param->change_ntf.event_parameter.volume;
        if (now_ms() - this->ct_connected_ms_ < AVRC_CONNECT_GRACE_MS) {
          // The speaker's own volume when connecting: ours has been applied instead
          ESP_LOGD(TAG, "Ignoring speaker volume right after connecting");
        } else {
          // Changed on the speaker (buttons). Some speakers only apply it once the source sets it, so echo it back
          // unchanged (setting the current value does not trigger a new notification), and pass it on
          const uint8_t percent = avrc_to_percent(this->peer_volume_);
          ESP_LOGI(TAG, "Volume changed on the speaker: %u%%", percent);
          esp_avrc_ct_send_set_absolute_volume_cmd(AVRC_TL_VOLUME, this->peer_volume_);
          this->volume_percent_ = percent;
          this->pending_volume_ = percent;
        }
        // Notifications are one-shot: register again
        esp_avrc_ct_send_register_notification_cmd(AVRC_TL_VOLUME, ESP_AVRC_RN_VOLUME_CHANGE, 0);
      }
      break;
    default:
      break;
  }
}

// Target side: the speaker sends button presses (passthrough) to us
void A2dpOutput::handle_avrc_tg_event_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
      ESP_LOGD(TAG, "AVRCP target %s", param->conn_stat.connected ? "connected" : "disconnected");
      if (param->conn_stat.connected) {
        this->tg_connected_ms_ = now_ms();
        // All passthrough commands are rejected by default. TG init is asynchronous, so the filter can only be set
        // once it has completed, which is guaranteed once the speaker connected to the target.
        static constexpr esp_avrc_pt_cmd_t CMDS[] = {ESP_AVRC_PT_CMD_PLAY,    ESP_AVRC_PT_CMD_PAUSE,    ESP_AVRC_PT_CMD_STOP,
                                           ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_VOL_UP,
                                           ESP_AVRC_PT_CMD_VOL_DOWN};
        esp_avrc_psth_bit_mask_t cmd_set = {0};
        for (esp_avrc_pt_cmd_t cmd : CMDS) {
          esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, cmd);
        }
        esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);
      }
      break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
      const uint8_t key = param->psth_cmd.key_code;
      if (param->psth_cmd.key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
        break;  // Act on press only
      }
      if (now_ms() - this->tg_connected_ms_ < AVRC_CONNECT_GRACE_MS) {
        ESP_LOGI(TAG, "Ignoring button 0x%x right after connecting", key);
        break;
      }
      ESP_LOGI(TAG, "Speaker button 0x%x", key);
      switch (key) {
        case ESP_AVRC_PT_CMD_PLAY:
          this->pending_command_ = RemoteCommand::PLAY;
          break;
        case ESP_AVRC_PT_CMD_PAUSE:
          this->pending_command_ = RemoteCommand::PAUSE;
          break;
        case ESP_AVRC_PT_CMD_STOP:
          this->pending_command_ = RemoteCommand::STOP;
          break;
        case ESP_AVRC_PT_CMD_FORWARD:
          this->pending_command_ = RemoteCommand::NEXT;
          break;
        case ESP_AVRC_PT_CMD_BACKWARD:
          this->pending_command_ = RemoteCommand::PREVIOUS;
          break;
        case ESP_AVRC_PT_CMD_VOL_UP:
        case ESP_AVRC_PT_CMD_VOL_DOWN: {
          // Speakers without absolute volume send volume buttons as commands: step our (digital) volume
          const int current = this->volume_percent_.load();
          const int next = key == ESP_AVRC_PT_CMD_VOL_UP ? std::min(current + AVRC_VOLUME_STEP, 100)
                                                         : std::max(current - AVRC_VOLUME_STEP, 0);
          this->volume_percent_ = static_cast<uint8_t>(next);
          this->apply_volume_setting_();
          this->pending_volume_ = next;
          break;
        }
        default:
          break;
      }
      break;
    }
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
      if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        esp_avrc_rn_param_t rn_param = {};
        rn_param.volume = percent_to_avrc(this->volume_percent_.load());
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
      }
      break;
    default:
      break;
  }
}

int32_t A2dpOutput::data_callback(uint8_t *data, int32_t len) {
  if (instance_ == nullptr || data == nullptr || len <= 0) {
    return 0;
  }
  const int64_t start = esp_timer_get_time();
  const int32_t result = instance_->fill_audio_(data, len);
  const auto elapsed = static_cast<uint32_t>(esp_timer_get_time() - start);
  instance_->cb_calls_++;
  instance_->cb_time_us_ += elapsed;
  uint32_t max = instance_->cb_max_us_.load();
  while (elapsed > max && !instance_->cb_max_us_.compare_exchange_weak(max, elapsed)) {
  }
  return result;
}

int32_t A2dpOutput::fill_audio_(uint8_t *data, int32_t len) {
  size_t bytes_read = 0;
  const bool streaming = this->streaming_.load();
  if (streaming) {
    const size_t wanted = len - (len % BYTES_PER_FRAME);
    bytes_read = this->read_ring_buffer_(data, wanted);
    bytes_read -= bytes_read % BYTES_PER_FRAME;

    // The stack pulls audio in irregular bursts, so the callback time jitters by tens of milliseconds. It paces the
    // pulls with the ESP's own clock, so the frames handed over are the playback clock: report timestamps from that
    // steady clock and only re-anchor after a real discontinuity (stream start, long stall). A constant offset
    // between pull time and actual playback is the sink's latency, which Sendspin's static delay compensates.
    const int64_t now = esp_timer_get_time();
    const int64_t model_now = this->clock_anchor_us_ + this->clock_frames_ * 1000000LL / SAMPLE_RATE;
    const int64_t error = now - model_now;
    if (this->clock_reset_.exchange(false)) {
      this->clock_valid_ = false;
    }
    if (this->clock_valid_) {
      this->clock_error_min_us_ = std::min<int64_t>(this->clock_error_min_us_, error);
      this->clock_error_max_us_ = std::max<int64_t>(this->clock_error_max_us_, error);
    }
    if (!this->clock_valid_ || error > CLOCK_RESYNC_US || error < -CLOCK_RESYNC_US) {
      if (this->clock_valid_) {
        this->clock_resyncs_++;
      }
      this->clock_anchor_us_ = now;
      this->clock_frames_ = 0;
      this->clock_valid_ = true;
    }
    this->clock_frames_ += len / BYTES_PER_FRAME;
    this->pulled_frames_ += len / BYTES_PER_FRAME;

    if (bytes_read > 0) {
      this->apply_volume_(data, bytes_read);
      // Diagnostic: peak level of the audio actually sent to the sink
      const auto *samples = reinterpret_cast<const int16_t *>(data);
      int32_t peak = this->peak_level_.load();
      for (size_t i = 0; i < bytes_read / sizeof(int16_t); i++) {
        peak = std::max<int32_t>(peak, std::abs(static_cast<int32_t>(samples[i])));
      }
      this->peak_level_ = peak;
      // Time when the last frame of this block leaves the ESP
      const int64_t timestamp =
          this->clock_anchor_us_ +
          (this->clock_frames_ - static_cast<int64_t>((len - bytes_read) / BYTES_PER_FRAME)) * 1000000LL / SAMPLE_RATE;
      portENTER_CRITICAL(&this->report_lock_);
      this->report_frames_ += bytes_read / BYTES_PER_FRAME;
      this->report_timestamp_ = timestamp;
      portEXIT_CRITICAL(&this->report_lock_);
    }
    if (bytes_read < static_cast<size_t>(len)) {
      this->underrun_bytes_ += len - bytes_read;
    }
  } else {
    this->clock_valid_ = false;
  }

  // Always hand the stack a full buffer; silence (or the test tone) fills any gap
  if (bytes_read < static_cast<size_t>(len)) {
    if (this->config_.test_tone && !streaming) {
      auto *samples = reinterpret_cast<int16_t *>(data + bytes_read);
      const size_t frames = (len - bytes_read) / BYTES_PER_FRAME;
      for (size_t i = 0; i < frames; i++) {
        samples[2 * i] = samples[2 * i + 1] = TEST_TONE[this->test_tone_phase_];
        this->test_tone_phase_ = (this->test_tone_phase_ + 1) % TEST_TONE_PERIOD;
      }
    } else {
      memset(data + bytes_read, 0, len - bytes_read);
    }
  }
  return len;
}

void A2dpOutput::handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
        if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME && name[0] == '\0') {
          memcpy(name, prop->val, std::min<size_t>(prop->len, ESP_BT_GAP_MAX_BDNAME_LEN));
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR && name[0] == '\0') {
          uint8_t n = 0;
          auto *eir = static_cast<uint8_t *>(prop->val);
          uint8_t *eir_name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &n);
          if (eir_name == nullptr) {
            eir_name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &n);
          }
          if (eir_name != nullptr) {
            memcpy(name, eir_name, std::min<size_t>(n, ESP_BT_GAP_MAX_BDNAME_LEN));
          }
        }
      }
      char addr[18];
      format_bda(param->disc_res.bda, addr);
      ESP_LOGI(TAG, "Found device %s '%s'", addr, name);
      if (!this->device_found_.load() && name[0] != '\0' && this->config_.device_name == name) {
        memcpy(this->remote_address_, param->disc_res.bda, ESP_BD_ADDR_LEN);
        this->device_found_ = true;
        esp_bt_gap_cancel_discovery();
      }
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        this->discovery_stopped_ = true;
      }
      break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
      // Tells a speaker that is off (no link) apart from one that answers but refuses A2DP
      if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS &&
          memcmp(param->acl_conn_cmpl_stat.bda, this->remote_address_, ESP_BD_ADDR_LEN) == 0) {
        this->acl_up_ = true;
      }
      break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Paired with '%s'", param->auth_cmpl.device_name);
      } else {
        ESP_LOGW(TAG, "Pairing failed (status %d); is the speaker in pairing mode?", param->auth_cmpl.stat);
      }
      break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
      esp_bt_pin_code_t pin_code;
      const size_t pin_len = std::min(this->config_.pin_code.size(), sizeof(pin_code));
      memcpy(pin_code, this->config_.pin_code.data(), pin_len);
      esp_bt_gap_pin_reply(param->pin_req.bda, true, pin_len, pin_code);
      break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
    default:
      break;
  }
}

void A2dpOutput::handle_a2dp_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
      char addr[18];
      format_bda(param->conn_stat.remote_bda, addr);
      if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Connected to %s", addr);
        this->failed_connects_ = 0;
        this->rejected_connects_ = 0;
        if (memcmp(this->remote_address_, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN) != 0) {
          memcpy(this->remote_address_, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
          this->address_dirty_ = true;
        }
        this->media_state_ = MediaState::SUSPENDED;
        this->media_ctrl_pending_ = false;
        this->link_state_ = LinkState::CONNECTED;
      } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from %s", addr);
        if (this->link_state_.load() == LinkState::CONNECTING) {
          if (this->acl_up_.load()) {
            this->rejected_connects_++;
          } else {
            this->failed_connects_++;
          }
        }
        this->media_state_ = MediaState::SUSPENDED;
        this->media_ctrl_pending_ = false;
        this->last_attempt_ms_ = now_ms();
        this->link_state_ = LinkState::IDLE;
      }
      break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
      // No ESP_COEX_BT_ST_A2DP_STREAMING hint: it gives Bluetooth priority over WiFi, and the resulting WiFi latency
      // ruined Sendspin's clock sync (3.5 ms round trip before streaming, hundreds of ms after)
      if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
        ESP_LOGI(TAG, "Media stream started");
        this->media_state_ = MediaState::STARTED;
      } else {
        ESP_LOGI(TAG, "Media stream suspended");
        this->media_state_ = MediaState::SUSPENDED;
      }
      break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
      this->media_ctrl_pending_ = false;
      if (param->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
        ESP_LOGW(TAG, "Media control %d rejected (status %d)", param->media_ctrl_stat.cmd,
                 param->media_ctrl_stat.status);
        this->media_state_ =
            param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START ? MediaState::SUSPENDED : MediaState::STARTED;
      }
      break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
      // Delay reports are in 1/10 ms
      ESP_LOGI(TAG, "Speaker reports a delay of %u.%u ms", param->a2d_report_delay_value_stat.delay_value / 10,
               param->a2d_report_delay_value_stat.delay_value % 10);
      break;
    default:
      break;
  }
}
