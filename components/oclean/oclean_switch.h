#pragma once

#include "esphome/core/component.h"

#ifdef USE_ESP32
#include "esphome/components/switch/switch.h"

#include <string>
#include <vector>

#include "oclean.h"

namespace esphome {
namespace oclean {

// Two-byte opcode plus an on/off value. Published optimistically, then
// corrected by the settings readback on the same poll.
class OcleanCommandSwitch : public switch_::Switch, public Parented<OcleanHub> {
 public:
  void set_opcode(uint8_t b0, uint8_t b1) {
    this->b0_ = b0;
    this->b1_ = b1;
  }
  void set_values(uint8_t on_value, uint8_t off_value) {
    this->on_value_ = on_value;
    this->off_value_ = off_value;
  }
  void set_label(const std::string &label) { this->label_ = label; }

 protected:
  void write_state(bool state) override {
    std::vector<uint8_t> cmd =
        build_toggle_command(this->b0_, this->b1_, this->on_value_, this->off_value_, state);
    // a dropped write must leave the switch on its last real state, not lie
    if (this->parent_->send_command(std::move(cmd), this->label_))
      this->publish_state(state);
  }

  uint8_t b0_{0};
  uint8_t b1_{0};
  uint8_t on_value_{0x01};
  uint8_t off_value_{0x00};
  std::string label_{"switch"};
};

// Local only, nothing reaches the brush: off frees it for the official app.
// The restored state has to be applied from a deferred call because
// BLEClient::setup() runs later (AFTER_BLUETOOTH) and re-enables the client.
class OcleanBleSwitch : public switch_::Switch,
                        public Parented<OcleanHub>,
                        public Component {
 public:
  void setup() override {
    bool state = this->get_initial_state_with_restore_mode().value_or(true);
    this->publish_state(state);
    this->defer([this, state]() { this->parent_->set_ble_user_enabled(state); });
  }

 protected:
  void write_state(bool state) override {
    this->parent_->set_ble_user_enabled(state);
    this->publish_state(state);
  }
};

}  // namespace oclean
}  // namespace esphome

#endif  // USE_ESP32
