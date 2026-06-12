#pragma once

#include <cstddef>
#include <cstdint>

#include "oclean_protocol.h"

namespace esphome {
namespace oclean {

// === Runtime model profile ===
// The variant is picked at runtime from the DIS model string (char 0x2A24)
// rather than hardcoded, because the family spans several protocol variants and
// one firmware image serves all of them.
//
// Pure C++, no ESPHome or esp-idf dependency, so the table and the lookup link
// into the host test build.

// Which characteristic a command is written to.
enum WriteTarget : uint8_t {
  TX_MAIN = 0,     // the main write characteristic (most commands)
  TX_SESSION = 1,  // the session-download write characteristic
};

struct ProfileCmd {
  const uint8_t *bytes;
  uint8_t len;
  WriteTarget target;
  const char *name;
};

enum SettingsKind : uint8_t {
  SETTINGS_NONE = 0,   // no settings read for this profile
  SETTINGS_TYPE1_34B,  // two-frame 0302 transfer into a 34-byte buffer
};

// signature matches decode_session_record so the pure function can be pointed
// at directly
using RecordDecoder = bool (*)(const uint8_t *rec, SessionRecord *out);

// Plain data, no virtuals: the table lives in flash and allocates nothing.
struct OcleanProfile {
  const char *name;
  uint8_t confidence;  // 2 = hardware-validated, 1 = ported / unconfirmed

  const ProfileCmd *query_cmds;  // in send order
  uint8_t query_cmd_count;

  WriteTarget config_write_target;

  RecordDecoder decode_record;  // nullptr = no session records on this profile

  SettingsKind settings_kind;
};

extern const OcleanProfile PROFILE_TYPE1;
extern const OcleanProfile PROFILE_UNKNOWN;
extern const OcleanProfile PROFILE_TYPE_Z1;

// Longest-matching prefix, never null: anything unrecognised, empty or null
// lands on PROFILE_UNKNOWN. model need not be null-terminated.
const OcleanProfile *profile_for_model(const char *model, size_t len);

}  // namespace oclean
}  // namespace esphome
