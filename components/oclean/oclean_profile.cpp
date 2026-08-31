#include "oclean_profile.h"

#include <cstring>

namespace esphome::oclean {

// === TYPE1 query sequence ===
static const uint8_t TYPE1_STATUS_BYTES[] = {0x03, 0x03};
static const uint8_t TYPE1_SETTINGS_BYTES[] = {0x03, 0x02, 0x01};
static const uint8_t TYPE1_DOWNLOAD_BYTES[] = {0x03, 0x07};

static const ProfileCmd TYPE1_QUERY_CMDS[] = {
    {.bytes = TYPE1_STATUS_BYTES, .len = sizeof(TYPE1_STATUS_BYTES), .target = WriteTarget::TX_MAIN, .name = "STATUS"},
    {.bytes = TYPE1_SETTINGS_BYTES,
     .len = sizeof(TYPE1_SETTINGS_BYTES),
     .target = WriteTarget::TX_MAIN,
     .name = "SETTINGS"},
    {.bytes = TYPE1_DOWNLOAD_BYTES,
     .len = sizeof(TYPE1_DOWNLOAD_BYTES),
     .target = WriteTarget::TX_SESSION,
     .name = "SESSION_DOWNLOAD"},
};

// === UNKNOWN query sequence ===
// Status is the only model-agnostic query: settings framing and record layout
// are model-specific and would mis-decode on an unrecognised device. Battery
// and device info come from standard characteristics outside this sequence.
static const uint8_t UNKNOWN_STATUS_BYTES[] = {0x03, 0x03};

static const ProfileCmd UNKNOWN_QUERY_CMDS[] = {
    {.bytes = UNKNOWN_STATUS_BYTES,
     .len = sizeof(UNKNOWN_STATUS_BYTES),
     .target = WriteTarget::TX_MAIN,
     .name = "STATUS"},
};

const OcleanProfile PROFILE_TYPE1 = {
    /*name=*/.name = "TYPE1",
    /*confidence=*/.confidence = 2,
    /*query_cmds=*/.query_cmds = TYPE1_QUERY_CMDS,
    /*query_cmd_count=*/.query_cmd_count = sizeof(TYPE1_QUERY_CMDS) / sizeof(TYPE1_QUERY_CMDS[0]),
    /*config_write_target=*/.config_write_target = WriteTarget::TX_MAIN,
    /*decode_record=*/.decode_record = &decode_session_record,
    /*settings_kind=*/.settings_kind = SettingsKind::SETTINGS_TYPE1_34B,
};

const OcleanProfile PROFILE_UNKNOWN = {
    /*name=*/.name = "UNKNOWN",
    /*confidence=*/.confidence = 1,
    /*query_cmds=*/.query_cmds = UNKNOWN_QUERY_CMDS,
    /*query_cmd_count=*/.query_cmd_count = sizeof(UNKNOWN_QUERY_CMDS) / sizeof(UNKNOWN_QUERY_CMDS[0]),
    /*config_write_target=*/.config_write_target = WriteTarget::TX_MAIN,
    /*decode_record=*/.decode_record = nullptr,
    /*settings_kind=*/.settings_kind = SettingsKind::SETTINGS_NONE,
};

// === Z1 profile (model OCLEANY5) ===
// Identical to TYPE1 in every runtime respect, hence the shared query sequence
// and decoder. It stays a separate profile because the record layout and the
// settings framing are inherited, not confirmed against an OCLEANY5 capture.
const OcleanProfile PROFILE_TYPE_Z1 = {
    /*name=*/.name = "TYPE_Z1",
    /*confidence=*/.confidence = 1,
    /*query_cmds=*/.query_cmds = TYPE1_QUERY_CMDS,
    /*query_cmd_count=*/.query_cmd_count = sizeof(TYPE1_QUERY_CMDS) / sizeof(TYPE1_QUERY_CMDS[0]),
    /*config_write_target=*/.config_write_target = WriteTarget::TX_MAIN,
    /*decode_record=*/.decode_record = &decode_session_record,
    /*settings_kind=*/.settings_kind = SettingsKind::SETTINGS_TYPE1_34B,
};

// Order matters: first match wins, so the most specific prefix comes first
// (OCLEANY3P before OCLEANY3, OCLEANA1e/A1f before OCLEANA1).
namespace {
struct ProfileEntry {
  const char *prefix;
  const OcleanProfile *profile;
};
}  // namespace

static const ProfileEntry PROFILE_TABLE[] = {
    {.prefix = "OCLEANY3P", .profile = &PROFILE_TYPE1},  // X Pro Elite (validated); also matches Y3PD
    {.prefix = "OCLEANY3M", .profile = &PROFILE_TYPE1},  // ported
    {.prefix = "OCLEANY3D", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANY3N", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANY3S", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANY3T", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANY3", .profile = &PROFILE_TYPE1},  // generic X / X Pro, shorter prefix last
    {.prefix = "OCLEANR3L", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANX20", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANV1", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANA1e", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANA1f", .profile = &PROFILE_TYPE1},
    {.prefix = "OCLEANA1", .profile = &PROFILE_UNKNOWN},  // legacy, after the A1e / A1f specifics
    {.prefix = "OCLEANY5", .profile = &PROFILE_TYPE_Z1},  // Z1
    {.prefix = "OCLEANC1", .profile = &PROFILE_UNKNOWN},  // WiFi, out of BLE scope
};
static const size_t PROFILE_TABLE_SIZE = sizeof(PROFILE_TABLE) / sizeof(PROFILE_TABLE[0]);

const OcleanProfile *profile_for_model(const char *model, size_t len) {
  if (model == nullptr || len == 0)
    return &PROFILE_UNKNOWN;
  for (auto i : PROFILE_TABLE) {
    size_t const plen = strlen(i.prefix);
    if (len < plen)
      continue;
    if (strncmp(model, i.prefix, plen) == 0)
      return i.profile;
  }
  return &PROFILE_UNKNOWN;
}

}  // namespace esphome::oclean
