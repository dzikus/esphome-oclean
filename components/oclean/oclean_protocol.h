#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esphome::oclean {

// === GATT UUIDs (TYPE1 profile) ===
// Main custom service. Holds the brush command and notify characteristics.
extern const char *const OCLEAN_SERVICE_UUID;
// Tx for most commands (Write).
extern const char *const WRITE_CHAR_UUID;
// Rx for status / settings / device-info responses (Read/Notify).
extern const char *const READ_NOTIFY_CHAR_UUID;
// Tx for the session-download command (Write only, do not subscribe).
extern const char *const SEND_BRUSH_CMD_UUID;
// Rx for the session record stream (Notify only).
extern const char *const RECEIVE_BRUSH_UUID;

// Standard 16-bit assigned numbers used as full 128-bit base UUIDs by the stack.
static constexpr uint16_t BATTERY_SERVICE_UUID16 = 0x180F;
static constexpr uint16_t BATTERY_CHAR_UUID16 = 0x2A19;
static constexpr uint16_t DIS_SERVICE_UUID16 = 0x180A;
static constexpr uint16_t DIS_MODEL_UUID16 = 0x2A24;
static constexpr uint16_t DIS_FW_REV_UUID16 = 0x2A26;
static constexpr uint16_t DIS_HW_REV_UUID16 = 0x2A27;
static constexpr uint16_t DIS_SW_REV_UUID16 = 0x2A28;
static constexpr uint16_t BLUFI_SERVICE_UUID16 = 0xFFFF;
static constexpr uint16_t BLUFI_WRITE_CHAR_UUID16 = 0xFF01;
static constexpr uint16_t BLUFI_NOTIFY_CHAR_UUID16 = 0xFF02;

std::string dis_printable_text(const uint8_t *data, size_t len);

// 0x2A27 on newer models: "HH", protocol code (u16 BE), OTA type (u16 BE)
struct HwRevisionCode {
  uint16_t protocol;
  uint16_t ota_type;
};
bool decode_hw_revision_code(const uint8_t *data, size_t len, HwRevisionCode *out);

std::string hw_revision_text(const uint8_t *data, size_t len);

std::string gatt_props_text(uint8_t props);

// le: the uuid bytes least significant first, as the stack stores them
std::string ble_uuid_text(const uint8_t *le, size_t len);

// Commands are big-endian byte sequences with no CRC and no checksum. The device
// requires Write With Response; Write No Response is silently dropped. Per-model
// query sequences live in the profile table (oclean_profile.cpp).

// === Session stream constants ===
// Marker that prefixes the first session packet after the command echo.
static constexpr uint8_t SESSION_MAGIC[3] = {0x2A, 0x42, 0x23};
static constexpr size_t SESSION_MAGIC_LEN = 3;
// First packet layout: [03 07][2A 42 23][count_hi count_lo] then inline body.
static constexpr size_t SESSION_HEADER_LEN = 7;
static constexpr size_t SESSION_RECORD_SIZE = 42;
// the device ring holds 32; the headroom leaves room to reject a larger
// declared count as malformed rather than trusting it
static constexpr uint16_t SESSION_MAX_RECORDS = 64;
static constexpr uint8_t SESSION_NO_SCORE = 0xFF;
// The firmware scores a session it voids as 1: 14 s or less of brushing, or 85%
// or more of it without motion. Kept raw, published as no score.
static constexpr uint8_t SESSION_SCORE_VOID = 1;
// a zone the record does not carry; published as unknown
static constexpr uint8_t SESSION_ZONE_ABSENT = 0xFF;
// Per-region coverage, left 0-3 then right 4-7, each side ordered upper-outer /
// upper-inner / lower-outer / lower-inner.
static constexpr size_t SESSION_ZONES_OFFSET = 23;
static constexpr size_t SESSION_ZONES_COUNT = 8;
// the X Ultra 20 adds the upper (8, 9) and lower (10, 11) front teeth
static constexpr size_t SESSION_ZONES_MAX = 12;
static constexpr uint16_t SESSION_PRESSURE_ABSENT = 0xFFFF;
static constexpr size_t SESSION_SCORE_OFFSET = 33;
// time zone index of the brush clock when the session was recorded
static constexpr size_t SESSION_TZ_OFFSET = 17;
// Share per quadrant, summing to 100: upper left, lower left, upper right, lower
// right, i.e. the zone pairs 0+1, 2+3, 4+5, 6+7 rounded on the brush.
static constexpr size_t SESSION_QUADRANTS_OFFSET = 19;
static constexpr size_t SESSION_QUADRANTS_COUNT = 4;
static_assert(SESSION_ZONES_OFFSET + SESSION_ZONES_COUNT <= SESSION_RECORD_SIZE, "zones must fit inside a record");
static_assert(SESSION_SCORE_OFFSET < SESSION_RECORD_SIZE, "score offset must lie inside a record");
static_assert(SESSION_QUADRANTS_OFFSET + SESSION_QUADRANTS_COUNT <= SESSION_ZONES_OFFSET,
              "quadrants must sit before the zones");

