#pragma once

// Hardware facts for the diagnostics of both variants (native web interface, ESPHome template sensors via
// esphome: includes). Header-only so the ESPHome build can include it without a component.

#include <cinttypes>
#include <cstdio>
#include <string>

#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_private/esp_clk.h>
#include <esp_system.h>
#include <sdkconfig.h>
#if CONFIG_SPIRAM
#include <esp_psram.h>
#endif

namespace device_info {

/// e.g. "ESP32 rev3.1, 2 cores, 240 MHz"
inline std::string chip() {
  esp_chip_info_t info;
  esp_chip_info(&info);
  const char *model = "ESP32";
  switch (info.model) {
    case CHIP_ESP32:
      model = "ESP32";
      break;
    default:
      model = "ESP32 (other)";
      break;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%s rev%u.%u, %u cores, %d MHz", model, info.revision / 100, info.revision % 100,
           info.cores, esp_clk_cpu_freq() / 1000000);
  return buf;
}

/// e.g. "Flash 8 MB, PSRAM 8 MB (4 MB usable)": physical sizes; the ESP32 maps at most 4 MB of PSRAM into the heap
inline std::string memory() {
  uint32_t flash = 0;
  if (esp_flash_get_physical_size(nullptr, &flash) != ESP_OK) {
    esp_flash_get_size(nullptr, &flash);  // Configured size
  }
  size_t psram = 0;
#if CONFIG_SPIRAM
  psram = esp_psram_get_size();
#endif
  const size_t usable = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  char buf[64];
  if (psram == 0) {
    snprintf(buf, sizeof(buf), "Flash %" PRIu32 " MB, no PSRAM", flash >> 20);
  } else if (usable + (512u << 10) < psram) {
    snprintf(buf, sizeof(buf), "Flash %" PRIu32 " MB, PSRAM %u MB (%u MB usable)", flash >> 20,
             static_cast<unsigned>(psram >> 20), static_cast<unsigned>((usable + (512u << 10)) >> 20));
  } else {
    snprintf(buf, sizeof(buf), "Flash %" PRIu32 " MB, PSRAM %u MB", flash >> 20, static_cast<unsigned>(psram >> 20));
  }
  return buf;
}

/// Free PSRAM in bytes (0 without PSRAM)
inline size_t free_psram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

/// Why the chip last started
inline const char *reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "Power on";
    case ESP_RST_EXT:
      return "External pin";
    case ESP_RST_SW:
      return "Software restart";
    case ESP_RST_PANIC:
      return "Crash (exception/panic)";
    case ESP_RST_INT_WDT:
      return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "Task watchdog";
    case ESP_RST_WDT:
      return "Other watchdog";
    case ESP_RST_DEEPSLEEP:
      return "Wake from deep sleep";
    case ESP_RST_BROWNOUT:
      return "Brownout (supply voltage)";
    case ESP_RST_SDIO:
      return "SDIO";
    default:
      return "Unknown";
  }
}

/// Track position, e.g. "3:07" or "1:02:05"
inline std::string track_time(uint32_t ms) {
  const unsigned t = ms / 1000;
  char buf[16];
  if (t >= 3600) {
    snprintf(buf, sizeof(buf), "%u:%02u:%02u", t / 3600, t / 60 % 60, t % 60);
  } else {
    snprintf(buf, sizeof(buf), "%u:%02u", t / 60, t % 60);
  }
  return buf;
}

/// e.g. "3d 04:12:05"
inline std::string uptime(int64_t seconds) {
  char buf[32];
  const unsigned days = static_cast<unsigned>(seconds / 86400);
  const unsigned rest = static_cast<unsigned>(seconds % 86400);
  if (days > 0) {
    snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u", days, rest / 3600, rest / 60 % 60, rest % 60);
  } else {
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", rest / 3600, rest / 60 % 60, rest % 60);
  }
  return buf;
}

}  // namespace device_info
