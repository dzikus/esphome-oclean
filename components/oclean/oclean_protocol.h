#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace oclean {

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
static constexpr uint16_t DIS_HW_REV_UUID16 = 0x2A27;
static constexpr uint16_t DIS_SW_REV_UUID16 = 0x2A28;

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
// Per-region coverage, left 0-3 then right 4-7, each side ordered upper-outer /
// upper-inner / lower-outer / lower-inner.
static constexpr size_t SESSION_ZONES_OFFSET = 23;
static constexpr size_t SESSION_ZONES_COUNT = 8;
static constexpr size_t SESSION_SCORE_OFFSET = 33;
static_assert(SESSION_ZONES_OFFSET + SESSION_ZONES_COUNT <= SESSION_RECORD_SIZE,
              "zones must fit inside a record");
static_assert(SESSION_SCORE_OFFSET < SESSION_RECORD_SIZE,
              "score offset must lie inside a record");

struct SessionRecord {
  uint16_t year;  // full year (2000 + record byte 0)
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t scheme;             // pNum (brushing scheme id)
  uint16_t duration_s;        // total brushing time
  uint16_t valid_duration_s;  // time counted as effective
  uint8_t areas[5];           // offset 11-15 (area1-5 / pressureRatio)
  uint8_t zones[SESSION_ZONES_COUNT];  // gestureArray at SESSION_ZONES_OFFSET (left 0-3, right 4-7)
  uint8_t score;              // 0-100; SESSION_NO_SCORE means absent
  bool has_score;
};

// caller guarantees SESSION_RECORD_SIZE readable bytes at rec
bool decode_session_record(const uint8_t *rec, SessionRecord *out);

// With no unread sessions the device answers a download with a count=0 header
// plus the head of the newest already-read record. Timestamp, scheme and both
// durations fit in that fragment; zones and score do not (zeroed, has_score
// false).
bool decode_inline_0307(const uint8_t *data, size_t len, SessionRecord *out);

// the ring is not stored chronologically, so newest is found by timestamp
bool session_record_newer(const SessionRecord &a, const SessionRecord &b);

// Civil date-time read as if it were UTC. The reading is wrong but consistent,
// so a difference of two such values is still the true elapsed seconds, which is
// all the ordering and drift comparisons need.
int64_t civil_to_epoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour,
                       uint8_t minute, uint8_t second);

// ordering and dedup key, not a display time: brush-clock drift is left in
uint32_t session_record_epoch(const SessionRecord &r);

// How far past the local clock a session timestamp may sit before it is treated
// as implausible. One day absorbs brush-clock drift and timezone skew.
static constexpr uint32_t SESSION_FUTURE_MARGIN_S = 86400;

// A wildly future date from a hostile or glitched peer would push the dedup
// watermark past every real session and mute them for good. now_local_epoch
// shares the civil-as-UTC basis of session_record_epoch; <= 0 means unsynced,
// which cannot judge and passes. Past dates never advance the watermark.
bool session_epoch_plausible(uint32_t epoch, int64_t now_local_epoch,
                             uint32_t future_margin_s);

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
  if (drift > CLOCK_DRIFT_CLAMP_S) return CLOCK_DRIFT_CLAMP_S;
  if (drift < -CLOCK_DRIFT_CLAMP_S) return -CLOCK_DRIFT_CLAMP_S;
  return drift;
}

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

  uint8_t buf_[SESSION_MAX_RECORDS * SESSION_RECORD_SIZE];
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

// STATUS (0303) response. Empirically 8 bytes: 03 03 [b2] [b3] [b4] [battery]
// [b6] 00. battery (byte 5) is confirmed. charging_raw (byte 2) is the
// dock/charge state, confirmed empirically with three values: 0x01 charging on
// the dock, 0x02 off the dock, 0x03 on the dock fully charged (battery 100%).
// Returns false unless data starts 03 03, is at least 6 bytes long, and the
// battery byte is 0-100.
struct StatusResponse {
  uint8_t battery;       // byte 5, percent (0-100)
  uint8_t charging_raw;  // byte 2, dock/charge state (0x01/0x02/0x03)
};
bool parse_status_response(const uint8_t *data, size_t len, StatusResponse *out);

