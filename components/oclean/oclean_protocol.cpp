#include "oclean_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace esphome::oclean {

const char *const OCLEAN_SERVICE_UUID = "8082caa8-41a6-4021-91c6-56f9b954cc18";
const char *const WRITE_CHAR_UUID = "9d84b9a3-000c-49d8-9183-855b673fbb85";
const char *const READ_NOTIFY_CHAR_UUID = "5f78df94-798c-46f5-990a-855b673fbb86";
const char *const SEND_BRUSH_CMD_UUID = "5f78df94-798c-46f5-990a-855b673fbb89";
const char *const RECEIVE_BRUSH_UUID = "5f78df94-798c-46f5-990a-855b673fbb90";

std::string dis_printable_text(const uint8_t *data, size_t len) {
  std::string out;
  if (data == nullptr)
    return out;
  for (size_t i = 0; i < len; i++) {
    if (data[i] >= 0x20 && data[i] <= 0x7E)
      out += static_cast<char>(data[i]);
  }
  return out;
}

bool decode_hw_revision_code(const uint8_t *data, size_t len, HwRevisionCode *out) {
  if (data == nullptr || out == nullptr || len < 6)
    return false;
  if (data[0] != 'H' || data[1] != 'H')
    return false;
  out->protocol = u16be(data + 2);
  out->ota_type = u16be(data + 4);
  return true;
}

std::string hw_revision_text(const uint8_t *data, size_t len) {
  HwRevisionCode code{};
  if (!decode_hw_revision_code(data, len, &code))
    return dis_printable_text(data, len);
  char buf[16];
  snprintf(buf, sizeof(buf), "HH %04X/%04X", static_cast<unsigned>(code.protocol),
           static_cast<unsigned>(code.ota_type));
  return buf;
}

std::string gatt_props_text(uint8_t props) {
  static constexpr uint8_t PROP_READ = 0x02;
  static constexpr uint8_t PROP_WRITE_NO_RSP = 0x04;
  static constexpr uint8_t PROP_WRITE = 0x08;
  static constexpr uint8_t PROP_NOTIFY = 0x10;
  static constexpr uint8_t PROP_INDICATE = 0x20;
  std::string out;
  if ((props & PROP_READ) != 0)
    out += 'R';
  if ((props & PROP_WRITE) != 0)
    out += 'W';
  if ((props & PROP_WRITE_NO_RSP) != 0)
    out += 'w';
  if ((props & PROP_NOTIFY) != 0)
    out += 'N';
  if ((props & PROP_INDICATE) != 0)
    out += 'I';
  if (out.empty())
    out = "-";
  return out;
}

std::string ble_uuid_text(const uint8_t *le, size_t len) {
  if (le == nullptr)
    return "?";
  char buf[12];
  if (len == 2) {
    snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(le[0] | (le[1] << 8)));
    return buf;
  }
  if (len == 4) {
    uint32_t const v = uint32_t(le[0]) | (uint32_t(le[1]) << 8) | (uint32_t(le[2]) << 16) | (uint32_t(le[3]) << 24);
    snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(v));
    return buf;
  }
  if (len != 16)
    return "?";
  static const char HEX_DIGITS[] = "0123456789abcdef";
  std::string out;
  for (size_t k = 0; k < 16; k++) {
    size_t const i = 15 - k;
    out += HEX_DIGITS[le[i] >> 4];
    out += HEX_DIGITS[le[i] & 0x0F];
    if (i == 12 || i == 10 || i == 8 || i == 6)
      out += '-';
  }
  return out;
}

bool parse_battery_level(const uint8_t *data, size_t len, uint8_t *out) {
  if (data == nullptr || out == nullptr)
    return false;
  if (len < 1)
    return false;
  uint8_t const level = data[0];
  if (level > 100)
    return false;
  *out = level;
  return true;
}

bool parse_status_response(const uint8_t *data, size_t len, StatusResponse *out) {
  if (data == nullptr || out == nullptr)
    return false;
  if (len < 6)
    return false;
  if (data[0] != 0x03 || data[1] != 0x03)
    return false;
  uint8_t const battery = data[5];
  if (battery > 100)
    return false;
  out->battery = battery;
  out->charging_raw = data[2];
  return true;
}

