#pragma once

#include <cstddef>
#include <cstdint>

#include "oclean_protocol.h"

namespace esphome::oclean {

// === Runtime model profile ===
// The variant is picked at runtime from the DIS model string (char 0x2A24)
// rather than hardcoded, because the family spans several protocol variants and
// one firmware image serves all of them.
//
// Pure C++, no ESPHome or esp-idf dependency, so the table and the lookup link
// into the host test build.

// Which characteristic a command is written to.
enum class WriteTarget : uint8_t {
  TX_MAIN = 0,     // the main write characteristic (most commands)
  TX_SESSION = 1,  // the session-download write characteristic
};

struct ProfileCmd {
  const uint8_t *bytes;
  uint8_t len;
  WriteTarget target;
  const char *name;
};

enum class SettingsKind : uint8_t {
  SETTINGS_NONE = 0,   // no settings read for this profile
  SETTINGS_TYPE1_34B,  // two-frame 0302 transfer into a 34-byte buffer
  SETTINGS_V20_34B,    // same transfer, X Ultra 20 meaning of buffer 0..15
};

enum class SessionFormat : uint8_t {
  NONE = 0,
  FIXED_42,  // count * 42-byte records
  VARIABLE,  // length-prefixed records, X Ultra 20
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

  RecordDecoder decode_record;  // FIXED_42 only
  SessionFormat session_format;

  SettingsKind settings_kind;

  bool allows_writes;
  // 0201 passes even when allows_writes is false
  bool allows_clock_write;
  bool skip_cccd_write;
  // 0202 after the queries
  bool sends_clear_running_data;
  // the mode byte indexes the scheme select's preset table
  bool cloud_scheme_ids;
};

extern const OcleanProfile PROFILE_TYPE1;
extern const OcleanProfile PROFILE_UNKNOWN;
extern const OcleanProfile PROFILE_TYPE_Z1;
extern const OcleanProfile PROFILE_TYPE_V20;

// Longest-matching prefix, never null: anything unrecognised, empty or null
// lands on PROFILE_UNKNOWN. model need not be null-terminated.
const OcleanProfile *profile_for_model(const char *model, size_t len);

bool clock_write_permitted(bool read_only, const OcleanProfile &profile);

bool command_permitted(bool read_only, const OcleanProfile &profile, const uint8_t *bytes, size_t len);

// the query answered by the settings transfer; nullptr when the profile reads none
const ProfileCmd *settings_query(const OcleanProfile &profile);

}  // namespace esphome::oclean
