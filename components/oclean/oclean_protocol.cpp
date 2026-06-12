#include "oclean_protocol.h"

namespace esphome {
namespace oclean {

const char *const OCLEAN_SERVICE_UUID = "8082caa8-41a6-4021-91c6-56f9b954cc18";
const char *const WRITE_CHAR_UUID = "9d84b9a3-000c-49d8-9183-855b673fbb85";
const char *const READ_NOTIFY_CHAR_UUID = "5f78df94-798c-46f5-990a-855b673fbb86";
const char *const SEND_BRUSH_CMD_UUID = "5f78df94-798c-46f5-990a-855b673fbb89";
const char *const RECEIVE_BRUSH_UUID = "5f78df94-798c-46f5-990a-855b673fbb90";

bool parse_battery_level(const uint8_t *data, size_t len, uint8_t *out) {
  if (data == nullptr || out == nullptr) return false;
  if (len < 1) return false;
  uint8_t level = data[0];
  if (level > 100) return false;
  *out = level;
  return true;
}

bool parse_status_response(const uint8_t *data, size_t len, StatusResponse *out) {
  if (data == nullptr || out == nullptr) return false;
  if (len < 6) return false;
  if (data[0] != 0x03 || data[1] != 0x03) return false;
  uint8_t battery = data[5];
  if (battery > 100) return false;
  out->battery = battery;
  out->charging_raw = data[2];
  return true;
}

bool parse_settings_clock(const uint8_t *data, size_t len, SettingsClock *out) {
  if (data == nullptr || out == nullptr) return false;
  if (len < 8) return false;
  if (data[0] != 0x03 || data[1] != 0x02) return false;
  // The device sends more than one 0302-prefixed message; only the clock has
  // in-range calendar fields. Range-check to reject the other 0302 payload
  // instead of decoding it into a nonsense date.
  uint8_t month = data[3];
  uint8_t day = data[4];
  uint8_t hour = data[5];
  uint8_t minute = data[6];
  uint8_t second = data[7];
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (hour > 23 || minute > 59 || second > 59) return false;
  out->year = uint16_t(2000) + data[2];
  out->month = month;
  out->day = day;
  out->hour = hour;
  out->minute = minute;
  out->second = second;
  return true;
}

void SettingsAssembler::reset() {
  for (size_t i = 0; i < SETTINGS_BUFFER_SIZE; i++) buf_[i] = 0;
  got_start_ = false;
  got_cont_ = false;
}

bool SettingsAssembler::feed(const uint8_t *data, size_t len) {
  if (data == nullptr) return false;
  if (len < 20) return false;
  if (data[0] != 0x03 || data[1] != 0x02) return false;
  if (data[2] == 0x23 && data[3] == 0x24) {
    // Start frame: 16 payload bytes at data[4..20) map to buffer[0..16).
    for (size_t i = 0; i < 16; i++) buf_[i] = data[4 + i];
    got_start_ = true;
  } else {
    // Continuation frame: 18 payload bytes at data[2..20) map to buffer[16..34).
    for (size_t i = 0; i < 18; i++) buf_[16 + i] = data[2 + i];
    got_cont_ = true;
  }
  return complete();
}

void parse_device_settings(const uint8_t *buf, DeviceSettings *out) {
  if (buf == nullptr || out == nullptr) return;
  // Start-frame region (buffer 0..15).
  out->device_theme = buf[0];
  out->brush_pause = buf[1] != 0;
  out->raise_wake = buf[2] != 0;
  out->fill_brush = buf[3] != 0;
  out->auto_mode = buf[4] != 0;
  out->volume_enabled = buf[8] == 0;     // inverted: 0 means enabled
  out->volume_index = buf[9];
  out->calendar_enabled = buf[10] == 0;  // inverted: 0 means enabled
  out->scheme_pnum = buf[11];
  out->brush_mode_on = buf[12] != 0xEC;  // 0xEC is the off sentinel
  out->splash_prevent = buf[13] != 0;
  out->head_used_time = u16be(buf + 14);
  // Continuation-frame region (buffer 16..33).
  out->year = uint16_t(2000) + buf[16];
  out->month = buf[17];
  out->day = buf[18];
  out->hour = buf[19];
  out->minute = buf[20];
  out->second = buf[21];
  out->clock_valid = out->month >= 1 && out->month <= 12 && out->day >= 1 &&
                     out->day <= 31 && out->hour <= 23 && out->minute <= 59 &&
                     out->second <= 59;
  out->over_pressure = buf[22] != 0;
  out->area_reminder = buf[23] != 0;
  out->tz_index = buf[24];
  out->head_max = u16be(buf + 25);
  out->head_used_days = u16be(buf + 27);
  out->head_used_times = u16be(buf + 29);
  out->device_language = buf[31];
}

bool decode_brush_areas_push(const uint8_t *data, size_t len, BrushAreasPush *out) {
  if (data == nullptr || out == nullptr) return false;
  if (len < 2) return false;
  bool is_y3p = (data[0] == 0x02 && data[1] == 0x1F);
  bool is_t1 = (data[0] == 0x26 && data[1] == 0x04);
  if (!is_y3p && !is_t1) return false;
  if (len < BRUSH_AREAS_VALUE_OFFSET + BRUSH_AREAS_COUNT) return false;
  for (size_t i = 0; i < BRUSH_AREAS_COUNT; i++)
    out->values[i] = data[BRUSH_AREAS_VALUE_OFFSET + i];
  return true;
}

bool decode_session_record(const uint8_t *rec, SessionRecord *out) {
  if (rec == nullptr || out == nullptr) return false;
  out->year = uint16_t(2000) + rec[0];
  out->month = rec[1];
  out->day = rec[2];
  out->hour = rec[3];
  out->minute = rec[4];
  out->second = rec[5];
  out->scheme = rec[6];
  out->duration_s = u16be(rec + 7);
  out->valid_duration_s = u16be(rec + 9);
  for (size_t i = 0; i < 5; i++) out->areas[i] = rec[11 + i];
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    out->zones[i] = rec[SESSION_ZONES_OFFSET + i];
  uint8_t s = rec[SESSION_SCORE_OFFSET];
  out->has_score = (s != SESSION_NO_SCORE);
  out->score = s;
  return true;
}

bool decode_inline_0307(const uint8_t *data, size_t len, SessionRecord *out) {
  if (data == nullptr || out == nullptr) return false;
  // 0307 marker, *B# magic, record count 0, then the head of the newest
  // record. The fixed head (date-time, scheme, duration, valid duration) is
  // 11 bytes; up to two leading area bytes follow within one notify.
  static const uint8_t HEAD[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00};
  static const size_t HEAD_LEN = sizeof(HEAD);
  if (len < HEAD_LEN + 11) return false;
  for (size_t i = 0; i < HEAD_LEN; i++)
    if (data[i] != HEAD[i]) return false;
  const uint8_t *r = data + HEAD_LEN;
  // An all-zero date means the device holds no session at all.
  if (r[0] == 0 && r[1] == 0 && r[2] == 0) return false;
  out->year = uint16_t(2000) + r[0];
  out->month = r[1];
  out->day = r[2];
  out->hour = r[3];
  out->minute = r[4];
  out->second = r[5];
  out->scheme = r[6];
  out->duration_s = u16be(r + 7);
  out->valid_duration_s = u16be(r + 9);
  const size_t avail = len - HEAD_LEN;
  for (size_t i = 0; i < 5; i++)
    out->areas[i] = (11 + i < avail) ? r[11 + i] : 0;
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++) out->zones[i] = 0;
  out->score = 0;
  out->has_score = false;
  return true;
}

bool session_record_newer(const SessionRecord &a, const SessionRecord &b) {
  if (a.year != b.year) return a.year > b.year;
  if (a.month != b.month) return a.month > b.month;
  if (a.day != b.day) return a.day > b.day;
  if (a.hour != b.hour) return a.hour > b.hour;
  if (a.minute != b.minute) return a.minute > b.minute;
  return a.second > b.second;
}

int64_t civil_to_epoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour,
                       uint8_t minute, uint8_t second) {
  // Days from 1970-01-01 for a proleptic Gregorian date (days-from-civil). The
  // fields are treated as UTC: differences between two values are the true
  // elapsed seconds, which is what the drift comparison and session ordering
  // need.
  int32_t y = (int32_t) year;
  uint32_t m = month;
  uint32_t d = day;
  y -= (m <= 2) ? 1 : 0;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = (uint32_t)(y - era * 400);                       // [0, 399]
  uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  int32_t days = era * 146097 + (int32_t) doe - 719468;
  return (int64_t) days * 86400 + (int64_t) hour * 3600 + (int64_t) minute * 60 +
         (int64_t) second;
}

