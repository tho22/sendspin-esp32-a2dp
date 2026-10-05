// Sendspin player that streams to a Bluetooth A2DP speaker. Plain ESP-IDF, no ESPHome.

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mdns.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <sendspin/client.h>
#include <sendspin/config.h>
#include <sendspin/controller_role.h>
#include <sendspin/player_role.h>

#include "a2dp_output.h"

static const char *const TAG = "main";

static constexpr uint32_t CLIENT_LOOP_INTERVAL_MS = 5;
static constexpr const char *NVS_NAMESPACE = "sendspin";

static std::atomic<bool> g_network_ready{false};

// ---------------------------------------------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------------------------------------------

static void wifi_event_handler(void * /*arg*/, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    g_network_ready = false;
    ESP_LOGW(TAG, "WiFi disconnected (reason %d), reconnecting",
             static_cast<wifi_event_sta_disconnected_t *>(data)->reason);
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&event->ip_info.ip));
    g_network_ready = true;
  }
}

static void wifi_start() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *netif = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(netif, CONFIG_APP_HOSTNAME);

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));

  wifi_config_t cfg = {};
  strncpy(reinterpret_cast<char *>(cfg.sta.ssid), CONFIG_APP_WIFI_SSID, sizeof(cfg.sta.ssid));
  strncpy(reinterpret_cast<char *>(cfg.sta.password), CONFIG_APP_WIFI_PASSWORD, sizeof(cfg.sta.password));
  cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  // WiFi/BT coexistence needs modem sleep, otherwise A2DP does not get enough airtime
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

static void mdns_start() {
  ESP_ERROR_CHECK(mdns_init());
  mdns_hostname_set(CONFIG_APP_HOSTNAME);
  mdns_instance_name_set(CONFIG_APP_FRIENDLY_NAME);
  mdns_txt_item_t txt[] = {{"path", "/sendspin"}};
  mdns_service_add(nullptr, "_sendspin", "_tcp", sendspin::SendspinClientConfig::DEFAULT_SERVER_PORT, txt, 1);
}

// ---------------------------------------------------------------------------------------------------------------
// Sendspin glue
// ---------------------------------------------------------------------------------------------------------------