struct SessionRecord {
  uint16_t year;  // full year (2000 + record byte 0)
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t scheme;             // pNum (brushing scheme id)
  uint16_t duration_s;        // program length
  uint16_t valid_duration_s;  // time actually brushed
  // 1-based into the 33-entry GMT table; 0 when the record does not carry it
  uint8_t tz_index;
  uint8_t quadrants[SESSION_QUADRANTS_COUNT];  // SESSION_ZONE_ABSENT when not carried
  // X Pro Elite: share per region in 0-7. X Ultra 20: seconds per zone in all
  // 12. SESSION_ZONE_ABSENT where the record has none.
  uint8_t zones[SESSION_ZONES_MAX];
  uint8_t score;  // 0-100; SESSION_NO_SCORE means absent
  bool has_score;
  // X Ultra 20 pressure log; SESSION_PRESSURE_ABSENT on the other records
  uint16_t over_pressure_s;  // brushing time spent over the brush's 400 limit
  uint16_t pressure_max;     // the brush's own force unit, 1000 = 1000 or more
};

// caller guarantees SESSION_RECORD_SIZE readable bytes at rec
bool decode_session_record(const uint8_t *rec, SessionRecord *out);

// 12 when the record carries the front-teeth zones, else 8
size_t session_zone_count(const SessionRecord &r);

// SessionRecord as persisted before the 12 zones and the pressure summary
struct SessionRecordV2 {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t scheme;
  uint16_t duration_s;
  uint16_t valid_duration_s;
  uint8_t tz_index;
  uint8_t quadrants[SESSION_QUADRANTS_COUNT];
  uint8_t zones[SESSION_ZONES_COUNT];
  uint8_t score;
  bool has_score;
};

SessionRecord session_record_from_v2(const SessionRecordV2 &old);

inline bool session_voided(const SessionRecord &r) {
  return !r.has_score && r.score == SESSION_SCORE_VOID;
}

// With no unread sessions the device answers a download with a count=0 header
// plus the first 13 bytes of ring slot 0. Sessions after a 0202 are written from
// slot 0 on, so that is the oldest session of the last batch handed over, not the
// newest. Timestamp, scheme and both durations fit in the fragment; time zone,
// quadrants, zones and score do not.
bool decode_inline_0307(const uint8_t *data, size_t len, SessionRecord *out);

// the ring is not stored chronologically, so newest is found by timestamp
bool session_record_newer(const SessionRecord &a, const SessionRecord &b);

// Civil date-time read as if it were UTC. The reading is wrong but consistent,
// so a difference of two such values is still the true elapsed seconds, which is
// all the ordering and drift comparisons need.
int64_t civil_to_epoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);

// ordering and dedup key, not a display time: brush-clock drift is left in
uint32_t session_record_epoch(const SessionRecord &r);

// How far past the local clock a session timestamp may sit before it is treated
// as implausible. The X Ultra 20 ships on a UTC+8 clock, up to 20 h ahead of a
// node in UTC-12; anything tighter drops every session of a brush nobody synced.
static constexpr uint32_t SESSION_FUTURE_MARGIN_S = 86400;
// the settings reply reports the clock that stamps the records, so only read
// latency and rounding separate the two
static constexpr uint32_t SESSION_BRUSH_CLOCK_MARGIN_S = 300;

// Civil-as-UTC like session_record_epoch; 0 = not available.
struct SessionClocks {
  int64_t node_local;
  int64_t brush;  // this round's settings reply, aged to now
};

// A wildly future date from a hostile or glitched peer would push the dedup
// watermark past every real session and mute them for good, hence the node
// bound. The brush bound rejects what the brush clock cannot have written yet:
// a record left from before the clock was set back. A brush clock more than the
// node margin behind is a lost clock, not a time zone, and judges nothing.
bool session_epoch_plausible(uint32_t epoch, const SessionClocks &clocks);

// The longest preset program runs 200 s and a 4-step custom one caps at 480 s,
// so two hours leaves room for any future scheme while keeping a spoofed 65535
// out of the entities and the long-term statistics.
static constexpr uint16_t SESSION_MAX_DURATION_S = 7200;

inline uint16_t clamp_session_duration(uint16_t seconds) {
  return seconds > SESSION_MAX_DURATION_S ? SESSION_MAX_DURATION_S : seconds;
}

// head_used_days/times/time are 16-bit wire values feeding TOTAL_INCREASING
// stats; one spoofed 65535 skews the sums for good. 7300 = 2/day over 10 years.
static constexpr uint16_t SETTINGS_HEAD_COUNTER_MAX = 7300;

inline uint16_t clamp_head_counter(uint16_t v) {
  return v > SETTINGS_HEAD_COUNTER_MAX ? SETTINGS_HEAD_COUNTER_MAX : v;
}

// drift sensor saturation, ~116 days. settings year byte reaches 2255 and
// passes the month..second range check, so raw drift can hit ~7.2e9 s; past
// the bound only the sign matters.
static constexpr int64_t CLOCK_DRIFT_CLAMP_S = 10000000;

inline int64_t clamp_clock_drift(int64_t drift) {
  if (drift > CLOCK_DRIFT_CLAMP_S)
    return CLOCK_DRIFT_CLAMP_S;
  if (drift < -CLOCK_DRIFT_CLAMP_S)
    return -CLOCK_DRIFT_CLAMP_S;
  return drift;
}