uint32_t session_record_epoch(const SessionRecord &r) {
  // Ordering / dedup key, not a display time. Brush-clock drift is not
  // corrected. Clamp to 0 so the unsigned key never wraps on a degenerate date.
  int64_t secs = civil_to_epoch(r.year, r.month, r.day, r.hour, r.minute, r.second);
  if (secs < 0) secs = 0;
  // A malformed record with a wild future date would otherwise persist a
  // watermark that suppresses every later session. Treat anything past the
  // uint32 range as unordered, same as the negative clamp.
  if (secs > (int64_t) UINT32_MAX) return 0;
  return (uint32_t) secs;
}

bool session_epoch_plausible(uint32_t epoch, int64_t now_local_epoch,
                             uint32_t future_margin_s) {
  if (now_local_epoch <= 0) return true;  // no trustworthy clock: cannot judge
  return (int64_t) epoch <= now_local_epoch + (int64_t) future_margin_s;
}

bool should_resync_clock(int64_t brush_epoch, int64_t local_epoch, uint32_t threshold_s) {
  int64_t drift = brush_epoch - local_epoch;
  if (drift < 0) drift = -drift;
  return drift > (int64_t) threshold_s;
}

bool poll_is_due(uint32_t since_ms, bool docked, uint32_t charging_interval_ms,
                 uint32_t battery_interval_ms) {
  return since_ms >= (docked ? charging_interval_ms : battery_interval_ms);
}

