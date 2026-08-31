#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

// esphome.h pulls in every component header, so guard the platform here too
#if defined(USE_ESP32) && defined(USE_SELECT)
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/select/select.h"
#include "oclean.h"
#include "oclean_protocol.h"

namespace esphome {
namespace oclean {

// Each option is a whole program, so selecting one rewrites every step on the
// brush (0206, with a 020B continuation when it does not fit one write).
// Optimistic on actuation, corrected by publish_pnum on the next settings read.
class OcleanSchemeSelect : public select::Select, public Parented<OcleanHub> {
 public:
  // flat_steps is [gear, duration, ...]; name must match an option passed to
  // set_options at codegen time, or control() will never find it
  void add_scheme(uint8_t pnum, const std::string &name, const std::vector<uint8_t> &flat_steps) {
    Scheme sc;
    sc.pnum = pnum;
    sc.name = name;
    for (size_t i = 0; i + 1 < flat_steps.size(); i += 2) {
      sc.steps.push_back(SchemeStep{flat_steps[i], flat_steps[i + 1]});
    }
    this->schemes_.push_back(std::move(sc));
  }

  // the one option whose program comes from live parameters, not a fixed table
  void set_custom_option(uint8_t pnum, const std::string &name) {
    this->custom_pnum_ = pnum;
    this->custom_name_ = name;
  }

  // kind 0 = gear, 1 = duration; the program is always CUSTOM_STEPS long so the
  // brush keeps its quadrant guidance
  void set_custom_param(uint8_t kind, uint8_t index, uint8_t value) {
    if (kind == 0 && index < CUSTOM_STEPS) {
      this->custom_gears_[index] = value;
    } else if (kind == 1 && index < CUSTOM_STEPS) {
      this->custom_durations_[index] = value;
    }
  }

  // True when the custom option is the currently selected scheme.
  bool custom_active() const {
    return !this->custom_name_.empty() && this->has_state() && this->current_option() == this->custom_name_;
  }

  // false when nothing was queued (BLE off or queue full)
  bool send_custom_program() {
    if (this->custom_name_.empty())
      return false;
    std::vector<SchemeStep> steps;
    for (uint8_t i = 0; i < CUSTOM_STEPS; i++) {
      steps.push_back(SchemeStep{this->custom_gears_[i], this->custom_durations_[i]});
    }
    return this->send_program_(this->custom_pnum_, steps);
  }

  // empty when the id is not one of ours: a session recorded under a scheme
  // that has since been renamed or dropped from the yaml
  std::string name_for_pnum(uint8_t pnum) const {
    if (!this->custom_name_.empty() && pnum == this->custom_pnum_)
      return this->custom_name_;
    for (const auto &sc : this->schemes_) {
      if (sc.pnum == pnum)
        return sc.name;
    }
    return "";
  }

  // settings readback; an unknown pNum leaves the last state alone
  void publish_pnum(uint8_t pnum) {
    std::string name = this->name_for_pnum(pnum);
    if (!name.empty())
      this->publish_state(name);
  }

 protected:
  static const uint8_t CUSTOM_STEPS = 4;

  // a half-queued split program counts as not queued
  bool send_program_(uint8_t pnum, const std::vector<SchemeStep> &steps) {
    auto packets = build_scheme_packets(pnum, steps);
    bool queued = !packets.empty();
    for (size_t i = 0; i < packets.size(); i++) {
      const char *label = "brush-scheme";
      if (packets.size() > 1)
        label = (i == 0) ? "brush-scheme-a" : "brush-scheme-b";
      if (!this->parent_->send_command(packets[i], label))
        queued = false;
    }
    return queued;
  }

  void control(const std::string &value) override {
    // publish only what was queued, same rule as the command switches
    if (!this->custom_name_.empty() && value == this->custom_name_) {
      if (this->send_custom_program())
        this->publish_state(value);
      return;
    }
    for (const auto &sc : this->schemes_) {
      if (sc.name != value)
        continue;
      if (this->send_program_(sc.pnum, sc.steps))
        this->publish_state(value);
      return;
    }
  }

  struct Scheme {
    uint8_t pnum;
    std::string name;
    std::vector<SchemeStep> steps;
  };
  std::vector<Scheme> schemes_;

  uint8_t custom_pnum_{0};
  std::string custom_name_;
  // mirror the number entities' initials until those restore from flash
  uint8_t custom_gears_[CUSTOM_STEPS]{8, 8, 8, 8};
  uint8_t custom_durations_[CUSTOM_STEPS]{30, 30, 30, 30};
};

// Writes the id shown on the brush display (0216), optimistic then corrected
// from settings buffer 31.
class OcleanLanguageSelect : public select::Select, public Parented<OcleanHub> {
 public:
  void add_language(uint8_t id, const std::string &name) { this->languages_.push_back(Language{id, name}); }

  // an unknown id leaves the last state alone
  void publish_language(uint8_t id) {
    for (const auto &lang : this->languages_) {
      if (lang.id != id)
        continue;
      this->publish_state(lang.name);
      return;
    }
  }

 protected:
  void control(const std::string &value) override {
    for (const auto &lang : this->languages_) {
      if (lang.name != value)
        continue;
      if (this->parent_->send_command(build_language_command(lang.id), "device-language"))
        this->publish_state(value);
      return;
    }
  }

  struct Language {
    uint8_t id;
    std::string name;
  };
  std::vector<Language> languages_;
};

}  // namespace oclean
}  // namespace esphome

#endif  // USE_ESP32 && USE_SELECT