bool parse_settings_clock(const uint8_t *data, size_t len, SettingsClock *out) {
  if (data == nullptr || out == nullptr)
    return false;
  if (len < 8)
    return false;
  if (data[0] != 0x03 || data[1] != 0x02)
    return false;
  // The device sends more than one 0302-prefixed message; only the clock has
  // in-range calendar fields. Range-check to reject the other 0302 payload
  // instead of decoding it into a nonsense date.
  uint8_t const month = data[3];
  uint8_t const day = data[4];
  uint8_t const hour = data[5];
  uint8_t const minute = data[6];
  uint8_t const second = data[7];
  if (month < 1 || month > 12)
    return false;
  if (day < 1 || day > 31)
    return false;
  if (hour > 23 || minute > 59 || second > 59)
    return false;
  out->year = uint16_t(2000) + data[2];
  out->month = month;
  out->day = day;
  out->hour = hour;
  out->minute = minute;
  out->second = second;
  return true;
}

void SettingsAssembler::reset() {
  for (unsigned char &i : buf_)
    i = 0;
  got_start_ = false;
  got_cont_ = false;
}

bool SettingsAssembler::feed(const uint8_t *data, size_t len) {
  if (data == nullptr)
    return false;
  if (len < 20)
    return false;
  if (data[0] != 0x03 || data[1] != 0x02)
    return false;
  if (data[2] == 0x23 && data[3] == 0x24) {
    // Start frame: 16 payload bytes at data[4..20) map to buffer[0..16).
    for (size_t i = 0; i < 16; i++)
      buf_[i] = data[4 + i];
    got_start_ = true;
  } else {
    // Continuation frame: 18 payload bytes at data[2..20) map to buffer[16..34).
    for (size_t i = 0; i < 18; i++)
      buf_[16 + i] = data[2 + i];
    got_cont_ = true;
  }
  return complete();
}

void parse_device_settings(const uint8_t *buf, DeviceSettings *out) {
  if (buf == nullptr || out == nullptr)
    return;
  // Start-frame region (buffer 0..15).
  out->device_theme = buf[0];
  out->brush_pause = buf[1] != 0;
  out->raise_wake = buf[2] != 0;
  out->fill_brush = buf[3] != 0;
  out->auto_mode = buf[4] != 0;
  out->volume_enabled = buf[8] == 0;  // inverted: 0 means enabled
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
  out->clock_valid = out->month >= 1 && out->month <= 12 && out->day >= 1 && out->day <= 31 && out->hour <= 23 &&
                     out->minute <= 59 && out->second <= 59;
  out->over_pressure = buf[22] != 0;
  out->area_reminder = buf[23] != 0;
  out->tz_index = buf[24];
  out->head_max = u16be(buf + 25);
  out->head_used_days = u16be(buf + 27);
  out->head_used_times = u16be(buf + 29);
  out->device_language = buf[31];
}

void parse_device_settings_v20_start(const uint8_t *buf, DeviceSettingsV20Start *out) {
  if (buf == nullptr || out == nullptr)
    return;
  out->battery = buf[0];
  out->network_status = buf[1];
  out->raise_wake = buf[2] != 0;
  out->auto_update = buf[3] != 0;
  out->auto_mode = buf[4] != 0;
  out->mode_count = buf[5];
  out->bus_brushing = buf[6];
  out->voice = buf[7] != 0;
  out->voice_zone_change = buf[8] != 0;
  out->voice_pressure = buf[9] != 0;
  out->festival_reminder = buf[10] != 0;
  out->mode = buf[11];
  out->brush_mode_on = buf[12] != 0xEC;
  out->scheme_type = buf[13];
  out->head_used_time = u16be(buf + 14);
}

bool decode_brush_areas_push(const uint8_t *data, size_t len, BrushAreasPush *out) {
  if (data == nullptr || out == nullptr)
    return false;
  if (len < 2)
    return false;
  bool const is_y3p = (data[0] == 0x02 && data[1] == 0x1F);
  bool const is_t1 = (data[0] == 0x26 && data[1] == 0x04);
  if (!is_y3p && !is_t1)
    return false;
  if (len < BRUSH_AREAS_VALUE_OFFSET + BRUSH_AREAS_COUNT)
    return false;
  for (size_t i = 0; i < BRUSH_AREAS_COUNT; i++)
    out->values[i] = data[BRUSH_AREAS_VALUE_OFFSET + i];
  return true;
}

