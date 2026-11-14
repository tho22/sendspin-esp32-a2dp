#pragma once

// Seek for the ESPHome variant (esphome: includes). ESPHome's Sendspin hub only sends controller commands without a
// position (SendspinHub::send_client_command), so this reaches its controller role directly to send Sendspin's
// "seek" command. If a later ESPHome renames the hub's member, this fails to compile (no silent breakage).

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "esphome/components/sendspin/sendspin_hub.h"
#include "esphome/core/log.h"

namespace sendspin_seek {

using Hub = esphome::sendspin_::SendspinHub;
using ControllerPtr = sendspin::ControllerRole *Hub::*;

// Access to the protected SendspinHub::controller_role_ (the hub is final, so no subclass). An explicit template
// instantiation may name protected members; the friend function hands the member pointer out.
struct ControllerTag {
  friend ControllerPtr controller_member(ControllerTag);
};
template<ControllerPtr Member> struct ControllerAccess {
  friend ControllerPtr controller_member(ControllerTag) { return Member; }
};
template struct ControllerAccess<&Hub::controller_role_>;
ControllerPtr controller_member(ControllerTag);

// What the server offers, from its controller state (main loop)
inline bool g_supported = false;
inline uint32_t g_max_ms = 0;  // 0: unknown

/// Call once at boot: follows whether the server offers seeking and up to where
inline void track(Hub *hub) {
  hub->add_controller_state_callback([](const sendspin::ServerStateControllerObject &state) {
    const auto &commands = state.supported_commands;
    g_supported = std::find(commands.begin(), commands.end(), sendspin::SendspinControllerCommand::SEEK) !=
                  commands.end();
    g_max_ms = state.seek_max_ms.value_or(0);
  });
  hub->add_controller_state_clear_callback([]() {
    g_supported = false;
    g_max_ms = 0;
  });
}

/// Seeks to position_ms of the current track; false if the server does not offer seeking right now
inline bool seek(Hub *hub, uint32_t position_ms) {
  if (!g_supported || !hub->is_ready()) {
    ESP_LOGW("seek", "The server does not offer seeking for this track");
    return false;
  }
  if (g_max_ms > 0) {
    position_ms = std::min(position_ms, g_max_ms);
  }
  sendspin::ControllerRole *controller = hub->*controller_member(ControllerTag{});
  if (controller == nullptr) {
    return false;
  }
  ESP_LOGI("seek", "Seeking to %u ms", static_cast<unsigned>(position_ms));
  sendspin::ClientCommandControllerObject command{};
  command.command = sendspin::SendspinControllerCommand::SEEK;
  command.position_ms = position_ms;
  controller->send_command(command);
  return true;
}

}  // namespace sendspin_seek
