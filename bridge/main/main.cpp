// Bluetooth half of the two-chip setup: receives PCM over I2S from the WiFi side (Sendspin) and streams it to an
// A2DP speaker. WiFi stays off, so Bluetooth has the radio to itself (WiFi/Bluetooth coexistence caused the speaker
// stalls of the single-chip firmware).
//
// Wiring (bridge = I2S master, 44.1 kHz, 16 bit stereo, Philips):
//   BCLK, WS  bridge -> WiFi side
//   DIN       WiFi side -> bridge
//   UART      bridge TX -> WiFi side RX, bridge RX <- WiFi side TX, common GND
//
// Link protocol: ASCII lines terminated by '\n', 115200 8N1. The same commands are accepted on the USB console for
// testing without the WiFi side; events are logged there as "-> EVENT".
//
// Commands (WiFi side -> bridge):
//   PING                 answered with PONG
//   STATUS               sends READY, CONNECTED, SEARCHING, STATUS and DEVICES again
//   VOL <0-100>          playback volume (AVRCP absolute volume on the speaker if supported, else digital gain)
//   MUTE <0|1>
//   LOCK <0|1>           volume lock: the speaker's own volume changes are reverted to VOL and not reported
//   SCAN                 search for speakers ready to pair; the result follows as DEVICES
//   PAIR <index>         pair the speaker at <index> of the last DEVICES list
//   UNPAIR               forget the speaker, no connection attempts until the next pairing
//   REPAIR               forget the speaker and search for the configured speaker name
//   LOG <NONE|ERROR|WARN|INFO|DEBUG|VERBOSE>   log level of the bridge's USB console
//
// Events (bridge -> WiFi side):
//   READY <version>      after boot; the WiFi side then sends VOL, MUTE and LOCK
//   CONNECTED <0|1>      A2DP link to the speaker
//   SEARCHING <0|1>      a speaker search is running
//   STATUS <text>        human readable link/pairing state
//   DEVICES <n>          result of the last search, followed by n lines DEVICE <index> <name (address)>
//   BUTTON <PLAY|PAUSE|STOP|NEXT|PREVIOUS>   button pressed on the speaker
//   VOLUME <0-100>       volume changed on the speaker; the WiFi side reports it and sends it back with VOL
//   OK | ERR <reason>    answer to a command

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "a2dp_output.h"

static const char *const TAG = "bridge";
static const char *const VERSION = "0.1.0";

static constexpr uart_port_t LINK_UART = UART_NUM_1;
static constexpr uart_port_t CONSOLE_UART = UART_NUM_0;
static constexpr size_t MAX_LINE = 128;
static constexpr uint32_t LINK_INTERVAL_MS = 20;

// I2S chunks of 240 frames (5.4 ms); 6 DMA buffers give 33 ms of slack for the audio task
static constexpr size_t CHUNK_FRAMES = 240;
static constexpr uint32_t DMA_DESC_NUM = 6;

// Fill level control. The Bluetooth stack pulls in bursts, so the level is smoothed first. The I2S clock and the
// stack's pull pacing both come from the bridge's crystal, so the level only drifts slowly; it is corrected one frame
// at a time. Large deviations (stream start, underrun) are corrected in whole chunks.
static constexpr float FILL_SMOOTHING = 1.0f / 128.0f;  // ~0.7 s at 5.4 ms per chunk
static constexpr float FINE_START_MS = 3.0f;             // Start dropping/repeating single frames
static constexpr float FINE_STOP_MS = 1.0f;              // Stop again once this close to the target
static constexpr float COARSE_MS = 40.0f;                // Drop or insert whole chunks beyond this

static A2dpOutput g_output;
static i2s_chan_handle_t g_i2s_rx = nullptr;

static float bytes_to_ms(size_t bytes) {
  return static_cast<float>(bytes / A2dpOutput::BYTES_PER_FRAME) * 1000.0f / A2dpOutput::SAMPLE_RATE;
}

// ---------------------------------------------------------------------------------------------------------------
// Audio: I2S in -> A2DP ring buffer at a constant fill level
// ---------------------------------------------------------------------------------------------------------------