bool decode_session_record(const uint8_t *rec, SessionRecord *out) {
  if (rec == nullptr || out == nullptr)
    return false;
  out->year = uint16_t(2000) + rec[0];
  out->month = rec[1];
  out->day = rec[2];
  out->hour = rec[3];
  out->minute = rec[4];
  out->second = rec[5];
  out->scheme = rec[6];
  out->duration_s = u16be(rec + 7);
  out->valid_duration_s = u16be(rec + 9);
  for (size_t i = 0; i < 5; i++)
    out->areas[i] = rec[11 + i];
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    out->zones[i] = rec[SESSION_ZONES_OFFSET + i];
  uint8_t const s = rec[SESSION_SCORE_OFFSET];
  out->has_score = (s != SESSION_NO_SCORE);
  out->score = s;
  return true;
}

bool decode_inline_0307(const uint8_t *data, size_t len, SessionRecord *out) {
  if (data == nullptr || out == nullptr)
    return false;
  // 0307 marker, *B# magic, record count 0, then the head of the newest
  // record. The fixed head (date-time, scheme, duration, valid duration) is
  // 11 bytes; up to two leading area bytes follow within one notify.
  static const uint8_t HEAD[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00};
  static const size_t HEAD_LEN = sizeof(HEAD);
  if (len < HEAD_LEN + 11)
    return false;
  for (size_t i = 0; i < HEAD_LEN; i++) {
    if (data[i] != HEAD[i])
      return false;
  }
  const uint8_t *r = data + HEAD_LEN;
  // An all-zero date means the device holds no session at all.
  if (r[0] == 0 && r[1] == 0 && r[2] == 0)
    return false;
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
  for (unsigned char &zone : out->zones)
    zone = 0;
  out->score = 0;
  out->has_score = false;
  return true;
}

bool session_record_newer(const SessionRecord &a, const SessionRecord &b) {
  if (a.year != b.year)
    return a.year > b.year;
  if (a.month != b.month)
    return a.month > b.month;
  if (a.day != b.day)
    return a.day > b.day;
  if (a.hour != b.hour)
    return a.hour > b.hour;
  if (a.minute != b.minute)
    return a.minute > b.minute;
  return a.second > b.second;
}

int64_t civil_to_epoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
  // Days from 1970-01-01 for a proleptic Gregorian date (days-from-civil). The
  // fields are treated as UTC: differences between two values are the true
  // elapsed seconds, which is what the drift comparison and session ordering
  // need.
  int32_t y = static_cast<int32_t>(year);
  uint32_t const m = month;
  uint32_t const d = day;
  y -= (m <= 2) ? 1 : 0;
  int32_t const era = (y >= 0 ? y : y - 399) / 400;
  uint32_t const yoe = static_cast<uint32_t>(y - (era * 400));               // [0, 399]
  uint32_t const doy = (((153 * (m > 2 ? m - 3 : m + 9)) + 2) / 5) + d - 1;  // [0, 365]
  uint32_t const doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;          // [0, 146096]
  int32_t const days = (era * 146097) + static_cast<int32_t>(doe) - 719468;
  return (static_cast<int64_t>(days) * 86400) + (static_cast<int64_t>(hour) * 3600) +
         (static_cast<int64_t>(minute) * 60) + static_cast<int64_t>(second);
}

uint32_t session_record_epoch(const SessionRecord &r) {
  // Ordering / dedup key, not a display time. Brush-clock drift is not
  // corrected. Clamp to 0 so the unsigned key never wraps on a degenerate date.
  int64_t secs = civil_to_epoch(r.year, r.month, r.day, r.hour, r.minute, r.second);
  if (secs < 0)
    secs = 0;
  // A malformed record with a wild future date would otherwise persist a
  // watermark that suppresses every later session. Treat anything past the
  // uint32 range as unordered, same as the negative clamp.
  if (secs > static_cast<int64_t>(UINT32_MAX))
    return 0;
  return static_cast<uint32_t>(secs);
}

