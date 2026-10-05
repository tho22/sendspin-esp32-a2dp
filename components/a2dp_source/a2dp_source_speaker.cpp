#include "a2dp_source_speaker.h"

#ifdef USE_ESP32

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_coexist.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_timer.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

// esp-audio-libs
#include <gain.h>

namespace esphome::a2dp_source {

static const char *const TAG = "a2dp_source";

// The legacy A2DP source data path always encodes 44.1 kHz, 16 bit, stereo PCM
static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr size_t BYTES_PER_FRAME = 4;

// Same software volume curve as the i2s_audio speaker
static constexpr float SOFTWARE_VOLUME_MIN_DB = -49.0f;

static constexpr uint32_t MEDIA_CTRL_RETRY_MS = 1000;
static constexpr uint32_t MAX_FAILED_CONNECTS = 4;  // Then restart the Bluetooth stack
static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
static constexpr size_t DISCARD_CHUNK_MS = 20;

// Playback progress is reported to the listeners from this task, off the time-critical Bluetooth task (core 0)
static constexpr uint32_t REPORT_INTERVAL_MS = 5;
static constexpr uint32_t REPORT_TASK_STACK_SIZE = 3072;
static constexpr UBaseType_t REPORT_TASK_PRIORITY = 5;
static constexpr BaseType_t REPORT_TASK_CORE = 1;  // Granularity of real-time discarding while disconnected
static constexpr uint32_t UNDERRUN_LOG_INTERVAL_MS = 5000;
static constexpr uint8_t DISCOVERY_DURATION = 10;  // x 1.28 s

// Playback clock: Sendspin hard-syncs at 5 ms error, so the reported clock must be steady (see fill_audio_())
static constexpr int64_t CLOCK_RESYNC_US = 2000000;  // Stalls up to the sink buffer are caught up later
static constexpr uint32_t STATS_LOG_INTERVAL_MS = 30000;

A2DPSourceSpeaker *A2DPSourceSpeaker::instance_ = nullptr;

// 441 Hz test tone: exactly 100 samples per period at 44.1 kHz, -12 dBFS
static constexpr size_t TEST_TONE_PERIOD = 100;
static const std::array<int16_t, TEST_TONE_PERIOD> TEST_TONE = [] {
  std::array<int16_t, TEST_TONE_PERIOD> table{};
  for (size_t i = 0; i < TEST_TONE_PERIOD; i++) {
    table[i] = static_cast<int16_t>(8192.0f * sinf(2.0f * static_cast<float>(M_PI) * i / TEST_TONE_PERIOD));
  }
  return table;
}();

static const char *link_state_to_str(LinkState state) {
  switch (state) {
    case LinkState::IDLE:
      return "idle";
    case LinkState::DISCOVERING:
      return "discovering";
    case LinkState::CONNECTING:
      return "connecting";
    case LinkState::CONNECTED:
      return "connected";
  }
  return "unknown";
}

static void format_bda(const uint8_t *bda, char *out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

void A2DPSourceSpeaker::set_address(uint64_t address) {
  for (int i = 0; i < 6; i++) {
    this->remote_address_[i] = (address >> ((5 - i) * 8)) & 0xFF;
  }
  this->has_address_ = true;
  this->address_configured_ = true;
}

void A2DPSourceSpeaker::setup() {
  instance_ = this;
  ESP_LOGI(TAG, "Reset reason: %d", static_cast<int>(esp_reset_reason()));

  // Reuse the address found by an earlier discovery: a paired sink is usually no longer discoverable
  this->address_pref_ = global_preferences->make_preference<esp_bd_addr_t>(fnv1_hash("a2dp_source_address"));
  if (!this->address_configured_) {
    esp_bd_addr_t saved;
    if (this->address_pref_.load(&saved)) {
      memcpy(this->remote_address_, saved, ESP_BD_ADDR_LEN);
      this->has_address_ = true;
    }
  }

  const size_t buffer_size = this->buffer_duration_ms_ * SAMPLE_RATE / 1000 * BYTES_PER_FRAME;
  // Internal RAM: the data callback reads it from the time-critical Bluetooth task, and PSRAM is slow on ESP32
  this->ring_buffer_ = ring_buffer::RingBuffer::create(buffer_size, ring_buffer::RingBuffer::MemoryPreference::INTERNAL_FIRST);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %zu byte ring buffer", buffer_size);
    this->mark_failed();
    return;
  }

  this->set_volume(this->volume_);

  if (xTaskCreatePinnedToCore(A2DPSourceSpeaker::report_task, "a2dp_report", REPORT_TASK_STACK_SIZE, this,
                              REPORT_TASK_PRIORITY, nullptr, REPORT_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "Failed to start report task");
    this->mark_failed();
    return;
  }

  if (!this->init_bluetooth_()) {
    this->mark_failed();
    return;
  }
}

void A2DPSourceSpeaker::report_task(void *params) {
  auto *self = static_cast<A2DPSourceSpeaker *>(params);
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(REPORT_INTERVAL_MS));
    portENTER_CRITICAL(&self->report_lock_);
    const uint32_t frames = self->report_frames_;
    const int64_t timestamp = self->report_timestamp_;
    self->report_frames_ = 0;
    portEXIT_CRITICAL(&self->report_lock_);
    if (frames > 0) {
      self->audio_output_callback_.call(frames, timestamp);
    }
  }
}

