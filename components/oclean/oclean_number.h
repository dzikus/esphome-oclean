#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

// esphome.h pulls in every component header, so guard the platform here too
#if defined(USE_ESP32) && defined(USE_NUMBER)
#include <algorithm>
#include <cmath>
#include <vector>

#include "esphome/components/number/number.h"
#include "esphome/core/preferences.h"
#include "oclean.h"

namespace esphome::oclean {

// Days, sent as a two-byte big-endian payload after the opcode.
class OcleanHeadDaysNumber : public number::Number, public Parented<OcleanHub> {
 protected:
  void control(float value) override {
    // lroundf(Inf/NaN) is undefined
    if (!std::isfinite(value))
      return;
    float const r = std::max(0.0f, std::min(static_cast<float>(UINT16_MAX), roundf(value)));
    auto const v = static_cast<uint16_t>(r);
    std::vector<uint8_t> cmd = {0x02, 0x17, static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xFF)};
    // a dropped write must not publish, same rule as the command switches
    if (this->parent_->send_command(std::move(cmd), "head-max-days"))
      this->publish_state(value);
  }
};

// One step parameter of the runtime custom program (kind 0 = gear, 1 =
// duration). Flash-backed and local: on its own it writes nothing to the brush,
// but while the custom scheme is selected the hub debounces a program rewrite.
class OcleanCustomParamNumber : public number::Number, public Component, public Parented<OcleanHub> {
 public:
  void set_param(uint8_t kind, uint8_t index) {
    this->kind_ = kind;
    this->index_ = index;
  }
  void set_initial(float v) { this->initial_ = v; }

  void setup() override {
    // Salted, because auto-created entities on two hubs share an object-id hash
    // and would land in one flash slot. Not make_entity_preference(): its key
    // mixes in the device id, which is empty on a config without sub-devices.
    // The brush MAC always separates them.
    this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash() ^ this->parent_->pref_salt());
    float v = this->initial_;
    this->pref_.load(&v);
    // flash can hold garbage from an older layout
    if (!std::isfinite(v))
      v = this->initial_;
    v = this->clamp_to_range_(v);
    this->publish_state(v);
    this->parent_->set_custom_scheme_param(this->kind_, this->index_, clamp_u8_(v), false);
  }

 protected:
  void control(float value) override {
    // lroundf(Inf/NaN) is undefined, and a NaN in flash leaves the entity
    // stateless after a reboot
    if (!std::isfinite(value))
      return;
    this->pref_.save(&value);
    this->publish_state(value);
    this->parent_->set_custom_scheme_param(this->kind_, this->index_, clamp_u8_(value), true);
  }

  static uint8_t clamp_u8_(float v) {
    return static_cast<uint8_t>(std::max(0.0f, std::min(static_cast<float>(UINT8_MAX), roundf(v))));
  }

  float clamp_to_range_(float v) {
    float const lo = this->traits.get_min_value();
    float const hi = this->traits.get_max_value();
    return v < lo ? lo : (v > hi ? hi : v);
  }

  uint8_t kind_{0};
  uint8_t index_{0};
  float initial_{0};
  ESPPreferenceObject pref_;
};

}  // namespace esphome::oclean

#endif  // USE_ESP32 && USE_NUMBER