bool session_epoch_plausible(uint32_t epoch, const SessionClocks &clocks) {
  auto const e = static_cast<int64_t>(epoch);
  auto const node_margin = static_cast<int64_t>(SESSION_FUTURE_MARGIN_S);
  if (clocks.node_local > 0 && e > clocks.node_local + node_margin)
    return false;
  bool const brush_usable =
      clocks.brush > 0 && (clocks.node_local <= 0 || clocks.brush >= clocks.node_local - node_margin);
  return !brush_usable || e <= clocks.brush + static_cast<int64_t>(SESSION_BRUSH_CLOCK_MARGIN_S);
}

bool should_resync_clock(int64_t brush_epoch, int64_t local_epoch, uint32_t threshold_s) {
  int64_t drift = brush_epoch - local_epoch;
  if (drift < 0)
    drift = -drift;
  return drift > static_cast<int64_t>(threshold_s);
}

int64_t clock_set_shift(int64_t brush_read, uint32_t ms_since_read, int64_t written) {
  return brush_read + static_cast<int64_t>(ms_since_read / 1000) - written;
}

bool clock_shift_applies(int64_t shift_s) {
  return shift_s > 0 && shift_s <= static_cast<int64_t>(SESSION_FUTURE_MARGIN_S);
}

bool clock_set_confirmed(int64_t shift_s, int64_t drift_s) {
  int64_t const magnitude = drift_s < 0 ? -drift_s : drift_s;
  return shift_s > 0 && magnitude * 2 < shift_s;
}

uint32_t shift_epoch_back(uint32_t epoch, int64_t shift_s) {
  if (!clock_shift_applies(shift_s) || static_cast<int64_t>(epoch) <= shift_s)
    return epoch;
  return epoch - static_cast<uint32_t>(shift_s);
}

bool poll_is_due(uint32_t since_ms, bool docked, uint32_t charging_interval_ms, uint32_t battery_interval_ms) {
  return since_ms >= (docked ? charging_interval_ms : battery_interval_ms);
}

bool should_hold_link(bool hold_option, bool ble_enabled, bool docked, bool round_status_seen) {
  return hold_option && ble_enabled && docked && round_status_seen;
}

PollDecision plan_poll_tick(const PollTickState &s) {
  if (!s.ble_enabled)
    return {.action = PollAction::SKIP_BLE_OFF, .defer_ms = 0};
  // Skip rather than stack connect attempts; the watchdog ends a stalled cycle.
  if (s.link_busy)
    return {.action = PollAction::SKIP_LINK_BUSY, .defer_ms = 0};
  if (!s.boot_stagger_done && s.poll_pending && s.now_ms < s.boot_stagger_ms) {
    // The radio scans one target at a time, so simultaneous first cycles make
    // the losing hub burn its whole window.
    return {.action = PollAction::DEFER_BOOT_STAGGER, .defer_ms = s.boot_stagger_ms - s.now_ms};
  }
  if (s.adaptive && !s.poll_pending &&
      !poll_is_due(s.now_ms - s.last_poll_ms, s.docked, s.charging_interval_ms, s.battery_interval_ms)) {
    return {.action = PollAction::SKIP_NOT_DUE, .defer_ms = 0};
  }
  return {.action = PollAction::POLL, .defer_ms = 0};
}

float session_coverage_percent(uint16_t valid_duration_s, uint16_t duration_s) {
  if (duration_s == 0)
    return NAN;
  float const pct = roundf(100.0f * static_cast<float>(valid_duration_s) / static_cast<float>(duration_s));
  return pct > 100.0f ? 100.0f : pct;
}

bool same_session(const SessionRecord &a, const SessionRecord &b) {
  return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour && a.minute == b.minute &&
         a.second == b.second && a.scheme == b.scheme && a.duration_s == b.duration_s;
}

bool accept_inline_record(const SessionRecord &inl, uint32_t newest_epoch, const SessionRecord *shown,
                          const SessionClocks &clocks) {
  uint32_t const epoch = session_record_epoch(inl);
  if (epoch <= newest_epoch || (shown != nullptr && same_session(inl, *shown)))
    return false;
  return session_epoch_plausible(epoch, clocks);
}

uint32_t restored_newest_epoch(uint32_t record_epoch, uint32_t watermark) {
  if (watermark == 0)
    return record_epoch;
  return record_epoch < watermark ? record_epoch : watermark;
}