// === Variable-length session stream (X Ultra 20) ===
// First packet: [03 07][2A 42 23][count u16 BE][stream length u16 BE] then record
// bytes; continuation packets are raw record bytes. Each record opens with its
// own length (u16 BE). The firmware keeps at most 33 records of up to 182 bytes.
static constexpr size_t SESSION_V20_HEADER_LEN = 9;
static constexpr uint16_t SESSION_V20_MAX_RECORDS = 33;
static constexpr size_t SESSION_V20_RECORD_MAX = 182;
// The record length is min(182, brushed_s / 2 + 51), and the firmware keeps no
// session under 15 s: 58 bytes is the shortest one.
static constexpr size_t SESSION_V20_RECORD_MIN = 58;
static constexpr size_t SESSION_V20_LENGTH_BASE = 51;
static constexpr size_t SESSION_V20_MAX_BYTES = size_t(SESSION_V20_MAX_RECORDS) * SESSION_V20_RECORD_MAX;
static constexpr size_t SESSION_V20_TZ_OFFSET = 19;
// Seconds brushed per zone, low byte only: the back teeth 0-7 at [20-27] (left
// 0-3, right 4-7, upper pair first, as on the X Pro Elite), the front teeth 8-11
// at [32-35] (upper pair first). Only in a full record, never the count=0
// inline. Firmware 0.0.1.6 fills [20] and [32-35] alone and leaves 0xFF in
// [21-27].
static constexpr size_t SESSION_V20_ZONES_OFFSET = 20;
static constexpr size_t SESSION_V20_SCORE_OFFSET = 28;
static constexpr size_t SESSION_V20_FRONT_ZONES_OFFSET = 32;
static constexpr size_t SESSION_V20_FRONT_ZONES_COUNT = SESSION_ZONES_MAX - SESSION_ZONES_COUNT;
// Pressure log: one byte per 2 s of brushing, force / 4, 0xFF from 1000 up.
// Above 400 the brush halves the motor.
static constexpr size_t SESSION_V20_PRESSURE_OFFSET = 51;
static constexpr uint16_t SESSION_V20_PRESSURE_SAMPLE_S = 2;
static constexpr uint8_t SESSION_V20_PRESSURE_SATURATED = 0xFF;
static constexpr uint16_t SESSION_V20_PRESSURE_SATURATED_VALUE = 1000;
static constexpr uint16_t SESSION_V20_OVER_PRESSURE = 400;
static_assert(SESSION_V20_ZONES_OFFSET + SESSION_ZONES_COUNT <= SESSION_V20_SCORE_OFFSET &&
                  SESSION_V20_FRONT_ZONES_OFFSET + SESSION_V20_FRONT_ZONES_COUNT <= SESSION_V20_PRESSURE_OFFSET &&
                  SESSION_V20_PRESSURE_OFFSET < SESSION_V20_RECORD_MIN,
              "zones, score and the pressure log must start inside the shortest record");
// length, start time, mode, program length: what a count=0 reply carries
static constexpr size_t SESSION_V20_INLINE_LEN = 11;

// [0-1] length, [2-7] start time, [8] mode, [9-10] program length s, [11-12]
// brushed s, [19] time zone, [20-27] and [32-35] zones, [28] score, [51..]
// pressure log. The record has no quadrants; they come back
// SESSION_ZONE_ABSENT.
bool decode_session_record_v20(const uint8_t *rec, size_t len, SessionRecord *out);

// Brushed seconds read off the record length, low by at most 1 s. A record cut
// at the maximum length only bounds it, so that is 0 (unknown) unless the
// program is too short to run past the cut.
uint16_t v20_brushed_seconds(size_t record_len, uint16_t program_s);

// count=0 reply: 11 bytes of the store from offset 0 follow the header, the
// oldest record since the store was last cleared. valid_duration_s comes from
// the record length (0 when unknown); no zones or score.
bool decode_inline_0307_v20(const uint8_t *data, size_t len, SessionRecord *out);

class VarSessionAssembler {
 public:
  void reset();
  // Input after completion is ignored; a malformed header latches a failed
  // state that never completes. A count=0 header leaves it empty().
  bool feed(const uint8_t *data, size_t len);
  bool complete() const { return started_ && !failed_ && need_ > 0 && buf_.size() >= need_; }
  bool started() const { return started_; }
  bool failed() const { return failed_; }
  bool empty() const { return started_ && count_ == 0; }
  size_t record_count() const { return spans_.size(); }
  bool record(size_t i, SessionRecord *out) const;
  const uint8_t *raw_record(size_t i, size_t *len) const;
  // -1 when there is no record
  int newest_index() const;

 private:
  struct Span {
    size_t offset;
    size_t len;
  };
  void split_();

  std::vector<uint8_t> buf_{};
  std::vector<Span> spans_{};
  size_t need_ = 0;
  uint16_t count_ = 0;
  bool started_ = false;
  bool failed_ = false;
};

// First packet carries SESSION_HEADER_LEN header bytes then inline record
// bytes; continuation packets are raw record bytes, concatenated until
// count * SESSION_RECORD_SIZE is reached.
class SessionAssembler {
 public:
  void reset();
  // Input after completion is ignored; a malformed header latches a failed
  // state that never completes.
  bool feed(const uint8_t *data, size_t len);
  bool complete() const { return started_ && !failed_ && got_ >= need_ && need_ > 0; }
  bool started() const { return started_; }
  bool failed() const { return failed_; }
  uint16_t record_count() const { return count_; }
  // Decode record i (0-based). Returns false if out of range or not complete.
  bool record(uint16_t i, SessionRecord *out) const;
  // unparsed bytes of record i, for dumping offsets that are still being mapped
  const uint8_t *raw_record(uint16_t i) const;
  // -1 when the stream is empty or incomplete
  int newest_index() const;