// True when the STATUS byte2 value means the brush is actively charging: only
// 0x01. This drives the Home Assistant charging binary sensor.
inline bool status_is_charging(uint8_t charging_raw) { return charging_raw == 0x01; }

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
  uint8_t device_theme;      // buffer 0
  bool brush_pause;          // buffer 1 != 0
  bool raise_wake;           // buffer 2 != 0
  bool fill_brush;           // buffer 3 != 0
  bool auto_mode;            // buffer 4 != 0
  bool volume_enabled;       // buffer 8 == 0 (inverted: 0 means enabled)
  uint8_t volume_index;      // buffer 9 (index into the volume table)
  bool calendar_enabled;     // buffer 10 == 0 (inverted: 0 means enabled)
  uint8_t scheme_pnum;       // buffer 11
  bool brush_mode_on;        // buffer 12 != 0xEC (0xEC is the off sentinel)
  bool splash_prevent;       // buffer 13 != 0
  uint16_t head_used_time;   // buffer 14-15 BE
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
  uint16_t head_max;         // buffer 25-26 BE (head replacement reminder days)
  uint16_t head_used_days;   // buffer 27-28 BE
  uint16_t head_used_times;  // buffer 29-30 BE
  uint8_t device_language;   // buffer 31
};

void parse_device_settings(const uint8_t *buf, DeviceSettings *out);

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
std::vector<uint8_t> build_toggle_command(uint8_t b0, uint8_t b1, uint8_t on_value,
                                          uint8_t off_value, bool state);

// === Timezone index decode ===
// 1-based index into the device's 33-entry GMT table; "unknown" out of range.
const char *timezone_index_to_string(uint8_t wire_index);

// exact match only, 0 when the offset has no table entry
uint8_t tz_index_for_offset_seconds(int32_t offset_seconds);

// === Device UI language (0216) ===
// one value byte, read back from settings buffer 31
std::vector<uint8_t> build_language_command(uint8_t lang_id);

// one packet, or two when the program needs the 020B split
std::vector<std::vector<uint8_t>> build_scheme_packets(uint8_t pnum,
                                                       const std::vector<SchemeStep> &steps);

// === Set-clock (0201) ===
//   02 01 [year-2000][month][day][hour][minute][second][weekday][tz_index]
// Plain decimal per byte, not BCD (minute 30 -> 0x1E), device local wall-clock
// rather than UTC, weekday 0=Sunday..6=Saturday.
std::vector<uint8_t> build_set_clock_command(uint16_t year, uint8_t month, uint8_t day,
                                             uint8_t hour, uint8_t minute, uint8_t second,
                                             uint8_t weekday, uint8_t tz_index);

// strictly greater than threshold_s, so a zero threshold still needs a 1 s gap
bool should_resync_clock(int64_t brush_epoch, int64_t local_epoch, uint32_t threshold_s);

bool poll_is_due(uint32_t since_ms, bool docked, uint32_t charging_interval_ms,
                 uint32_t battery_interval_ms);

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

// A count=0 reply carries the head of the newest already-read record, without
// zones or score. Publishing it blanks both, so it may only go out when strictly
// newer than what the entities already show.
bool accept_inline_record(uint32_t inline_epoch, uint32_t newest_epoch,
                          int64_t now_local_epoch);

// === Session ring ingest decision ===
// No I/O in here. Which records are new, where the dedup watermark lands, and
// whether the newest one is worth a flash write.
struct SessionIngestPlan {
  // new records, oldest first: the live state must settle on the most recent one
  std::vector<SessionRecord> to_publish;
  // newer than the watermark but dated implausibly far ahead, so dropped without
  // moving the watermark past a bogus date
  std::vector<SessionRecord> implausible;
  uint32_t new_watermark;
  uint32_t new_newest_epoch;
  bool persist_newest;
  bool newest_plausible;
  // The newest record of the whole ring, new or not: a re-served ring with
  // nothing new still republishes it to keep the live state right.
  bool have_newest;
  SessionRecord newest;
};

// The newest record is picked here, not passed in. An index from the reassembler
// would address a different record than the caller's vector as soon as one entry
// fails to decode. now_local_epoch <= 0 means an unsynced clock, which the
// plausibility check reads as "cannot judge".
SessionIngestPlan plan_session_ingest(const std::vector<SessionRecord> &records,
                                      uint32_t watermark, uint32_t newest_epoch,
                                      int64_t now_local_epoch);

// End of a query window. Holding the link costs no brush battery only while the
// brush sits on its charger, and a round with no STATUS reply cannot vouch for
// the dock state, so a brush that goes quiet cannot pin a BLE slot on a stale
// dock reading.
bool should_hold_link(bool hold_option, bool ble_enabled, bool docked,
                      bool round_status_seen);

}  // namespace oclean
}  // namespace esphome