class SendspinBridge : public sendspin::SendspinClientListener,
                       public sendspin::SendspinNetworkProvider,
                       public sendspin::SendspinPersistenceProvider,
                       public sendspin::PlayerRoleListener {
 public:
  bool begin() {
    A2dpOutput::Config out_cfg;
    out_cfg.local_name = CONFIG_APP_FRIENDLY_NAME;
    out_cfg.device_name = CONFIG_APP_BT_DEVICE_NAME;
#ifdef CONFIG_APP_TEST_TONE
    out_cfg.test_tone = true;
#endif
    if (!this->output_.begin(out_cfg, [this](uint32_t frames, int64_t timestamp) {
          if (this->player_ != nullptr) {
            this->player_->notify_audio_played(frames, timestamp);
          }
        })) {
      return false;
    }

    sendspin::SendspinClientConfig cfg;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    // The server matches client_id against the source MAC of the device's traffic
    cfg.client_id = mac_str;
    cfg.name = CONFIG_APP_FRIENDLY_NAME;
    cfg.product_name = "Sendspin A2DP Bridge";
    cfg.manufacturer = "DIY";
    cfg.software_version = "0.1.0";

    this->client_ = std::make_unique<sendspin::SendspinClient>(std::move(cfg));
    this->client_->set_listener(this);
    this->client_->set_network_provider(this);
    this->client_->set_persistence_provider(this);

    // A2DP (SBC) runs at a fixed 44.1 kHz, 16 bit stereo: only advertise that, so no resampling is needed
    sendspin::PlayerRoleConfig player_cfg;
    for (auto codec : {sendspin::SendspinCodecFormat::FLAC, sendspin::SendspinCodecFormat::PCM}) {
      player_cfg.audio_formats.push_back({codec, 2, A2dpOutput::SAMPLE_RATE, 16});
    }
    // Small: the more audio the server may send ahead, the larger the WiFi bursts that disturb A2DP
    player_cfg.audio_buffer_capacity = CONFIG_APP_SENDSPIN_BUFFER_SIZE;
    player_cfg.initial_static_delay_ms = CONFIG_APP_STATIC_DELAY_MS;
    // PSRAM: internal RAM is needed for the WiFi buffers (moving those out of PSRAM halved the CPU load)
    player_cfg.decode_buffer_location = sendspin::MemoryLocation::PREFER_EXTERNAL;
    this->player_ = &this->client_->add_player(player_cfg);
    this->player_->set_listener(this);
    this->player_->set_static_delay_adjustable(true);
    this->player_->update_volume(this->volume_);
    this->output_.set_volume(this->volume_);

    // Controller role: forwards the speaker's play/pause/next/previous buttons to the server
    this->controller_ = &this->client_->add_controller();

    if (!this->client_->start_server()) {
      ESP_LOGE(TAG, "Failed to start Sendspin server");
      return false;
    }
    return true;
  }

  void loop() {
    this->client_->loop();
    this->handle_speaker_controls_();
  }

 protected:
  // --- SendspinClientListener (client loop thread) ---
  void on_group_update(const sendspin::GroupUpdateObject & /*group*/) override {}
  void on_time_sync_updated(float error) override {
    // The sync task converts server timestamps with this; a bad sync turns audio into silence
    static int64_t last_log = 0;
    const int64_t now = esp_timer_get_time();
    if (now - last_log >= 60000000) {
      last_log = now;
      ESP_LOGI(TAG, "Time sync error: %.0f us (synced: %s)", error, this->client_->is_time_synced() ? "yes" : "no");
    }
  }
  // Sendspin's time sync needs low, steady WiFi latency. In modem sleep, packets for the ESP wait at the AP until
  // the next beacon (~100 ms), which left the clock sync ~440 ms off and turned all audio into silence.
  void on_request_high_performance() override {
    ESP_LOGI(TAG, "WiFi high performance (power save off)");
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
  void on_release_high_performance() override {
    ESP_LOGI(TAG, "WiFi power save on");
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  }

  // --- SendspinNetworkProvider ---
  bool is_network_ready() override { return g_network_ready.load(); }

  // --- SendspinPersistenceProvider (client loop thread) ---
  bool save_last_server_hash(uint32_t hash) override { return nvs_set("last_srv", hash); }
  std::optional<uint32_t> load_last_server_hash() override {
    uint32_t value;
    if (nvs_get("last_srv", value)) {
      return value;
    }
    return std::nullopt;
  }
  bool save_static_delay(uint16_t delay_ms) override { return nvs_set("delay", static_cast<uint32_t>(delay_ms)); }
  std::optional<uint16_t> load_static_delay() override {
    uint32_t value;
    if (nvs_get("delay", value)) {
      ESP_LOGI(TAG, "Loaded static delay: %u ms", static_cast<unsigned>(value));
      return static_cast<uint16_t>(value);
    }
    return std::nullopt;
  }

  // --- PlayerRoleListener ---
  // THREAD CONTEXT: Sendspin sync task. May block up to timeout_ms.
  size_t on_audio_write(uint8_t *data, size_t length, uint32_t timeout_ms) override {
    return this->output_.write(data, length, pdMS_TO_TICKS(timeout_ms));
  }
  void on_stream_start() override {
    this->stream_active_ = true;
    this->client_->update_state(sendspin::SendspinClientState::SYNCHRONIZED);
    this->output_.start();
  }
  void on_stream_end() override {
    this->stream_active_ = false;
    this->output_.stop();
  }
  void on_volume_changed(uint8_t volume) override {
    ESP_LOGI(TAG, "Volume set to %u%% by the server", volume);
    this->volume_ = volume;
    this->output_.set_volume(volume);
    this->player_->update_volume(volume);
  }

  // THREAD CONTEXT: client loop (the controller and player roles must be used from here)
  void handle_speaker_controls_() {
    A2dpOutput::RemoteCommand command;
    int volume;
    this->output_.poll_remote(command, volume);

    if (volume >= 0 && volume != this->volume_) {
      // Changed with the speaker's buttons: report it as the player volume so the server shows and keeps it
      this->volume_ = static_cast<uint8_t>(volume);
      this->player_->update_volume(this->volume_);
    }

    using Cmd = sendspin::SendspinControllerCommand;
    std::optional<Cmd> cmd;
    switch (command) {
      case A2dpOutput::RemoteCommand::PLAY:
      case A2dpOutput::RemoteCommand::PAUSE:
        // The A2DP stream runs all the time (keep alive), so the speaker cannot know whether music plays and its
        // play/pause choice is unreliable: toggle based on whether a Sendspin stream is running
        cmd = this->stream_active_ ? Cmd::PAUSE : Cmd::PLAY;
        break;
      case A2dpOutput::RemoteCommand::STOP:
        cmd = Cmd::STOP;
        break;
      case A2dpOutput::RemoteCommand::NEXT:
        cmd = Cmd::NEXT;
        break;
      case A2dpOutput::RemoteCommand::PREVIOUS:
        cmd = Cmd::PREVIOUS;
        break;
      case A2dpOutput::RemoteCommand::NONE:
        break;
    }
    if (cmd.has_value()) {
      ESP_LOGI(TAG, "Speaker button -> server command %d", static_cast<int>(*cmd));
      sendspin::ClientCommandControllerObject obj{};
      obj.command = *cmd;
      this->controller_->send_command(obj);
    }
  }
  void on_mute_changed(bool muted) override {
    ESP_LOGI(TAG, "Mute %s", muted ? "on" : "off");
    this->output_.set_muted(muted);
    this->player_->update_muted(muted);
  }

  static bool nvs_set(const char *key, uint32_t value) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
      return false;
    }
    const bool ok = nvs_set_u32(handle, key, value) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
  }
  static bool nvs_get(const char *key, uint32_t &value) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
      return false;
    }
    const bool ok = nvs_get_u32(handle, key, &value) == ESP_OK;
    nvs_close(handle);
    return ok;
  }

  A2dpOutput output_;
  std::unique_ptr<sendspin::SendspinClient> client_;
  sendspin::PlayerRole *player_{nullptr};
  sendspin::ControllerRole *controller_{nullptr};
  bool stream_active_{false};
  uint8_t volume_{CONFIG_APP_INITIAL_VOLUME};
};

extern "C" void app_main() {
  ESP_LOGI(TAG, "Reset reason: %d", static_cast<int>(esp_reset_reason()));
#ifdef CONFIG_APP_SENDSPIN_DEBUG_LOG
  for (const char *tag : {"sendspin.sync_task", "sendspin.player", "sendspin.client", "sendspin.time_burst"}) {
    esp_log_level_set(tag, ESP_LOG_DEBUG);
  }
#endif

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  wifi_start();
  mdns_start();

  static SendspinBridge bridge;
  if (!bridge.begin()) {
    ESP_LOGE(TAG, "Startup failed, restarting in 10 s");
    vTaskDelay(pdMS_TO_TICKS(10000));
    esp_restart();
  }

  // The client's callbacks and lifecycle run from this loop (core 1, see sdkconfig)
  while (true) {
    bridge.loop();
    vTaskDelay(pdMS_TO_TICKS(CLIENT_LOOP_INTERVAL_MS));
  }
}