 private:
  void append_(const uint8_t *data, size_t len);

  uint8_t buf_[SESSION_MAX_RECORDS * SESSION_RECORD_SIZE]{};
  // sized to the largest accepted stream, which is what makes the bound check in
  // append_() sufficient
  static_assert(sizeof(buf_) == size_t(SESSION_MAX_RECORDS) * SESSION_RECORD_SIZE,
                "session buffer size must match SESSION_MAX_RECORDS * SESSION_RECORD_SIZE");
  static_assert(SESSION_MAX_RECORDS <= SIZE_MAX / SESSION_RECORD_SIZE,
                "record count must not overflow the buffer math");
  size_t got_ = 0;
  size_t need_ = 0;
  uint16_t count_ = 0;
  bool started_ = false;
  bool failed_ = false;
};

// === Timeouts (milliseconds) ===
static constexpr uint32_t POST_CONNECT_SETTLE_MS = 800;
static constexpr uint32_t WHOLE_POLL_TIMEOUT_MS = 60000;
// Write With Response allows one outstanding write, so queued writes and the
// read queries that confirm them are spaced rather than pipelined.
static constexpr uint32_t PENDING_WRITE_STAGGER_MS = 300;
static constexpr uint32_t QUERY_STAGGER_MS = 500;
static constexpr uint32_t NOTIFY_REG_TIMEOUT_MS = 3000;
// A slider drags through many values; only the last program needs to be sent.
static constexpr uint32_t CUSTOM_SCHEME_DEBOUNCE_MS = 2000;
// Per-hub offset of the first poll after boot: the radio scans one target at a
// time, so simultaneous first cycles make the losing hub burn its whole window.
static constexpr uint32_t BOOT_STAGGER_MS = 90000;
static constexpr uint32_t CAPTURE_HOLD_MS = 30000;
// a normal poll's three queries all answer within a second or two
static constexpr uint32_t POLL_QUERY_HOLD_MS = 8000;
// model / hw / sw never change, so re-reading them only lengthens each link
static constexpr uint32_t DIS_CACHE_MS = 86400000UL;
// grace period after the stream for a brush-areas push (021f) to show up
static constexpr uint32_t ENRICHMENT_WAIT_MS = 2500;
// a clock set, then one settings query QUERY_STAGGER_MS later to read it back
static constexpr uint32_t CLOCK_READBACK_MS = 2000;
// Comfortably past the API batch delay: same-entity updates inside one batch
// window collapse to the last value, which would cost a backfilled ring all but
// its newest recorder row.
static constexpr uint32_t SESSION_PUBLISH_STAGGER_MS = 1500;

// === Decode helpers ===
inline uint16_t u16be(const uint8_t *buf) {
  return (uint16_t(buf[0]) << 8) | uint16_t(buf[1]);
}

// one byte, 0-100; a longer buffer is tolerated and only the first byte read
bool parse_battery_level(const uint8_t *data, size_t len, uint8_t *out);

// STATUS (0303) response: 03 03 [dock] [voltage u16 BE] [battery], then two more
// bytes on the X Pro Elite (8 in all) and none on the X Ultra 20 (6).
// charging_raw is the dock/charge state: 0x01 charging on the dock, 0x02 off the
// dock, 0x03 on the dock fully charged (battery 100%). Returns false unless data
// starts 03 03, is at least 6 bytes long, and the battery byte is 0-100.
struct StatusResponse {
  uint8_t battery;       // byte 5, percent (0-100)
  uint8_t charging_raw;  // byte 2, dock/charge state (0x01/0x02/0x03)
  uint16_t voltage_mv;   // bytes 3-4, cell voltage
};
bool parse_status_response(const uint8_t *data, size_t len, StatusResponse *out);

// outside this a reading is a glitch, not a li-ion cell
static constexpr uint16_t STATUS_VOLTAGE_MIN_MV = 2000;
static constexpr uint16_t STATUS_VOLTAGE_MAX_MV = 5000;

inline bool status_voltage_plausible(uint16_t mv) {
  return mv >= STATUS_VOLTAGE_MIN_MV && mv <= STATUS_VOLTAGE_MAX_MV;
}

// True when the STATUS byte2 value means the brush is actively charging: only
// 0x01. This drives the Home Assistant charging binary sensor.
inline bool status_is_charging(uint8_t charging_raw) {
  return charging_raw == 0x01;
}

// Dock presence, not charge phase, is what makes a fast cadence and a held link
// free of brush battery: a fully charged brush (0x03) still sits on the charger.
inline bool status_is_docked(uint8_t charging_raw) {
  return charging_raw == 0x01 || charging_raw == 0x03;
}

// Clock out of a single 0302 notify: [year-2000][month][day][hour][min][sec] at
// bytes 2-7. Superseded by the two-frame buffer below; kept because the range
// check on the calendar fields is what tells the two 0302 payloads apart.
struct SettingsClock {
  uint16_t year;  // full year (2000 + byte 2)
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};
bool parse_settings_clock(const uint8_t *data, size_t len, SettingsClock *out);