bool A2DPSourceSpeaker::init_bluetooth_() {
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

  esp_bt_gap_set_device_name(this->local_name_.c_str());
  esp_bt_gap_register_callback(A2DPSourceSpeaker::gap_callback);

  // Secure Simple Pairing without user interaction ("just works")
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

  // Fixed PIN for sinks that only support legacy pairing
  esp_bt_pin_code_t pin_code;
  const size_t pin_len = std::min(this->pin_code_.size(), sizeof(pin_code));
  memcpy(pin_code, this->pin_code_.data(), pin_len);
  esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, pin_len, pin_code);

  // Sinks open an AVRCP channel after connecting and may drop the link if it is refused
  esp_avrc_ct_register_callback(A2DPSourceSpeaker::avrc_ct_callback);
  if ((err = esp_avrc_ct_init()) != ESP_OK) {
    ESP_LOGW(TAG, "AVRCP controller init failed: %s", esp_err_to_name(err));
  }
  esp_avrc_tg_register_callback(A2DPSourceSpeaker::avrc_tg_callback);
  if ((err = esp_avrc_tg_init()) != ESP_OK) {
    ESP_LOGW(TAG, "AVRCP target init failed: %s", esp_err_to_name(err));
  }

  esp_a2d_register_callback(A2DPSourceSpeaker::a2dp_callback);
  esp_a2d_source_register_data_callback(A2DPSourceSpeaker::data_callback);
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

  // Only the ESP connects out (the address comes from the bond list or a discovery). Accepting a sink that reconnects
  // on its own collides with our outgoing connect and leaves a half-open ACL link ("Conn Exists").
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  return true;
}

void A2DPSourceSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG, "A2DP Source Speaker:");
  if (this->has_address_) {
    char addr[18];
    format_bda(this->remote_address_, addr);
    ESP_LOGCONFIG(TAG, "  Address: %s", addr);
  }
  if (!this->device_name_.empty()) {
    ESP_LOGCONFIG(TAG, "  Device name: %s", this->device_name_.c_str());
  }
  ESP_LOGCONFIG(TAG,
                "  Local name: %s\n"
                "  Buffer duration: %" PRIu32 " ms\n"
                "  Reconnect interval: %" PRIu32 " ms\n"
                "  Link state: %s",
                this->local_name_.c_str(), this->buffer_duration_ms_, this->reconnect_interval_ms_,
                link_state_to_str(this->link_state_.load()));
}