static bool i2s_start() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = DMA_DESC_NUM;
  chan_cfg.dma_frame_num = CHUNK_FRAMES;
  esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &g_i2s_rx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S channel failed: %s", esp_err_to_name(err));
    return false;
  }
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(A2dpOutput::SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = static_cast<gpio_num_t>(CONFIG_BRIDGE_I2S_BCLK_GPIO),
              .ws = static_cast<gpio_num_t>(CONFIG_BRIDGE_I2S_WS_GPIO),
              .dout = I2S_GPIO_UNUSED,
              .din = static_cast<gpio_num_t>(CONFIG_BRIDGE_I2S_DIN_GPIO),
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  // The audio PLL hits 44.1 kHz far more exactly than the default 160 MHz divider
  std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
  if ((err = i2s_channel_init_std_mode(g_i2s_rx, &std_cfg)) != ESP_OK) {
    ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
    return false;
  }
  // Without the WiFi side the data line would float and turn into loud noise: read silence instead
  gpio_pulldown_en(static_cast<gpio_num_t>(CONFIG_BRIDGE_I2S_DIN_GPIO));
  if ((err = i2s_channel_enable(g_i2s_rx)) != ESP_OK) {
    ESP_LOGE(TAG, "I2S enable failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "I2S master: BCLK %d, WS %d, DIN %d", CONFIG_BRIDGE_I2S_BCLK_GPIO, CONFIG_BRIDGE_I2S_WS_GPIO,
           CONFIG_BRIDGE_I2S_DIN_GPIO);
  return true;
}

static void audio_task(void * /*params*/) {
  static int16_t samples[(CHUNK_FRAMES + 1) * 2];
  static const int16_t silence[CHUNK_FRAMES * 2] = {};
  const float target_ms = CONFIG_BRIDGE_TARGET_FILL_MS;
  float fill_ms = target_ms;  // Smoothed level
  bool running = false;
  int correcting = 0;  // -1 repeat frames, 1 drop frames, 0 none
  uint32_t dropped_frames = 0;
  uint32_t repeated_frames = 0;
  uint32_t coarse_corrections = 0;
  uint32_t chunks = 0;

  while (true) {
    size_t bytes = 0;
    if (i2s_channel_read(g_i2s_rx, samples, CHUNK_FRAMES * A2dpOutput::BYTES_PER_FRAME, &bytes, portMAX_DELAY) !=
        ESP_OK) {
      continue;
    }
#ifdef CONFIG_BRIDGE_TEST_TONE
    continue;  // The output plays its test tone while it is not streaming
#endif
    if (!g_output.is_stream_started()) {
      if (running) {
        running = false;
        g_output.stop();  // Drops what is left while the stack does not pull
      }
      continue;
    }
    if (!running) {
      // New stream: start from an empty buffer filled with silence up to the target, so the latency is the same on
      // every connection
      running = true;
      correcting = 0;
      g_output.start();
      size_t prefill = static_cast<size_t>(target_ms * A2dpOutput::SAMPLE_RATE / 1000) * A2dpOutput::BYTES_PER_FRAME;
      while (prefill > 0) {
        const size_t part = std::min(prefill, sizeof(silence));
        g_output.write(reinterpret_cast<const uint8_t *>(silence), part, 0);
        prefill -= part;
      }
      fill_ms = target_ms;
    }

    size_t frames = bytes / A2dpOutput::BYTES_PER_FRAME;
    fill_ms += (bytes_to_ms(g_output.get_buffered_bytes()) - fill_ms) * FILL_SMOOTHING;
    const float error_ms = fill_ms - target_ms;
    const float chunk_ms = bytes_to_ms(bytes);

    if (error_ms > COARSE_MS) {
      // Far too much buffered: skip this chunk
      fill_ms -= chunk_ms;
      coarse_corrections++;
      continue;
    }
    if (error_ms < -COARSE_MS) {
      // Far too little (e.g. after an underrun): insert a chunk of silence
      g_output.write(reinterpret_cast<const uint8_t *>(silence), bytes, 0);
      fill_ms += chunk_ms;
      coarse_corrections++;
    }

    if (correcting == 0 && std::abs(error_ms) > FINE_START_MS) {
      correcting = error_ms > 0 ? 1 : -1;
    } else if (correcting != 0 && std::abs(error_ms) < FINE_STOP_MS) {
      correcting = 0;
    }
    if (correcting > 0 && frames > 1) {
      frames--;  // Drop the last frame
      dropped_frames++;
    } else if (correcting < 0 && frames > 0) {
      samples[frames * 2] = samples[frames * 2 - 2];  // Repeat the last frame
      samples[frames * 2 + 1] = samples[frames * 2 - 1];
      frames++;
      repeated_frames++;
    }
    g_output.write(reinterpret_cast<const uint8_t *>(samples), frames * A2dpOutput::BYTES_PER_FRAME, 0);

    // Diagnostics every ~30 s
    if (++chunks >= 30 * A2dpOutput::SAMPLE_RATE / CHUNK_FRAMES) {
      chunks = 0;
      ESP_LOGI(TAG, "Buffer: %.1f ms (target %.0f), %u frames dropped, %u repeated, %u coarse corrections", fill_ms,
               target_ms, static_cast<unsigned>(dropped_frames), static_cast<unsigned>(repeated_frames),
               static_cast<unsigned>(coarse_corrections));
      dropped_frames = repeated_frames = coarse_corrections = 0;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Link: commands from the WiFi side (or the USB console), events back
// ---------------------------------------------------------------------------------------------------------------

static void send_event(const std::string &line) {
  const std::string out = line + "\n";
  uart_write_bytes(LINK_UART, out.data(), out.size());
  ESP_LOGI("link", "-> %s", line.c_str());
}

static void send_devices() {
  const auto devices = g_output.get_found_devices();
  send_event("DEVICES " + std::to_string(devices.size()));
  for (size_t i = 0; i < devices.size(); i++) {
    send_event("DEVICE " + std::to_string(i) + " " + devices[i]);
  }
}

static bool parse_int(const std::string &text, int min, int max, int &value) {
  char *end = nullptr;
  const long parsed = strtol(text.c_str(), &end, 10);
  if (text.empty() || *end != '\0' || parsed < min || parsed > max) {
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

static void handle_command(const std::string &line) {
  ESP_LOGI("link", "<- %s", line.c_str());
  const size_t space = line.find(' ');
  std::string cmd = line.substr(0, space);
  const std::string arg = space == std::string::npos ? "" : line.substr(space + 1);
  std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
  int value = 0;

  if (cmd == "PING") {
    send_event("PONG");
    return;
  }
  if (cmd == "STATUS") {
    send_event(std::string("READY ") + VERSION);
    send_event(std::string("CONNECTED ") + (g_output.is_connected() ? "1" : "0"));
    send_event(std::string("SEARCHING ") + (g_output.is_searching() ? "1" : "0"));
    send_event("STATUS " + g_output.get_status_text());
    send_devices();
    return;
  }
  if (cmd == "VOL" && parse_int(arg, 0, 100, value)) {
    g_output.set_volume(static_cast<uint8_t>(value));
  } else if (cmd == "MUTE" && parse_int(arg, 0, 1, value)) {
    g_output.set_muted(value != 0);
  } else if (cmd == "LOCK" && parse_int(arg, 0, 1, value)) {
    g_output.set_volume_lock(value != 0);
  } else if (cmd == "SCAN" && arg.empty()) {
    g_output.request_scan();
  } else if (cmd == "PAIR" && parse_int(arg, 0, 99, value)) {
    if (static_cast<size_t>(value) >= g_output.get_found_devices().size()) {
      send_event("ERR no such device");
      return;
    }
    g_output.request_pair(static_cast<size_t>(value));
  } else if (cmd == "UNPAIR" && arg.empty()) {
    g_output.request_unpair();
  } else if (cmd == "REPAIR" && arg.empty()) {
    g_output.request_repair();
  } else if (cmd == "LOG") {
    static const char *const LEVELS[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
    std::string level = arg;
    std::transform(level.begin(), level.end(), level.begin(), ::toupper);
    const auto *it = std::find_if(std::begin(LEVELS), std::end(LEVELS), [&](const char *l) { return level == l; });
    if (it == std::end(LEVELS)) {
      send_event("ERR unknown log level");
      return;
    }
    esp_log_level_set("*", static_cast<esp_log_level_t>(it - std::begin(LEVELS)));
  } else {
    send_event("ERR unknown command");
    return;
  }
  send_event("OK");
}

// Collects bytes into lines; ignores '\r' so terminals sending CRLF work too
struct LineReader {
  std::string buffer;
  void feed(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
      const char c = static_cast<char>(data[i]);
      if (c == '\n') {
        if (!this->buffer.empty()) {
          handle_command(this->buffer);
        }
        this->buffer.clear();
      } else if (c != '\r' && this->buffer.size() < MAX_LINE) {
        this->buffer += c;
      }
    }
  }
};

static void link_start() {
  const uart_config_t cfg = {
      .baud_rate = CONFIG_BRIDGE_UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .source_clk = UART_SCLK_DEFAULT,
      .flags = {},
  };
  ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 1024, 2048, 0, nullptr, 0));
  ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(LINK_UART, CONFIG_BRIDGE_UART_TX_GPIO, CONFIG_BRIDGE_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
  // An open RX line (WiFi side not connected) must not produce garbage commands
  gpio_pullup_en(static_cast<gpio_num_t>(CONFIG_BRIDGE_UART_RX_GPIO));
  // Console input; the log output keeps going through the ROM/VFS path
  ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, nullptr, 0));
  ESP_LOGI(TAG, "Link UART: TX %d, RX %d, %d baud", CONFIG_BRIDGE_UART_TX_GPIO, CONFIG_BRIDGE_UART_RX_GPIO,
           CONFIG_BRIDGE_UART_BAUD);
}

static void link_task(void * /*params*/) {
  LineReader link_reader;
  LineReader console_reader;
  uint8_t data[64];
  int last_connected = -1;
  int last_searching = -1;
  std::string last_status;

  send_event(std::string("READY ") + VERSION);
  while (true) {
    int n = uart_read_bytes(LINK_UART, data, sizeof(data), pdMS_TO_TICKS(LINK_INTERVAL_MS));
    if (n > 0) {
      link_reader.feed(data, n);
    }
    n = uart_read_bytes(CONSOLE_UART, data, sizeof(data), 0);
    if (n > 0) {
      console_reader.feed(data, n);
    }

    const int connected = g_output.is_connected() ? 1 : 0;
    if (connected != last_connected) {
      last_connected = connected;
      send_event("CONNECTED " + std::to_string(connected));
    }
    const int searching = g_output.is_searching() ? 1 : 0;
    if (searching != last_searching) {
      if (last_searching == 1) {
        send_devices();
      }
      last_searching = searching;
      send_event("SEARCHING " + std::to_string(searching));
    }
    const std::string status = g_output.get_status_text();
    if (status != last_status) {
      last_status = status;
      send_event("STATUS " + status);
    }

    A2dpOutput::RemoteCommand command;
    int volume;
    g_output.poll_remote(command, volume);
    if (volume >= 0) {
      send_event("VOLUME " + std::to_string(volume));
    }
    switch (command) {
      case A2dpOutput::RemoteCommand::PLAY:
        send_event("BUTTON PLAY");
        break;
      case A2dpOutput::RemoteCommand::PAUSE:
        send_event("BUTTON PAUSE");
        break;
      case A2dpOutput::RemoteCommand::STOP:
        send_event("BUTTON STOP");
        break;
      case A2dpOutput::RemoteCommand::NEXT:
        send_event("BUTTON NEXT");
        break;
      case A2dpOutput::RemoteCommand::PREVIOUS:
        send_event("BUTTON PREVIOUS");
        break;
      case A2dpOutput::RemoteCommand::NONE:
        break;
    }
  }
}

extern "C" void app_main() {
  ESP_LOGI(TAG, "Sendspin Bluetooth bridge %s, reset reason %d", VERSION, static_cast<int>(esp_reset_reason()));

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  A2dpOutput::Config cfg;
  cfg.local_name = CONFIG_BRIDGE_BT_NAME;
  cfg.device_name = CONFIG_BRIDGE_BT_DEVICE_NAME;
  cfg.buffer_ms = CONFIG_BRIDGE_BUFFER_MS;
#ifdef CONFIG_BRIDGE_TEST_TONE
  cfg.test_tone = true;
#endif
  // No progress callback: the WiFi side keeps its own playback clock at the I2S output
  if (!g_output.begin(cfg, nullptr) || !i2s_start()) {
    ESP_LOGE(TAG, "Startup failed, restarting in 10 s");
    vTaskDelay(pdMS_TO_TICKS(10000));
    esp_restart();
  }
  g_output.set_volume(CONFIG_BRIDGE_INITIAL_VOLUME);

  link_start();
  xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 10, nullptr, 1);
  xTaskCreatePinnedToCore(link_task, "link", 4096, nullptr, 4, nullptr, 1);
}