// === Full settings buffer (030201, two-frame transfer) ===
// Not a single notify: a '#'-framed transfer split across two 0302 notifies.
// The start frame carries 03 02 23 24 then 16 payload bytes into buffer
// [0..16); the continuation frame carries 03 02 then 18 bytes into [16..34).
// Per-field offsets are on the DeviceSettings members below.
static constexpr size_t SETTINGS_BUFFER_SIZE = 34;

// The 03 02 23 24 prefix marks the start frame; any other 0302 frame is taken
// as the continuation. Either order is accepted.
class SettingsAssembler {
 public:
  void reset();
  // true once both frames are in
  bool feed(const uint8_t *data, size_t len);
  bool complete() const { return got_start_ && got_cont_; }
  bool has_start() const { return got_start_; }
  bool has_cont() const { return got_cont_; }
  const uint8_t *buffer() const { return buf_; }

 private:
  uint8_t buf_[SETTINGS_BUFFER_SIZE]{};
  bool got_start_{false};
  bool got_cont_{false};
};

// Each field is only meaningful once its own frame has arrived, so callers gate
// on has_start() / has_cont() rather than complete().
struct DeviceSettings {
  // start frame, buffer 0..15
  uint8_t device_theme;     // buffer 0
  bool brush_pause;         // buffer 1 != 0
  bool raise_wake;          // buffer 2 != 0
  bool fill_brush;          // buffer 3 != 0
  bool auto_mode;           // buffer 4 != 0
  bool volume_enabled;      // buffer 8 == 0 (inverted: 0 means enabled)
  uint8_t volume_index;     // buffer 9 (index into the volume table)
  bool calendar_enabled;    // buffer 10 == 0 (inverted: 0 means enabled)
  uint8_t scheme_pnum;      // buffer 11
  bool brush_mode_on;       // buffer 12 != 0xEC (0xEC is the off sentinel)
  bool splash_prevent;      // buffer 13 != 0
  uint16_t head_used_time;  // buffer 14-15 BE, minutes brushed on this head
  // Continuation-frame fields (buffer 16..33), valid once it has the cont.
  uint16_t year;  // clock, buffer 16-21
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  bool clock_valid;
  bool over_pressure;        // buffer 22 != 0
  bool area_reminder;        // buffer 23 != 0
  uint8_t tz_index;          // buffer 24 (index into the GMT-offset table)
  uint16_t head_max;         // buffer 25-26 BE, head reminder limit in minutes brushed
  uint16_t head_used_days;   // buffer 27-28 BE
  uint16_t head_used_times;  // buffer 29-30 BE
  uint8_t device_language;   // buffer 31
};

void parse_device_settings(const uint8_t *buf, DeviceSettings *out);

// X Ultra 20 start frame, buffer 0..15. Its continuation frame (16..33) matches
// DeviceSettings.
struct DeviceSettingsV20Start {
  uint8_t battery;           // buffer 0, percent
  uint8_t network_status;    // buffer 1
  bool raise_wake;           // buffer 2 != 0
  bool auto_update;          // buffer 3 != 0
  bool auto_mode;            // buffer 4 != 0
  uint8_t mode_num;          // buffer 5, moves with buffer 11: not a count
  uint8_t bus_brushing;      // buffer 6
  bool voice;                // buffer 7 != 0
  bool voice_fast_brushing;  // buffer 8 != 0
  bool voice_pressure;       // buffer 9 != 0
  bool festival_reminder;    // buffer 10 != 0
  uint8_t mode;              // buffer 11
  bool brush_mode_on;        // buffer 12 != 0xEC
  uint8_t scheme_type;       // buffer 13
  uint16_t head_used_time;   // buffer 14-15 BE
};

void parse_device_settings_v20_start(const uint8_t *buf, DeviceSettingsV20Start *out);

// Unprompted push the device may send on the session characteristic after the
// 0307 stream completes; prefix 02 1f (Y3P) or 26 04 (other TYPE1). HYPOTHESIS,
// unconfirmed on hardware: the 8 per-area values sit at raw notify bytes 8..15.
// Only ever seen with fresh unread sessions, so a brush at rest may never emit
// it.
static constexpr size_t BRUSH_AREAS_VALUE_OFFSET = 8;  // within the raw notify
static constexpr size_t BRUSH_AREAS_COUNT = 8;
struct BrushAreasPush {
  uint8_t values[BRUSH_AREAS_COUNT];  // left 0-3, right 4-7
};
bool decode_brush_areas_push(const uint8_t *data, size_t len, BrushAreasPush *out);

// === Brush-scheme select (0206 / 020B) ===
// A scheme is a whole per-step program, so selecting one rewrites every step:
//   02 06 [pNum][stepCount] (enc_gear, gear, duration)*N 00 05
// Past 20 bytes it splits: first write is the leading 16 logical bytes plus a
// 2A 2B marker, second starts 02 0B and carries the remainder.
struct SchemeStep {
  uint8_t gear;
  uint8_t duration;  // seconds
};

// gears 1-12 map to a fixed table, anything else encodes as 0
uint8_t encode_scheme_gear(uint8_t gear);

// === Config-toggle write encoding ===
// Two-byte opcode plus a value byte. Usually on 0x01 / off 0x00, but the off
// value is a per-toggle sentinel: brush-mode off is 0xEC.
std::vector<uint8_t> build_toggle_command(uint8_t b0, uint8_t b1, uint8_t on_value, uint8_t off_value, bool state);