SessionIngestPlan plan_session_ingest(const std::vector<SessionRecord> &records, uint32_t watermark,
                                      uint32_t newest_epoch, const SessionClocks &clocks) {
  SessionIngestPlan plan;
  plan.new_watermark = watermark;
  plan.new_newest_epoch = newest_epoch;
  plan.persist_newest = false;
  plan.newest_plausible = false;
  plan.have_newest = false;
  plan.newest = SessionRecord{};

  for (const auto &rec : records) {
    uint32_t const ts = session_record_epoch(rec);
    if (ts <= watermark)
      continue;  // already emitted, possibly before a reboot
    if (!session_epoch_plausible(ts, clocks)) {
      // A date far in the future would push the watermark past every real
      // session and mute them for good, so it never advances it.
      plan.implausible.push_back(rec);
      continue;
    }
    plan.to_publish.push_back(rec);
    if (ts > plan.new_watermark)
      plan.new_watermark = ts;
  }
  // The ring is not stored chronologically. Field-wise compare, not the epoch:
  // same ordering without running the days-from-civil arithmetic twice per
  // comparison.
  std::ranges::sort(plan.to_publish,
                    [](const SessionRecord &a, const SessionRecord &b) { return session_record_newer(b, a); });

  // unordered ring, so the newest is the maximum by timestamp. Ties keep the
  // earlier slot, same as the reassembler's own scan.
  for (const auto &rec : records) {
    if (!plan.have_newest || session_record_newer(rec, plan.newest)) {
      plan.newest = rec;
      plan.have_newest = true;
    }
  }
  if (plan.have_newest) {
    uint32_t const epoch = session_record_epoch(plan.newest);
    plan.newest_plausible = session_epoch_plausible(epoch, clocks);
    // strictly newer only. A peer re-serving one ring every poll would grind the
    // flash, and an older epoch would lower the gate that keeps a stale record
    // from overwriting the stored one.
    if (plan.newest_plausible && epoch > newest_epoch) {
      plan.persist_newest = true;
      plan.new_newest_epoch = epoch;
    }
  }
  return plan;
}

void SessionAssembler::reset() {
  got_ = 0;
  need_ = 0;
  count_ = 0;
  started_ = false;
  failed_ = false;
}

void SessionAssembler::append_(const uint8_t *data, size_t len) {
  size_t const room = need_ - got_;
  size_t const n = (len < room) ? len : room;
  for (size_t i = 0; i < n; i++)
    buf_[got_ + i] = data[i];
  got_ += n;
}

