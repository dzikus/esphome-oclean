#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

// esphome.h pulls in every component header, so guard the platform here too
#if defined(USE_ESP32) && defined(USE_TEXT)
#include <algorithm>
#include <cstring>
#include <string>

#include "esphome/components/text/text.h"
#include "esphome/core/preferences.h"
#include "oclean.h"

namespace esphome::oclean {

// Local, flash-backed string staged for a brush action (the cloud host). Writing
// it stores and shows the value; nothing reaches the brush until the matching
// button fires, since the firmware offers no read-back.
class OcleanStoredText : public text::Text, public Component, public Parented<OcleanHub> {
 public:
  void setup() override {
    // Salted per hub: auto-created entities on two hubs share an object-id hash
    // and would otherwise collide in one flash slot.
    this->pref_ =
        global_preferences->make_preference<StoredBlob>(this->get_object_id_hash() ^ this->parent_->pref_salt());
    StoredBlob blob{};
    if (this->pref_.load(&blob)) {
      blob.buf[STORED_TEXT_CAP - 1] = '\0';
      this->publish_state(std::string(blob.buf));
    } else {
      this->publish_state("");
    }
  }

  // persists and publishes, no brush write; shared by control() and clear
  void store_and_publish(const std::string &value) {
    StoredBlob blob{};
    size_t const n = std::min(value.size(), STORED_TEXT_CAP - 1);
    std::memcpy(blob.buf, value.data(), n);
    blob.buf[n] = '\0';
    this->pref_.save(&blob);
    this->publish_state(std::string(blob.buf, n));
  }

 protected:
  void control(const std::string &value) override { this->store_and_publish(value); }

  // room for the 59-byte cloud host; a new size drops the stored value
  static constexpr size_t STORED_TEXT_CAP = 64;
  struct StoredBlob {
    char buf[STORED_TEXT_CAP];
  };
  ESPPreferenceObject pref_;
};

}  // namespace esphome::oclean

#endif  // USE_ESP32 && USE_TEXT
