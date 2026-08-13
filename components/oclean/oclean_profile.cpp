#include "oclean_profile.h"

#include <cstring>

namespace esphome {
namespace oclean {

// === TYPE1 query sequence ===
static const uint8_t TYPE1_STATUS_BYTES[] = {0x03, 0x03};
static const uint8_t TYPE1_SETTINGS_BYTES[] = {0x03, 0x02, 0x01};
static const uint8_t TYPE1_DOWNLOAD_BYTES[] = {0x03, 0x07};

static const ProfileCmd TYPE1_QUERY_CMDS[] = {
    {TYPE1_STATUS_BYTES, sizeof(TYPE1_STATUS_BYTES), WriteTarget::TX_MAIN, "STATUS"},
    {TYPE1_SETTINGS_BYTES, sizeof(TYPE1_SETTINGS_BYTES), WriteTarget::TX_MAIN, "SETTINGS"},
    {TYPE1_DOWNLOAD_BYTES, sizeof(TYPE1_DOWNLOAD_BYTES), WriteTarget::TX_SESSION, "SESSION_DOWNLOAD"},
};

// === UNKNOWN query sequence ===
// Status is the only model-agnostic query: settings framing and record layout
// are model-specific and would mis-decode on an unrecognised device. Battery
// and device info come from standard characteristics outside this sequence.
static const uint8_t UNKNOWN_STATUS_BYTES[] = {0x03, 0x03};

static const ProfileCmd UNKNOWN_QUERY_CMDS[] = {
    {UNKNOWN_STATUS_BYTES, sizeof(UNKNOWN_STATUS_BYTES), WriteTarget::TX_MAIN, "STATUS"},
};

const OcleanProfile PROFILE_TYPE1 = {
    /*name=*/"TYPE1",
    /*confidence=*/2,
    /*query_cmds=*/TYPE1_QUERY_CMDS,
    /*query_cmd_count=*/sizeof(TYPE1_QUERY_CMDS) / sizeof(TYPE1_QUERY_CMDS[0]),
    /*config_write_target=*/WriteTarget::TX_MAIN,
    /*decode_record=*/&decode_session_record,
    /*settings_kind=*/SettingsKind::SETTINGS_TYPE1_34B,
};

const OcleanProfile PROFILE_UNKNOWN = {
    /*name=*/"UNKNOWN",
    /*confidence=*/1,
    /*query_cmds=*/UNKNOWN_QUERY_CMDS,
    /*query_cmd_count=*/sizeof(UNKNOWN_QUERY_CMDS) / sizeof(UNKNOWN_QUERY_CMDS[0]),
    /*config_write_target=*/WriteTarget::TX_MAIN,
    /*decode_record=*/nullptr,
    /*settings_kind=*/SettingsKind::SETTINGS_NONE,
};

// === Z1 profile (model OCLEANY5) ===
// Identical to TYPE1 in every runtime respect, hence the shared query sequence
// and decoder. It stays a separate profile because the record layout and the
// settings framing are inherited, not confirmed against an OCLEANY5 capture.
const OcleanProfile PROFILE_TYPE_Z1 = {
    /*name=*/"TYPE_Z1",
    /*confidence=*/1,
    /*query_cmds=*/TYPE1_QUERY_CMDS,
    /*query_cmd_count=*/sizeof(TYPE1_QUERY_CMDS) / sizeof(TYPE1_QUERY_CMDS[0]),
    /*config_write_target=*/WriteTarget::TX_MAIN,
    /*decode_record=*/&decode_session_record,
    /*settings_kind=*/SettingsKind::SETTINGS_TYPE1_34B,
};

// Order matters: first match wins, so the most specific prefix comes first
// (OCLEANY3P before OCLEANY3, OCLEANA1e/A1f before OCLEANA1).
struct ProfileEntry {
  const char *prefix;
  const OcleanProfile *profile;
};

static const ProfileEntry PROFILE_TABLE[] = {
    {"OCLEANY3P", &PROFILE_TYPE1},   // X Pro Elite (validated); also matches Y3PD
    {"OCLEANY3M", &PROFILE_TYPE1},   // ported
    {"OCLEANY3D", &PROFILE_TYPE1},
    {"OCLEANY3N", &PROFILE_TYPE1},
    {"OCLEANY3S", &PROFILE_TYPE1},
    {"OCLEANY3T", &PROFILE_TYPE1},
    {"OCLEANY3", &PROFILE_TYPE1},    // generic X / X Pro, shorter prefix last
    {"OCLEANR3L", &PROFILE_TYPE1},
    {"OCLEANX20", &PROFILE_TYPE1},
    {"OCLEANV1", &PROFILE_TYPE1},
    {"OCLEANA1e", &PROFILE_TYPE1},
    {"OCLEANA1f", &PROFILE_TYPE1},
    {"OCLEANA1", &PROFILE_UNKNOWN},  // legacy, after the A1e / A1f specifics
    {"OCLEANY5", &PROFILE_TYPE_Z1},  // Z1
    {"OCLEANC1", &PROFILE_UNKNOWN},  // WiFi, out of BLE scope
};
static const size_t PROFILE_TABLE_SIZE =
    sizeof(PROFILE_TABLE) / sizeof(PROFILE_TABLE[0]);

const OcleanProfile *profile_for_model(const char *model, size_t len) {
  if (model == nullptr || len == 0) return &PROFILE_UNKNOWN;
  for (size_t i = 0; i < PROFILE_TABLE_SIZE; i++) {
    size_t plen = strlen(PROFILE_TABLE[i].prefix);
    if (len < plen) continue;
    if (strncmp(model, PROFILE_TABLE[i].prefix, plen) == 0)
      return PROFILE_TABLE[i].profile;
  }
  return &PROFILE_UNKNOWN;
}

}  // namespace oclean
}  // namespace esphome