// X Ultra 20 voice prompts: one frame carries all three flags (main, fast
// brushing, over-pressure) plus a zero pad byte, so a single toggle has to
// resend the other two.
static constexpr size_t VOICE_PROMPT_COUNT = 3;
std::vector<uint8_t> build_voice_prompts_command(const std::array<bool, VOICE_PROMPT_COUNT> &flags);

// The brush stores the fast-brushing and over-pressure flags only from a frame
// with the main flag on; with it off they are acked and dropped.
inline bool voice_prompt_write_takes(size_t index, bool main_on) {
  return index == 0 || main_on;
}

// Single-byte status answer to a read: opcode, then the status. False when the
// frame is another opcode or too short.
bool parse_status_reply(const uint8_t *data, size_t len, uint8_t b0, uint8_t b1, uint8_t *status);

// <opcode> 45 52 ("ER"): a write turned down while a session runs, e.g. 0206,
// 0212 and 020C on the X Pro Elite
bool is_write_refusal(const uint8_t *data, size_t len);

// <b0> <b1> 4F 4B ("OK") with no echo byte, the ack of e.g. 0202
bool is_bare_ack(const uint8_t *data, size_t len, uint8_t b0, uint8_t b1);

// === Timezone index decode ===
// 1-based index into the device's 33-entry GMT table; "unknown" out of range.
const char *timezone_index_to_string(uint8_t wire_index);

// exact match only, 0 when the offset has no table entry
uint8_t tz_index_for_offset_seconds(int32_t offset_seconds);

// false for an index outside the table
bool tz_index_offset_seconds(uint8_t wire_index, int32_t *offset_seconds);

// Seconds to subtract from a record epoch (its local fields read as UTC) for
// the true UTC instant: the record's own time zone when it carries one, else
// node_offset_s, the node's offset now.
int64_t session_utc_offset_seconds(const SessionRecord &r, int64_t node_offset_s);

// === Device UI language (0216) ===
// one value byte, read back from settings buffer 31
std::vector<uint8_t> build_language_command(uint8_t lang_id);

// Highest language id the brush firmware has, from the DIS model string; 0 when
// the model's cap is unknown. A higher id turns the display English.
uint8_t max_language_id(const char *model, size_t len);

// one packet, or two when the program needs the 020B split
std::vector<std::vector<uint8_t>> build_scheme_packets(uint8_t pnum, const std::vector<SchemeStep> &steps);

// === X Ultra 20 cloud host (0233) ===
// Server the brush uploads records to. Firmware cap 59 bytes, no read-back.
// Frame: 02 33 2A [total_len][pkt_len] <ascii url>, the lengths equal for one
// unfragmented packet; empty url writes 02 33 2A 00 00 (firmware falls back).
static constexpr size_t CLOUD_HOST_MAX_LEN = 59;
std::vector<uint8_t> build_set_cloud_host_command(std::string_view url);
// The Host header of a request the brush sent is the host it has stored: host
// alone for port 80, host:port otherwise. True when it names the http:// url.
bool cloud_host_matches(std::string_view host_header, std::string_view url);

// === Birthday greeting (0211) ===
// 02 11 [gender][age][month][day]. On every wake on that day the X Ultra 20
// shows a birthday screen with the date. A month or day of 0xFF never matches,
// which is the unset state. An age of 0xFF would make it skip the date bytes,
// so gender and age are clamped the way the app does. Gender: 0 unknown, 1 male,
// 2 female; the firmware stores both without using them.
static constexpr uint8_t BIRTHDAY_UNSET = 0xFF;
static constexpr uint8_t USER_GENDER_DEFAULT = 0;
static constexpr uint8_t USER_AGE_DEFAULT = 18;
std::vector<uint8_t> build_birthday_command(uint8_t gender, uint8_t age, uint8_t month, uint8_t day);

// === BluFi Wi-Fi provisioning (service 0xFFFF, write 0xFF01, notify 0xFF02) ===
// Standard Espressif BluFi, the unencrypted variant (the X Ultra 20 accepts it).
// Frame: [type_byte][frame_control][seq][data_len] <data>, type_byte packs the
// subtype in bits 7..2 and the frame type in bits 1..0. frame_control 0 means no
// encryption, no checksum, phone-to-device, no ack, no fragment.
static constexpr uint8_t BLUFI_TYPE_CTRL = 0;
static constexpr uint8_t BLUFI_TYPE_DATA = 1;
static constexpr uint8_t BLUFI_CTRL_SET_SEC_MODE = 1;
static constexpr uint8_t BLUFI_CTRL_SET_OPMODE = 2;
static constexpr uint8_t BLUFI_CTRL_CONNECT_AP = 3;
static constexpr uint8_t BLUFI_CTRL_GET_STATUS = 5;
static constexpr uint8_t BLUFI_DATA_SSID = 2;
static constexpr uint8_t BLUFI_DATA_PASSWORD = 3;
static constexpr uint8_t BLUFI_DATA_WIFI_STATUS = 15;
static constexpr uint8_t BLUFI_OPMODE_STA = 1;

std::vector<uint8_t> build_blufi_frame(uint8_t frame_type, uint8_t subtype, const uint8_t *data, size_t data_len,
                                       uint8_t seq);