void SessionAssembler::reset() {
  got_ = 0;
  need_ = 0;
  count_ = 0;
  started_ = false;
  failed_ = false;
}

void SessionAssembler::append_(const uint8_t *data, size_t len) {
  size_t room = need_ - got_;
  size_t n = (len < room) ? len : room;
  for (size_t i = 0; i < n; i++) buf_[got_ + i] = data[i];
  got_ += n;
}

bool SessionAssembler::feed(const uint8_t *data, size_t len) {
  if (failed_) return false;
  if (data == nullptr) {
    failed_ = true;
    return false;
  }
  if (complete()) return true;  // ignore trailing input
  if (!started_) {
    if (len < SESSION_HEADER_LEN) {
      failed_ = true;
      return false;
    }
    if (data[0] != 0x03 || data[1] != 0x07) {
      failed_ = true;
      return false;
    }
    if (data[2] != SESSION_MAGIC[0] || data[3] != SESSION_MAGIC[1] ||
        data[4] != SESSION_MAGIC[2]) {
      failed_ = true;
      return false;
    }
    count_ = (uint16_t(data[5]) << 8) | data[6];
    if (count_ == 0 || count_ > SESSION_MAX_RECORDS) {
      failed_ = true;
      return false;
    }
    need_ = size_t(count_) * SESSION_RECORD_SIZE;
    started_ = true;
    this->append_(data + SESSION_HEADER_LEN, len - SESSION_HEADER_LEN);
    return complete();
  }
  this->append_(data, len);
  return complete();
}

bool SessionAssembler::record(uint16_t i, SessionRecord *out) const {
  if (!complete() || i >= count_) return false;
  return decode_session_record(buf_ + size_t(i) * SESSION_RECORD_SIZE, out);
}

const uint8_t *SessionAssembler::raw_record(uint16_t i) const {
  if (!complete() || i >= count_) return nullptr;
  return buf_ + size_t(i) * SESSION_RECORD_SIZE;
}

int SessionAssembler::newest_index() const {
  if (!complete() || count_ == 0) return -1;
  int best = 0;
  SessionRecord best_rec;
  decode_session_record(buf_, &best_rec);
  for (uint16_t i = 1; i < count_; i++) {
    SessionRecord cur;
    decode_session_record(buf_ + size_t(i) * SESSION_RECORD_SIZE, &cur);
    if (session_record_newer(cur, best_rec)) {
      best_rec = cur;
      best = i;
    }
  }
  return best;
}

std::vector<uint8_t> build_toggle_command(uint8_t b0, uint8_t b1, uint8_t on_value,
                                          uint8_t off_value, bool state) {
  return {b0, b1, state ? on_value : off_value};
}