bool SessionAssembler::feed(const uint8_t *data, size_t len) {
  if (failed_)
    return false;
  if (data == nullptr) {
    failed_ = true;
    return false;
  }
  if (complete())
    return true;  // ignore trailing input
  if (!started_) {
    if (len < SESSION_HEADER_LEN) {
      failed_ = true;
      return false;
    }
    if (data[0] != 0x03 || data[1] != 0x07) {
      failed_ = true;
      return false;
    }
    if (data[2] != SESSION_MAGIC[0] || data[3] != SESSION_MAGIC[1] || data[4] != SESSION_MAGIC[2]) {
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
  if (!complete() || i >= count_)
    return false;
  return decode_session_record(buf_ + (size_t(i) * SESSION_RECORD_SIZE), out);
}

const uint8_t *SessionAssembler::raw_record(uint16_t i) const {
  if (!complete() || i >= count_)
    return nullptr;
  return buf_ + (size_t(i) * SESSION_RECORD_SIZE);
}

int SessionAssembler::newest_index() const {
  if (!complete() || count_ == 0)
    return -1;
  int best = 0;
  SessionRecord best_rec{};
  decode_session_record(buf_, &best_rec);
  for (uint16_t i = 1; i < count_; i++) {
    SessionRecord cur{};
    decode_session_record(buf_ + (size_t(i) * SESSION_RECORD_SIZE), &cur);
    if (session_record_newer(cur, best_rec)) {
      best_rec = cur;
      best = i;
    }
  }
  return best;
}

static bool is_session_header(const uint8_t *data, size_t len) {
  return len >= SESSION_V20_HEADER_LEN && data[0] == 0x03 && data[1] == 0x07 && data[2] == SESSION_MAGIC[0] &&
         data[3] == SESSION_MAGIC[1] && data[4] == SESSION_MAGIC[2];
}

static void decode_v20_head(const uint8_t *rec, SessionRecord *out) {
  *out = SessionRecord{};
  out->year = uint16_t(2000) + rec[2];
  out->month = rec[3];
  out->day = rec[4];
  out->hour = rec[5];
  out->minute = rec[6];
  out->second = rec[7];
  out->scheme = rec[8];
  out->duration_s = u16be(rec + 9);
  for (uint8_t &zone : out->zones)
    zone = SESSION_ZONE_ABSENT;
  out->score = SESSION_NO_SCORE;
  out->has_score = false;
}

bool decode_session_record_v20(const uint8_t *rec, size_t len, SessionRecord *out) {
  if (rec == nullptr || out == nullptr || len < SESSION_V20_RECORD_MIN)
    return false;
  decode_v20_head(rec, out);
  out->valid_duration_s = u16be(rec + 11);
  for (size_t i = 0; i < 5; i++)
    out->areas[i] = rec[13 + i];
  out->score = rec[SESSION_V20_SCORE_OFFSET];
  out->has_score = out->score != SESSION_NO_SCORE;
  return true;
}

bool decode_inline_0307_v20(const uint8_t *data, size_t len, SessionRecord *out) {
  if (data == nullptr || out == nullptr || len < SESSION_V20_HEADER_LEN + SESSION_V20_INLINE_LEN)
    return false;
  if (!is_session_header(data, len) || u16be(data + 5) != 0)
    return false;
  const uint8_t *rec = data + SESSION_V20_HEADER_LEN;
  uint16_t const rec_len = u16be(rec);
  if (rec_len < SESSION_V20_RECORD_MIN || rec_len > SESSION_V20_RECORD_MAX)
    return false;
  decode_v20_head(rec, out);
  return true;
}

void VarSessionAssembler::reset() {
  buf_.clear();
  spans_.clear();
  need_ = 0;
  count_ = 0;
  started_ = false;
  failed_ = false;
}

bool VarSessionAssembler::feed(const uint8_t *data, size_t len) {
  if (failed_)
    return false;
  if (data == nullptr) {
    failed_ = true;
    return false;
  }
  if (complete() || this->empty())
    return complete();
  size_t skip = 0;
  if (!started_) {
    if (!is_session_header(data, len)) {
      failed_ = true;
      return false;
    }
    count_ = u16be(data + 5);
    need_ = u16be(data + 7);
    started_ = true;
    if (count_ == 0)
      return false;
    if (count_ > SESSION_V20_MAX_RECORDS || need_ < SESSION_V20_RECORD_MIN || need_ > SESSION_V20_MAX_BYTES) {
      failed_ = true;
      return false;
    }
    buf_.reserve(need_);
    skip = SESSION_V20_HEADER_LEN;
  }
  size_t const take = std::min(len - skip, need_ - buf_.size());
  buf_.insert(buf_.end(), data + skip, data + skip + take);
  if (complete())
    this->split_();
  return complete();
}

void VarSessionAssembler::split_() {
  spans_.clear();
  size_t offset = 0;
  while (offset + 2 <= buf_.size() && spans_.size() < count_) {
    size_t const rec_len = u16be(buf_.data() + offset);
    if (rec_len < SESSION_V20_RECORD_MIN || rec_len > SESSION_V20_RECORD_MAX || offset + rec_len > buf_.size())
      break;
    spans_.push_back(Span{.offset = offset, .len = rec_len});
    offset += rec_len;
  }
}

bool VarSessionAssembler::record(size_t i, SessionRecord *out) const {
  if (i >= spans_.size())
    return false;
  return decode_session_record_v20(buf_.data() + spans_[i].offset, spans_[i].len, out);
}

const uint8_t *VarSessionAssembler::raw_record(size_t i, size_t *len) const {
  if (i >= spans_.size() || len == nullptr)
    return nullptr;
  *len = spans_[i].len;
  return buf_.data() + spans_[i].offset;
}

int VarSessionAssembler::newest_index() const {
  int best = -1;
  SessionRecord best_rec{};
  for (size_t i = 0; i < spans_.size(); i++) {
    SessionRecord cur{};
    if (!this->record(i, &cur))
      continue;
    if (best < 0 || session_record_newer(cur, best_rec)) {
      best_rec = cur;
      best = static_cast<int>(i);
    }
  }
  return best;
}

std::vector<uint8_t> build_toggle_command(uint8_t b0, uint8_t b1, uint8_t on_value, uint8_t off_value, bool state) {
  return {b0, b1, state ? on_value : off_value};
}

const char *timezone_index_to_string(uint8_t wire_index) {
  static const char *const TABLE[33] = {
      "GMT-12:00", "GMT-11:00", "GMT-10:00", "GMT-09:00", "GMT-08:00", "GMT-07:00", "GMT-06:00",
      "GMT-05:00", "GMT-04:00", "GMT-03:30", "GMT-03:00", "GMT-02:00", "GMT-01:00", "GMT+00:00",
      "GMT+01:00", "GMT+02:00", "GMT+03:00", "GMT+03:30", "GMT+04:00", "GMT+04:30", "GMT+05:00",
      "GMT+05:30", "GMT+05:45", "GMT+06:00", "GMT+06:30", "GMT+07:00", "GMT+08:00", "GMT+09:00",
      "GMT+09:30", "GMT+10:00", "GMT+11:00", "GMT+12:00", "GMT+13:00",
  };
  if (wire_index < 1 || wire_index > 33)
    return "unknown";
  return TABLE[wire_index - 1];
}

uint8_t tz_index_for_offset_seconds(int32_t offset_seconds) {
  // Seconds per entry, same order as the string table above.
  static const int32_t OFFSETS[33] = {
      -43200, -39600, -36000, -32400, -28800,  // -12:00 .. -08:00
      -25200, -21600, -18000, -14400, -12600,  // -07:00 .. -03:30
      -10800, -7200,  -3600,  0,               // -03:00 .. +00:00
      3600,   7200,   10800,  12600,           // +01:00 .. +03:30
      14400,  16200,  18000,  19800,  20700,   // +04:00 .. +05:45
      21600,  23400,  25200,  28800,  32400,   // +06:00 .. +09:00
      34200,  36000,  39600,  43200,  46800,   // +09:30 .. +13:00
  };
  for (uint8_t i = 0; i < 33; i++) {
    if (OFFSETS[i] == offset_seconds)
      return static_cast<uint8_t>(i + 1);
  }
  return 0;
}

std::vector<uint8_t> build_language_command(uint8_t lang_id) {
  return {0x02, 0x16, lang_id};
}

uint8_t encode_scheme_gear(uint8_t gear) {
  switch (gear) {
    case 1:
      return 5;
    case 2:
      return 6;
    case 3:
      return 7;
    case 4:
      return 8;
    case 5:
      return 17;
    case 6:
      return 18;
    case 7:
      return 19;
    case 8:
      return 20;
    case 9:
      return 21;
    case 10:
      return 22;
    case 11:
      return 23;
    case 12:
      return 24;
    default:
      return 0;
  }
}

std::vector<std::vector<uint8_t>> build_scheme_packets(uint8_t pnum, const std::vector<SchemeStep> &steps) {
  // A program is 1 to 8 steps; reject anything else so stepCount stays a valid
  // byte and the caller never sends a degenerate program.
  if (steps.empty() || steps.size() > 8)
    return {};
  std::vector<uint8_t> payload;
  payload.push_back(0x02);
  payload.push_back(0x06);
  payload.push_back(pnum);
  payload.push_back(static_cast<uint8_t>(steps.size()));
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

std::vector<uint8_t> build_set_clock_command(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                             uint8_t second, uint8_t weekday, uint8_t tz_index) {
  // year is sent as the offset from 2000. Clamp below 2000 to 0 so the byte
  // never underflows; the caller guarantees a valid synced clock before calling.
  uint8_t const year_byte = (year >= 2000) ? static_cast<uint8_t>(year - 2000) : 0;
  return {0x02, 0x01, year_byte, month, day, hour, minute, second, weekday, tz_index};
}

int64_t set_clock_command_epoch(const uint8_t *cmd, size_t len) {
  if (cmd == nullptr || len != SET_CLOCK_CMD_LEN || cmd[0] != 0x02 || cmd[1] != 0x01)
    return 0;
  return civil_to_epoch(static_cast<uint16_t>(2000 + cmd[2]), cmd[3], cmd[4], cmd[5], cmd[6], cmd[7]);
}

}  // namespace esphome::oclean