void A2DPSourceSpeaker::loop() {
  const uint32_t now = millis();

  if (this->address_dirty_.exchange(false)) {
    this->has_address_ = true;
    this->address_pref_.save(&this->remote_address_);
    global_preferences->sync();
  }

  if (this->finishing_ && this->ring_buffer_->available() == 0) {
    this->stop();
  }

  // Connection management
  switch (this->link_state_.load()) {
    case LinkState::IDLE:
      if (!this->attempted_once_ || (now - this->last_attempt_ms_ >= this->reconnect_interval_ms_)) {
        this->attempted_once_ = true;
        this->last_attempt_ms_ = now;
        if (this->failed_connects_.load() >= MAX_FAILED_CONNECTS) {
          this->restart_bluetooth_();
          if (this->is_failed()) {
            return;
          }
        }
        if (this->has_address_) {
          this->connect_();
        } else {
          ESP_LOGI(TAG, "Searching for '%s'", this->device_name_.c_str());
          this->device_found_ = false;
          this->discovery_stopped_ = false;
          this->link_state_ = LinkState::DISCOVERING;
          esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, DISCOVERY_DURATION, 0);
          if (err != ESP_OK) {
            ESP_LOGW(TAG, "Starting discovery failed: %s", esp_err_to_name(err));
            this->link_state_ = LinkState::IDLE;
          }
        }
      }
      break;
    case LinkState::DISCOVERING:
      if (this->discovery_stopped_.load()) {
        if (this->device_found_.load()) {
          this->has_address_ = true;
          this->address_pref_.save(&this->remote_address_);
          global_preferences->sync();
          this->connect_();
        } else {
          ESP_LOGW(TAG, "'%s' not found, retrying in %" PRIu32 " s", this->device_name_.c_str(),
                   this->reconnect_interval_ms_ / 1000);
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
      const bool want_stream = this->keep_alive_ || this->state_ == speaker::STATE_RUNNING;
      const bool can_send = !this->media_ctrl_pending_.load() && (now - this->last_media_ctrl_ms_ >= MEDIA_CTRL_RETRY_MS);
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

  if (this->link_state_.load() == LinkState::CONNECTED) {
    // WiFi/BT coexistence needs WiFi modem sleep to give A2DP enough airtime, but Sendspin requests high performance
    // WiFi (no power save) while streaming. Without airtime the stack pulls audio slower than real time.
    wifi_ps_type_t ps;
    if (esp_wifi_get_ps(&ps) == ESP_OK && ps == WIFI_PS_NONE) {
      ESP_LOGD(TAG, "Re-enabling WiFi modem sleep for Bluetooth coexistence");
      esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
  }

  if (now - this->last_stats_log_ms_ >= STATS_LOG_INTERVAL_MS) {
    const uint32_t elapsed_ms = now - this->last_stats_log_ms_;
    this->last_stats_log_ms_ = now;
    const uint32_t pulled = this->pulled_frames_.exchange(0);
    const uint32_t calls = this->cb_calls_.exchange(0);
    const uint32_t cb_time = this->cb_time_us_.exchange(0);
    ESP_LOGD(TAG,
             "Stats: pull rate %" PRIu32 " Hz, %" PRIu32 " clock resyncs, callback %" PRIu32 "/s avg %" PRIu32
             " us max %" PRIu32 " us (%" PRIu32 " ms/s), internal heap %u free (min %u)",
             static_cast<uint32_t>(static_cast<uint64_t>(pulled) * 1000 / elapsed_ms), this->clock_resyncs_.exchange(0),
             calls * 1000 / elapsed_ms, calls ? cb_time / calls : 0, this->cb_max_us_.exchange(0),
             cb_time / elapsed_ms, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  }

  if (now - this->last_load_log_ms_ >= STATS_LOG_INTERVAL_MS) {
    this->last_load_log_ms_ = now;
    this->log_task_load_();
  }

  const uint32_t underruns = this->underrun_bytes_.load();
  if (underruns > 0 && (now - this->last_underrun_log_ms_ >= UNDERRUN_LOG_INTERVAL_MS)) {
    this->last_underrun_log_ms_ = now;
    this->underrun_bytes_ -= underruns;
    ESP_LOGW(TAG, "Buffer underrun: %" PRIu32 " ms of silence inserted",
             underruns / BYTES_PER_FRAME * 1000 / SAMPLE_RATE);
  }
}

void A2DPSourceSpeaker::restart_bluetooth_() {
  // A link loss can leave a stale ACL link in the controller that the host no longer knows about; every connect then
  // fails with "Conn Exists". Only a controller restart clears it.
  ESP_LOGW(TAG, "%" PRIu32 " connection attempts failed, restarting Bluetooth", this->failed_connects_.load());
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
    this->mark_failed();
  }
}

void A2DPSourceSpeaker::log_task_load_() {
#if defined(configGENERATE_RUN_TIME_STATS) && configGENERATE_RUN_TIME_STATS && defined(configUSE_TRACE_FACILITY) && \
    configUSE_TRACE_FACILITY
  // Diagnostic: CPU share of the busiest tasks since the last call, in % of one core
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
    UBaseType_t prio;
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
    const uint32_t permille = static_cast<uint32_t>((tasks[i].ulRunTimeCounter - prev) * 1000ULL / total_delta);
    loads[n++] = {tasks[i].pcTaskName, permille, static_cast<int>(tasks[i].xCoreID), tasks[i].uxCurrentPriority};
  }
  std::sort(loads, loads + n, [](const Load &a, const Load &b) { return a.permille > b.permille; });
  std::string line;
  for (size_t i = 0; i < std::min<size_t>(n, 10); i++) {
    char buf[48];
    snprintf(buf, sizeof(buf), " %s[c%d p%u]=%u.%u%%", loads[i].name, loads[i].core == tskNO_AFFINITY ? 9 : loads[i].core,
             static_cast<unsigned>(loads[i].prio), static_cast<unsigned>(loads[i].permille / 10),
             static_cast<unsigned>(loads[i].permille % 10));
    line += buf;
  }
  if (prev_total != 0) {
    ESP_LOGD(TAG, "Load:%s", line.c_str());
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

void A2DPSourceSpeaker::connect_() {
  char addr[18];
  format_bda(this->remote_address_, addr);
  ESP_LOGI(TAG, "Connecting to %s", addr);
  this->connect_started_ms_ = millis();
  this->link_state_ = LinkState::CONNECTING;
  esp_err_t err = esp_a2d_source_connect(this->remote_address_);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Connect failed: %s", esp_err_to_name(err));
    this->last_attempt_ms_ = millis();
    this->link_state_ = LinkState::IDLE;
  }
}

void A2DPSourceSpeaker::request_media_(esp_a2d_media_ctrl_t ctrl) {
  this->last_media_ctrl_ms_ = millis();
  this->media_ctrl_pending_ = true;
  this->media_state_ = ctrl == ESP_A2D_MEDIA_CTRL_START ? MediaState::STARTING : MediaState::SUSPENDING;
  ESP_LOGD(TAG, "Requesting media %s", ctrl == ESP_A2D_MEDIA_CTRL_START ? "start" : "suspend");
  esp_err_t err = esp_a2d_media_ctrl(ctrl);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Media control failed: %s", esp_err_to_name(err));
    this->media_ctrl_pending_ = false;
    this->media_state_ = ctrl == ESP_A2D_MEDIA_CTRL_START ? MediaState::SUSPENDED : MediaState::STARTED;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Speaker interface
// ---------------------------------------------------------------------------------------------------------------

size_t A2DPSourceSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed()) {
    return 0;
  }

  this->start();

  if (this->link_state_.load() != LinkState::CONNECTED) {
    // No sink to play to: discard the audio at real-time pace and report it as played. Blocking instead would stall
    // the upstream tasks (e.g. a resampler that cannot stop while it waits to write), and consuming faster than real
    // time would starve the CPU and let the source run ahead.
    const size_t max_bytes = DISCARD_CHUNK_MS * SAMPLE_RATE / 1000 * BYTES_PER_FRAME;
    const size_t discard = std::min(length - length % BYTES_PER_FRAME, max_bytes);
    vTaskDelay(std::max<TickType_t>(pdMS_TO_TICKS(discard / BYTES_PER_FRAME * 1000 / SAMPLE_RATE), 1));
    if (discard > 0) {
      this->audio_output_callback_.call(discard / BYTES_PER_FRAME, esp_timer_get_time());
    }
    return discard;
  }

  // Only ever write whole frames, so the data callback never sees a split frame
  length -= length % BYTES_PER_FRAME;
  return this->ring_buffer_->write_without_replacement(data, length, ticks_to_wait, false);
}

void A2DPSourceSpeaker::start() {
  if (this->is_failed()) {
    return;
  }
  // New audio after finish() continues the stream instead of stopping it
  this->finishing_ = false;
  if (this->state_ == speaker::STATE_RUNNING) {
    return;
  }
  // Starting only flips flags, so it happens synchronously; play() may call this from the source's task
  this->drain_ring_buffer_();
  this->state_ = speaker::STATE_RUNNING;
  this->streaming_ = true;
  ESP_LOGD(TAG, "Speaker started");
}

void A2DPSourceSpeaker::stop() {
  this->finishing_ = false;
  if (this->state_ == speaker::STATE_STOPPED) {
    return;
  }
  this->streaming_ = false;
  this->state_ = speaker::STATE_STOPPED;
  this->drain_ring_buffer_();
  ESP_LOGD(TAG, "Speaker stopped");
}

void A2DPSourceSpeaker::finish() {
  if (this->state_ == speaker::STATE_RUNNING) {
    this->finishing_ = true;
  } else {
    this->stop();
  }
}

bool A2DPSourceSpeaker::has_buffered_data() const {
  return this->ring_buffer_ != nullptr && this->ring_buffer_->available() > 0;
}

void A2DPSourceSpeaker::drain_ring_buffer_() {
  // Reading is safe while the data callback reads concurrently; resetting the ring buffer is not
  uint8_t scratch[256];
  while (this->ring_buffer_->read(scratch, sizeof(scratch), 0) > 0) {
  }
}

void A2DPSourceSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  if (this->mute_state_) {
    return;
  }
  if (volume >= 1.0f) {
    this->q31_volume_factor_ = INT32_MAX;
  } else if (volume <= 0.0f) {
    this->q31_volume_factor_ = 0;
  } else {
    this->q31_volume_factor_ =
        esp_audio_libs::gain::db_to_q31(remap<float, float>(volume, 0.0f, 1.0f, SOFTWARE_VOLUME_MIN_DB, 0.0f));
  }
}

void A2DPSourceSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  if (mute_state) {
    this->q31_volume_factor_ = 0;
  } else {
    this->set_volume(this->volume_);
  }
}

