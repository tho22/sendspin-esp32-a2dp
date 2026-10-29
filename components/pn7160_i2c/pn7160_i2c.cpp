#include "pn7160_i2c.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome::pn7160_i2c {

static const char *const TAG = "pn7160_i2c";

static constexpr uint32_t IRQ_CLEAR_TIMEOUT_US = 2000;

uint8_t PN7160I2C::read_nfcc(nfc::NciMessage &rx, const uint16_t timeout) {
  if (this->wait_for_irq_(timeout) != nfc::STATUS_OK) {
    ESP_LOGV(TAG, "read_nfcc_() timeout waiting for IRQ");
    return nfc::STATUS_FAILED;
  }

  rx.get_message().resize(nfc::NCI_PKT_HEADER_SIZE);
  if (!this->read_bytes_raw(rx.get_message().data(), nfc::NCI_PKT_HEADER_SIZE)) {
    return nfc::STATUS_FAILED;
  }

  uint8_t length = rx.get_payload_size();
  if (length > 0) {
    rx.get_message().resize(length + nfc::NCI_PKT_HEADER_SIZE);
    if (!this->read_bytes_raw(rx.get_message().data() + nfc::NCI_PKT_HEADER_SIZE, length)) {
      return nfc::STATUS_FAILED;
    }
  }
  // Wait for the IRQ line to clear, so the next read waits for a new message. If it stays high, the NFCC already
  // queued the next message (e.g. tag data right after the credits notification), which is no error
  const uint32_t start = micros();
  while (this->irq_pin_->digital_read()) {
    if (micros() - start >= IRQ_CLEAR_TIMEOUT_US) {
      ESP_LOGVV(TAG, "IRQ still high after read, next message pending");
      break;
    }
  }
  return nfc::STATUS_OK;
}

uint8_t PN7160I2C::write_nfcc(nfc::NciMessage &tx) {
  auto encoded = tx.encode();
  if (this->write(encoded.data(), encoded.size()) == i2c::ERROR_OK) {
    return nfc::STATUS_OK;
  }
  return nfc::STATUS_FAILED;
}

void PN7160I2C::dump_config() {
  PN7160::dump_config();
  LOG_I2C_DEVICE(this);
}

}  // namespace esphome::pn7160_i2c
