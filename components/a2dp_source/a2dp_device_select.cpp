#include "a2dp_device_select.h"

#ifdef USE_ESP32

namespace esphome::a2dp_source {

void A2DPDeviceSelect::set_devices(const std::string &placeholder, const std::vector<std::string> &devices) {
  this->labels_.clear();
  this->labels_.reserve(devices.size() + 1);
  this->labels_.push_back(placeholder);
  this->labels_.insert(this->labels_.end(), devices.begin(), devices.end());
  FixedVector<const char *> options;
  options.init(this->labels_.size());
  for (const auto &label : this->labels_) {
    options.push_back(label.c_str());
  }
  this->traits.set_options(options);
  this->publish_state(static_cast<size_t>(0));
}

void A2DPDeviceSelect::show(size_t index) {
  if (this->has_index(index)) {
    this->publish_state(index);
  }
}

void A2DPDeviceSelect::control(size_t index) {
  if (index == 0) {
    this->publish_state(index);
    return;
  }
  this->publish_state(index);
  this->parent_->pair_found_device(index - 1);
}

}  // namespace esphome::a2dp_source

#endif  // USE_ESP32