void A2DPSourceSpeaker::apply_software_volume_(uint8_t *data, size_t length) {
  const int32_t factor = this->q31_volume_factor_.load();
  if (factor == INT32_MAX) {
    return;
  }
  esp_audio_libs::gain::apply(data, data, factor, length / sizeof(int16_t), sizeof(int16_t));
}

// ---------------------------------------------------------------------------------------------------------------
// Bluedroid callbacks (BTC task context)
// ---------------------------------------------------------------------------------------------------------------

void A2DPSourceSpeaker::gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->handle_gap_event_(event, param);
  }
}

void A2DPSourceSpeaker::a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->handle_a2dp_event_(event, param);
  }
}

void A2DPSourceSpeaker::avrc_ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
    ESP_LOGD(TAG, "AVRCP controller %s", param->conn_stat.connected ? "connected" : "disconnected");
  }
}

void A2DPSourceSpeaker::avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  if (event == ESP_AVRC_TG_CONNECTION_STATE_EVT) {
    ESP_LOGD(TAG, "AVRCP target %s", param->conn_stat.connected ? "connected" : "disconnected");
  }
}

int32_t A2DPSourceSpeaker::data_callback(uint8_t *data, int32_t len) {
  if (instance_ == nullptr || data == nullptr || len <= 0) {
    return 0;
  }
  const int64_t start = esp_timer_get_time();
  const int32_t result = instance_->fill_audio_(data, len);
  const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - start);
  instance_->cb_calls_++;
  instance_->cb_time_us_ += elapsed;
  uint32_t max = instance_->cb_max_us_.load();
  while (elapsed > max && !instance_->cb_max_us_.compare_exchange_weak(max, elapsed)) {
  }
  return result;
}

