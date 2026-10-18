#pragma once

#include <cstdint>
#include <string>

#include "a2dp_output.h"

/// Player state and controls the web interface needs from the Sendspin side (implemented in main.cpp).
/// All methods are called from the web server task and must be thread-safe.
class WebUiPlayer {
 public:
  enum class Command : uint8_t { NONE, PLAY_PAUSE, NEXT, PREVIOUS };

  struct State {
    std::string title;
    std::string artist;
    std::string album;
    uint8_t volume{0};
    bool playing{false};
  };

  virtual ~WebUiPlayer() = default;
  virtual State get_state() = 0;
  virtual void request_volume(uint8_t volume) = 0;
  virtual void request_command(Command command) = 0;
};

/// Web interface on port 80, the counterpart of the ESPHome variant's web interface: Bluetooth status and pairing
/// (search, choose, unpair, re-pair), track info, volume, playback buttons, WiFi signal, free RAM, log level and log.
bool web_ui_start(A2dpOutput *output, WebUiPlayer *player);
