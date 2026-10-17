#pragma once

#ifdef USE_ESP32

#include <string>
#include <vector>

#include "esphome/components/select/select.h"
#include "esphome/core/helpers.h"

#include "a2dp_source_speaker.h"

namespace esphome::a2dp_source {

/// Offers the speakers found by A2DPSourceSpeaker::scan(); choosing one pairs it.
///
/// The first option is a placeholder that does nothing. The options change at runtime, which the web interface only
/// picks up when the page is loaded again (web/pairing.js reloads it after a search).
class A2DPDeviceSelect : public select::Select, public Parented<A2DPSourceSpeaker> {
 public:
  /// Replaces the options with `placeholder` followed by `devices` and selects the placeholder (main loop only)
  void set_devices(const std::string &placeholder, const std::vector<std::string> &devices);
  /// Selects the option `index` (0 = placeholder) without pairing
  void show(size_t index);

 protected:
  void control(size_t index) override;

  std::vector<std::string> labels_;  // Storage for the option strings the traits point to
};

}  // namespace esphome::a2dp_source

#endif  // USE_ESP32