int32_t A2DPSourceSpeaker::fill_audio_(uint8_t *data, int32_t len) {
  size_t bytes_read = 0;
  if (this->streaming_.load() && !this->pause_state_.load()) {
    const size_t wanted = len - (len % BYTES_PER_FRAME);
    bytes_read = this->ring_buffer_->read(data, wanted, 0);
    bytes_read -= bytes_read % BYTES_PER_FRAME;

    // The stack pulls audio in irregular bursts, so the callback time jitters by tens of milliseconds. It paces the
    // pulls with the ESP's own clock, so the frames handed over are the playback clock: report timestamps from that
    // steady clock and only re-anchor after a real discontinuity (stream start, stall). Sendspin hard-syncs at 5 ms
    // error, so any continuous correction towards the jittery pull time would show up as drift. A constant offset
    // between pull time and actual playback is the sink's latency, which Sendspin's static delay compensates.
    const int64_t now = esp_timer_get_time();
    const int64_t model_now = this->clock_anchor_us_ + this->clock_frames_ * 1000000LL / SAMPLE_RATE;
    const int64_t error = now - model_now;
    if (!this->clock_valid_ || error > CLOCK_RESYNC_US || error < -CLOCK_RESYNC_US) {
      if (this->clock_valid_) {
        this->clock_resyncs_++;
      }
      this->clock_anchor_us_ = now;
      this->clock_frames_ = 0;
      this->clock_valid_ = true;
    }
    this->pulled_frames_ += len / BYTES_PER_FRAME;
    this->clock_frames_ += len / BYTES_PER_FRAME;

    if (bytes_read > 0) {
      this->apply_software_volume_(data, bytes_read);
      // Time when the last frame of this block leaves the ESP; the sink's own latency is compensated with
      // Sendspin's static delay
      const int64_t timestamp = this->clock_anchor_us_ + (this->clock_frames_ - (len - bytes_read) / BYTES_PER_FRAME) *
                                                             1000000LL / SAMPLE_RATE;
      // Reporting runs the listeners (Sendspin takes a mutex there), which must not block the Bluetooth task: hand
      // the progress to report_task() instead
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

  // Always hand the stack a full buffer; silence fills any gap
  if (bytes_read < static_cast<size_t>(len)) {
    if (this->test_tone_ && !this->streaming_.load()) {
      // Diagnostic: a steady tone instead of idle silence makes dropouts on the Bluetooth link audible without any
      // WiFi streaming going on
      int16_t *samples = reinterpret_cast<int16_t *>(data + bytes_read);
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

void A2DPSourceSpeaker::handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
        if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME && name[0] == '\0') {
          const size_t n = std::min<size_t>(prop->len, ESP_BT_GAP_MAX_BDNAME_LEN);
          memcpy(name, prop->val, n);
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR && name[0] == '\0') {
          uint8_t n = 0;
          uint8_t *eir = static_cast<uint8_t *>(prop->val);
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

      if (!this->device_found_.load() && name[0] != '\0' && this->device_name_ == name) {
        memcpy(this->remote_address_, param->disc_res.bda, ESP_BD_ADDR_LEN);
        this->device_found_ = true;
        ESP_LOGI(TAG, "Found '%s' at %s; set 'address: %s' to skip discovery", name, addr, addr);
        esp_bt_gap_cancel_discovery();
      }
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        this->discovery_stopped_ = true;
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
      const size_t pin_len = std::min(this->pin_code_.size(), sizeof(pin_code));
      memcpy(pin_code, this->pin_code_.data(), pin_len);
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

void A2DPSourceSpeaker::handle_a2dp_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
      char addr[18];
      format_bda(param->conn_stat.remote_bda, addr);
      switch (param->conn_stat.state) {
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
          ESP_LOGI(TAG, "Connected to %s", addr);
          this->failed_connects_ = 0;
          if (!this->address_configured_ && memcmp(this->remote_address_, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN)) {
            // The sink connected on its own (e.g. after power on); remember it for outgoing reconnects
            memcpy(this->remote_address_, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            this->address_dirty_ = true;
          }
          this->media_state_ = MediaState::SUSPENDED;
          this->media_ctrl_pending_ = false;
          this->link_state_ = LinkState::CONNECTED;
          break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
          ESP_LOGW(TAG, "Disconnected from %s", addr);
          if (this->link_state_.load() == LinkState::CONNECTING) {
            this->failed_connects_++;
          }
          this->media_state_ = MediaState::SUSPENDED;
          this->media_ctrl_pending_ = false;
          this->last_attempt_ms_ = millis();
          this->link_state_ = LinkState::IDLE;
          break;
        default:
          break;
      }
      break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
      // Tell the WiFi/BT coexistence arbiter that A2DP is streaming, so it grants Bluetooth more airtime
      if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
        ESP_LOGD(TAG, "Media stream started");
        this->media_state_ = MediaState::STARTED;
        esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_PAUSED);
        esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_STREAMING);
      } else {
        ESP_LOGD(TAG, "Media stream suspended");
        this->media_state_ = MediaState::SUSPENDED;
        esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_STREAMING);
        esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_PAUSED);
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
      ESP_LOGI(TAG, "Speaker reports a delay of %u.%u ms; use it as Sendspin static delay",
               param->a2d_report_delay_value_stat.delay_value / 10,
               param->a2d_report_delay_value_stat.delay_value % 10);
      break;
    default:
      break;
  }
}

}  // namespace esphome::a2dp_source

#endif  // USE_ESP32
