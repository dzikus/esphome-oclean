#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

// esphome.h pulls in every component header, so guard the platform here too
#if defined(USE_ESP32) && defined(USE_BUTTON)
#include "esphome/components/button/button.h"
#include "oclean.h"

namespace esphome {
namespace oclean {

// Downloads the buffered sessions and holds the link open long enough to catch
// the whole record stream in the log.
class OcleanCaptureButton : public button::Button, public Parented<OcleanHub> {
 public:
  void press_action() override { this->parent_->trigger_session_capture(); }
};

// Irreversible: zeroes the head age the brush accumulated, restarting the
// replacement reminder.
class OcleanResetHeadButton : public button::Button, public Parented<OcleanHub> {
 public:
  void press_action() override { this->parent_->send_command({0x02, 0x0F}, "reset-brush-head"); }
};

// A mutation, so it fires on press only: never on boot, never on a poll.
class OcleanSyncTimeButton : public button::Button, public Parented<OcleanHub> {
 public:
  void press_action() override { this->parent_->sync_clock(); }
};

// Read-only on the brush side: forces the poll cycle, sends no command of its own.
class OcleanPollNowButton : public button::Button, public Parented<OcleanHub> {
 public:
  void press_action() override { this->parent_->trigger_immediate_poll(); }
};

}  // namespace oclean
}  // namespace esphome

#endif  // USE_ESP32 && USE_BUTTON