const char *timezone_index_to_string(uint8_t wire_index) {
  static const char *const TABLE[33] = {
      "GMT-12:00", "GMT-11:00", "GMT-10:00", "GMT-09:00", "GMT-08:00",
      "GMT-07:00", "GMT-06:00", "GMT-05:00", "GMT-04:00", "GMT-03:30",
      "GMT-03:00", "GMT-02:00", "GMT-01:00", "GMT+00:00",
      "GMT+01:00", "GMT+02:00", "GMT+03:00", "GMT+03:30",
      "GMT+04:00", "GMT+04:30", "GMT+05:00", "GMT+05:30", "GMT+05:45",
      "GMT+06:00", "GMT+06:30", "GMT+07:00", "GMT+08:00", "GMT+09:00",
      "GMT+09:30", "GMT+10:00", "GMT+11:00", "GMT+12:00", "GMT+13:00",
  };
  if (wire_index < 1 || wire_index > 33) return "unknown";
  return TABLE[wire_index - 1];
}

uint8_t tz_index_for_offset_seconds(int32_t offset_seconds) {
  // Seconds per entry, same order as the string table above.
  static const int32_t OFFSETS[33] = {
      -43200, -39600, -36000, -32400, -28800,  // -12:00 .. -08:00
      -25200, -21600, -18000, -14400, -12600,  // -07:00 .. -03:30
      -10800, -7200,  -3600,  0,                // -03:00 .. +00:00
      3600,   7200,   10800,  12600,            // +01:00 .. +03:30
      14400,  16200,  18000,  19800,  20700,    // +04:00 .. +05:45
      21600,  23400,  25200,  28800,  32400,    // +06:00 .. +09:00
      34200,  36000,  39600,  43200,  46800,    // +09:30 .. +13:00
  };
  for (uint8_t i = 0; i < 33; i++)
    if (OFFSETS[i] == offset_seconds) return (uint8_t) (i + 1);
  return 0;
}

std::vector<uint8_t> build_language_command(uint8_t lang_id) {
  return {0x02, 0x16, lang_id};
}

uint8_t encode_scheme_gear(uint8_t gear) {
  switch (gear) {
    case 1: return 5;
    case 2: return 6;
    case 3: return 7;
    case 4: return 8;
    case 5: return 17;
    case 6: return 18;
    case 7: return 19;
    case 8: return 20;
    case 9: return 21;
    case 10: return 22;
    case 11: return 23;
    case 12: return 24;
    default: return 0;
  }
}

std::vector<std::vector<uint8_t>> build_scheme_packets(uint8_t pnum,
                                                       const std::vector<SchemeStep> &steps) {
  // A program is 1 to 8 steps; reject anything else so stepCount stays a valid
  // byte and the caller never sends a degenerate program.
  if (steps.empty() || steps.size() > 8) return {};
  std::vector<uint8_t> payload;
  payload.push_back(0x02);
  payload.push_back(0x06);
  payload.push_back(pnum);
  payload.push_back((uint8_t) steps.size());
  for (const auto &s : steps) {
    payload.push_back(encode_scheme_gear(s.gear));
    payload.push_back(s.gear);
    payload.push_back(s.duration);
  }
  payload.push_back(0x00);
  payload.push_back(0x05);

  std::vector<std::vector<uint8_t>> packets;
  if (payload.size() > 20) {
    // Split: first 16 logical bytes plus the continuation marker, then a 020B
    // frame carrying the rest. One outstanding Write With Response at a time, so
    // the caller delivers these as two separate writes.
    std::vector<uint8_t> pkt1(payload.begin(), payload.begin() + 16);
    pkt1.push_back(0x2A);
    pkt1.push_back(0x2B);
    std::vector<uint8_t> pkt2;
    pkt2.push_back(0x02);
    pkt2.push_back(0x0B);
    pkt2.insert(pkt2.end(), payload.begin() + 16, payload.end());
    packets.push_back(std::move(pkt1));
    packets.push_back(std::move(pkt2));
  } else {
    packets.push_back(std::move(payload));
  }
  return packets;
}

std::vector<uint8_t> build_set_clock_command(uint16_t year, uint8_t month, uint8_t day,
                                             uint8_t hour, uint8_t minute, uint8_t second,
                                             uint8_t weekday, uint8_t tz_index) {
  // year is sent as the offset from 2000. Clamp below 2000 to 0 so the byte
  // never underflows; the caller guarantees a valid synced clock before calling.
  uint8_t year_byte = (year >= 2000) ? (uint8_t) (year - 2000) : 0;
  return {0x02, 0x01, year_byte, month, day, hour, minute, second, weekday, tz_index};
}

}  // namespace oclean
}  // namespace esphome