// A Wi-Fi connection report (data subtype 15): opmode and the station state
// (0 = connected) from the frame's data field. False for any other frame.
bool parse_blufi_wifi_status(const uint8_t *data, size_t len, uint8_t *opmode, uint8_t *sta_state);

// === Values the hub keeps on the brush from its yaml ===
// The birthday frame (0211), the cloud host (0233) and the BluFi Wi-Fi
// credentials cannot be read back. The hub stores a fingerprint of what the
// brush confirmed and writes again only when it changes: another yaml value,
// another node address, another brush.
enum class SyncSlot : uint8_t { BIRTHDAY = 0, CLOUD_HOST = 1, WIFI = 2 };
static constexpr size_t SYNC_SLOTS = 3;
// FNV-1a over the MAC, the slot and the payload; never 0, which stands for
// nothing confirmed
uint32_t sync_fingerprint(uint64_t mac, SyncSlot slot, const uint8_t *payload, size_t len);
// A value the brush has not confirmed goes out once per boot (and when it
// changes), then again every SYNC_RETRY_MS while still unconfirmed if retry is
// on. attempted_fp is the value last sent, 0 for none since boot.
static constexpr uint32_t SYNC_RETRY_MS = 24U * 3600U * 1000U;
bool sync_attempt_due(uint32_t fp, uint32_t confirmed_fp, uint32_t attempted_fp, uint32_t ms_since_attempt, bool retry);

// === Cloud session receiver (UploadBrushRecord body) ===
// The brush posts a flat JSON object whose string values carry no escapes or
// quotes (mac, model, brushdata hex). A full parser is not pulled in for two
// fields; these read exactly those shapes and reject anything else.

// Value of a "key":"value" pair in flat JSON. False when the key is absent or
// its value is not a plain double-quoted string. out is left untouched on false.
bool cloud_body_field(const char *body, size_t len, const char *key, std::string *out);

// Even-length hex text to bytes. False on odd length or any non-hex character.
bool parse_hex_bytes(std::string_view hex, std::vector<uint8_t> *out);

// "aa:bb:cc:dd:ee:ff" (any case, one or two digits per octet, a trailing colon
// tolerated) to the uint64 an ESPHome BLE address holds, most significant byte
// first. False unless exactly six octets.
bool parse_mac_u64(std::string_view mac, uint64_t *out);

// === X Ultra 20 weather (WeatherKit reply) ===
// The brush posts an empty body to .../WeatherKit each time it joins Wi-Fi and
// draws the answer on its clock page: an icon, a Today/Tomorrow banner and
// "low ~ high". Its codes, by the icon each one draws:
enum class BrushWeather : uint8_t {
  SNOW = 0,
  DUST = 1,  // orange wind lines with dots
  SUNNY = 2,
  WINDY = 3,
  CLOUDY = 4,  // sun behind a cloud
  RAIN = 5,
  STORM = 6,  // cloud, lightning and rain
  NONE = 0xFF,
};

// Home Assistant weather condition to the closest icon; NONE for anything else,
// "unavailable" included.
BrushWeather brush_weather_code(std::string_view condition);

// "2026-10-08T12:00:00+02:00" (Z, fractional seconds, or no offset = UTC) to a
// Unix epoch. False unless it starts with a full date and time.
bool parse_iso8601_epoch(std::string_view text, int64_t *out);

// one forecast day, keyed by the local calendar day (local civil epoch / 86400)
struct WeatherDay {
  int32_t local_day{0};
  BrushWeather code{BrushWeather::NONE};
  float high{0};
  float low{0};
};

static constexpr size_t WEATHER_DAYS_MAX = 3;
struct WeatherSnapshot {
  BrushWeather current_code{BrushWeather::NONE};
  float current_temp{NAN};
  std::array<WeatherDay, WEATHER_DAYS_MAX> days{};
  uint8_t day_count{0};
};

// from this local hour the reply carries tomorrow's forecast
static constexpr uint8_t WEATHER_TOMORROW_FROM_HOUR = 18;

struct WeatherPick {
  bool valid{false};
  bool tomorrow{false};
  BrushWeather code{BrushWeather::NONE};
  int high{0};
  int low{0};
};

// Today's forecast, tomorrow's from WEATHER_TOMORROW_FROM_HOUR, either one when
// the other is missing. Without a forecast day, the current condition with the
// current temperature as both bounds. Invalid when there is nothing to draw.
WeatherPick pick_weather(const WeatherSnapshot &s, int32_t local_day, uint8_t local_hour);

// Every value goes out as a JSON string: the firmware reads cJSON's valuestring,
// so a number would make it dereference null. Temperatures clamp to the two
// digits the page draws; hhmm lands in an 8-byte buffer and is left out unless
// it fits. An invalid pick answers {"state":false}: clock, dashes and a "?".
std::string build_weather_reply(const WeatherPick &pick, std::string_view hhmm);

// === Set-clock (0201) ===
//   02 01 [year-2000][month][day][hour][minute][second][weekday][tz_index]
// Plain decimal per byte, not BCD (minute 30 -> 0x1E), device local wall-clock
// rather than UTC, weekday 0=Sunday..6=Saturday.
static constexpr size_t SET_CLOCK_CMD_LEN = 10;
std::vector<uint8_t> build_set_clock_command(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                             uint8_t second, uint8_t weekday, uint8_t tz_index);

// civil-as-UTC time a set-clock command writes; 0 when it is not one
int64_t set_clock_command_epoch(const uint8_t *cmd, size_t len);

// strictly greater than threshold_s, so a zero threshold still needs a 1 s gap
bool should_resync_clock(int64_t brush_epoch, int64_t local_epoch, uint32_t threshold_s);

// Seconds the brush clock moved back when set: its reading aged to the write,
// minus the time written. Negative when it moved forward.
int64_t clock_set_shift(int64_t brush_read, uint32_t ms_since_read, int64_t written);

// Only a backward move within the node margin is carried into stored epochs. A
// lost clock reads years behind, and lifting the watermark by that would mute
// every session until then; a brush further ahead than the margin never got a
// record past the plausibility check, so the watermark is not in its time base.
bool clock_shift_applies(int64_t shift_s);

// a later reading within half the shift of the node clock shows the write took
bool clock_set_confirmed(int64_t shift_s, int64_t drift_s);

uint32_t shift_epoch_back(uint32_t epoch, int64_t shift_s);

bool poll_is_due(uint32_t since_ms, bool docked, uint32_t charging_interval_ms, uint32_t battery_interval_ms);

// === Poll tick decision ===
// last_poll_ms only counts once poll_pending is false: millis() legitimately
// reads 0 after the ~49.7 day wrap, so it cannot double as a never-polled
// sentinel.
struct PollTickState {
  uint32_t now_ms;
  uint32_t last_poll_ms;
  uint32_t boot_stagger_ms;  // hub index times the per-hub offset
  uint32_t charging_interval_ms;
  uint32_t battery_interval_ms;
  bool ble_enabled;
  bool link_busy;  // connected, or a connect already in flight
  bool poll_pending;
  bool adaptive;
  bool docked;
  bool boot_stagger_done;
};

enum class PollAction : uint8_t {
  SKIP_BLE_OFF,
  SKIP_LINK_BUSY,
  DEFER_BOOT_STAGGER,  // defer_ms carries how long
  SKIP_NOT_DUE,
  POLL,
};

struct PollDecision {
  PollAction action;
  uint32_t defer_ms;
};

PollDecision plan_poll_tick(const PollTickState &s);

// Share of the session counted as effective brushing. NAN on a record claiming
// no duration, clamped when a peer reports valid > duration. Rounded here, not
// on display: the raw float32 quotient reaches the recorder and the card
// verbatim, and 100*83/120 renders as 69.1666641235352.
float session_coverage_percent(uint16_t valid_duration_s, uint16_t duration_s);

// The fields an inline head and its full record share. Valid duration is left
// out: the X Ultra 20 head only derives it from the record length.
bool same_session(const SessionRecord &a, const SessionRecord &b);

// A count=0 reply carries the head of a read record, without zones or score.
// Publishing it blanks both, so it goes out only when strictly newer than what
// the entities show (the head is not always the newest record) and not the
// session they show: after the clock is set back, that head keeps its old date
// and would pass the epoch test. shown is nullptr when nothing is shown.
bool accept_inline_record(const SessionRecord &inl, uint32_t newest_epoch, const SessionRecord *shown,
                          const SessionClocks &clocks);

// === Session ring ingest decision ===
// No I/O in here. Which records are new, where the dedup watermark lands, and
// whether the newest one is worth a flash write.
struct SessionIngestPlan {
  // new records, oldest first: the live state must settle on the most recent one
  std::vector<SessionRecord> to_publish;
  // newer than the watermark but dated implausibly far ahead, so dropped without
  // moving the watermark past a bogus date
  std::vector<SessionRecord> implausible;
  uint32_t new_watermark{};
  uint32_t new_newest_epoch{};
  bool persist_newest{};
  bool newest_plausible{};
  // The newest record of the whole ring, new or not: a re-served ring with
  // nothing new still republishes it to keep the live state right.
  bool have_newest{};
  SessionRecord newest{};
};

// The newest record is picked here, not passed in. An index from the reassembler
// would address a different record than the caller's vector as soon as one entry
// fails to decode.
SessionIngestPlan plan_session_ingest(const std::vector<SessionRecord> &records, uint32_t watermark,
                                      uint32_t newest_epoch, const SessionClocks &clocks);

// 0202 confirms the batch: the brush zeroes its unread count and never serves
// those records again. A record held back as implausible would be lost with it.
bool should_confirm_sessions(bool profile_confirms, const SessionIngestPlan &plan);

// X Ultra 20 store clear after an inline record. Only on the dock, as this
// round's STATUS reads it: a brush in use ignores it. Once the brush acked the
// clear for a record, the same head keeps showing until the next session
// overwrites it, so it is not cleared again.
bool should_clear_inline(bool profile_clears, bool round_status_seen, bool docked, const SessionIngestPlan &plan,
                         uint32_t inline_epoch, uint32_t cleared_epoch);

// The stored record keeps the time base it was written in, while the watermark
// follows every clock shift, so after a set the watermark is the lower one.
uint32_t restored_newest_epoch(uint32_t record_epoch, uint32_t watermark);

// End of a query window. Holding the link costs no brush battery only while the
// brush sits on its charger, and a round with no STATUS reply cannot vouch for
// the dock state, so a brush that goes quiet cannot pin a BLE slot on a stale
// dock reading.
bool should_hold_link(bool hold_option, bool ble_enabled, bool docked, bool round_status_seen);

}  // namespace esphome::oclean
