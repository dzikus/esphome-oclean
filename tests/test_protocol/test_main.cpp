// Unit tests for oclean_protocol. Run `pio test -e native` from tests/.
// Fixtures in fixtures.h.
#include <unity.h>

#include "fixtures.h"
#include "oclean_profile.h"
#include "oclean_protocol.h"
// Pure C++ functions in oclean_protocol via single-TU include (no separate .o linkage).
#include "../../components/oclean/oclean_protocol.cpp"
// Profile table + model-to-profile lookup, same single-TU include style.
#include "../../components/oclean/oclean_profile.cpp"

using namespace esphome::oclean;

void setUp() {}
void tearDown() {}

// === parse_battery_level ===

void test_parse_battery_valid() {
  uint8_t out = 0xFF;
  TEST_ASSERT_TRUE(parse_battery_level(fixtures::BATTERY_90, sizeof(fixtures::BATTERY_90), &out));
  TEST_ASSERT_EQUAL_UINT8(90, out);
}

void test_parse_battery_zero() {
  const uint8_t buf[] = {0x00};
  uint8_t out = 0xFF;
  TEST_ASSERT_TRUE(parse_battery_level(buf, sizeof(buf), &out));
  TEST_ASSERT_EQUAL_UINT8(0, out);
}

void test_parse_battery_full() {
  const uint8_t buf[] = {0x64};  // 100
  uint8_t out = 0xFF;
  TEST_ASSERT_TRUE(parse_battery_level(buf, sizeof(buf), &out));
  TEST_ASSERT_EQUAL_UINT8(100, out);
}

void test_parse_battery_over_100_rejected() {
  const uint8_t buf[] = {0x65};  // 101
  uint8_t out = 0xFF;
  TEST_ASSERT_FALSE(parse_battery_level(buf, sizeof(buf), &out));
  TEST_ASSERT_EQUAL_UINT8(0xFF, out);  // out untouched on failure
}

void test_parse_battery_empty_rejected() {
  const uint8_t buf[] = {0x00};
  uint8_t out = 0xFF;
  TEST_ASSERT_FALSE(parse_battery_level(buf, 0, &out));
  TEST_ASSERT_EQUAL_UINT8(0xFF, out);
}

void test_parse_battery_multibyte_takes_first() {
  const uint8_t buf[] = {0x32, 0xFF, 0xAA};  // 50, trailing junk ignored
  uint8_t out = 0xFF;
  TEST_ASSERT_TRUE(parse_battery_level(buf, sizeof(buf), &out));
  TEST_ASSERT_EQUAL_UINT8(50, out);
}

// === u16be ===

void test_u16be_basic() {
  const uint8_t buf[] = {0x01, 0x2C};  // 300
  TEST_ASSERT_EQUAL_UINT16(300, u16be(buf));
  const uint8_t buf2[] = {0xFF, 0x00};
  TEST_ASSERT_EQUAL_UINT16(0xFF00, u16be(buf2));
}

// === Session stream constants ===

// A first packet whose magic does not match must latch the failed state instead
// of reassembling whatever follows as records.
void test_assembler_rejects_wrong_magic() {
  const uint8_t bad[] = {0x03, 0x07, 0x2A, 0x42, 0x24, 0x00, 0x01};
  SessionAssembler asm_;
  TEST_ASSERT_FALSE(asm_.feed(bad, sizeof(bad)));
  TEST_ASSERT_TRUE(asm_.failed());
  TEST_ASSERT_FALSE(asm_.started());
  // once failed it stays failed, even when a valid header follows
  const uint8_t good[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x01};
  TEST_ASSERT_FALSE(asm_.feed(good, sizeof(good)));
  TEST_ASSERT_TRUE(asm_.failed());
}

// === decode_session_record (real captured records) ===

void test_decode_record_normal() {
  SessionRecord r;
  TEST_ASSERT_TRUE(decode_session_record(fixtures::SESSION_REC_NORMAL, &r));
  TEST_ASSERT_EQUAL_UINT16(2026, r.year);
  TEST_ASSERT_EQUAL_UINT8(5, r.month);
  TEST_ASSERT_EQUAL_UINT8(1, r.day);
  TEST_ASSERT_EQUAL_UINT8(14, r.hour);
  TEST_ASSERT_EQUAL_UINT8(25, r.minute);
  TEST_ASSERT_EQUAL_UINT8(13, r.second);
  TEST_ASSERT_EQUAL_UINT16(120, r.duration_s);
  TEST_ASSERT_EQUAL_UINT16(120, r.valid_duration_s);
  TEST_ASSERT_EQUAL_UINT8(15, r.tz_index);
  const uint8_t quadrants[SESSION_QUADRANTS_COUNT] = {31, 14, 37, 18};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(quadrants, r.quadrants, SESSION_QUADRANTS_COUNT);
  TEST_ASSERT_TRUE(r.has_score);
  TEST_ASSERT_EQUAL_UINT8(82, r.score);
  TEST_ASSERT_FALSE(session_voided(r));
}

void test_decode_record_quadrants_follow_zone_pairs() {
  // the brush rounds each quadrant on its own, so a pair of zones lands within a
  // few points of it, never on the other side
  SessionRecord r;
  TEST_ASSERT_TRUE(decode_session_record(fixtures::SESSION_REC_HIGH, &r));
  unsigned sum = 0;
  for (size_t q = 0; q < SESSION_QUADRANTS_COUNT; q++) {
    sum += r.quadrants[q];
    int const pair = r.zones[2 * q] + r.zones[(2 * q) + 1];
    TEST_ASSERT_INT_WITHIN(3, pair, r.quadrants[q]);
  }
  TEST_ASSERT_EQUAL_UINT(100, sum);
}

void test_decode_record_gesture_zones_offset23() {
  // gestureArray is bytes 23-30 of the record (left 0-3, right 4-7).
  SessionRecord r;
  TEST_ASSERT_TRUE(decode_session_record(fixtures::SESSION_REC_NORMAL, &r));
  const uint8_t expected[SESSION_ZONES_COUNT] = {10, 21, 11, 2, 14, 22, 12, 3};
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    TEST_ASSERT_EQUAL_UINT8(expected[i], r.zones[i]);
}

void test_decode_record_aborted() {
  // 2 s of brushing: the firmware voids the session and scores it 1, which is
  // not a score to publish
  SessionRecord r;
  TEST_ASSERT_TRUE(decode_session_record(fixtures::SESSION_REC_ABORTED, &r));
  TEST_ASSERT_EQUAL_UINT16(2, r.valid_duration_s);
  TEST_ASSERT_FALSE(r.has_score);
  TEST_ASSERT_EQUAL_UINT8(1, r.score);
  TEST_ASSERT_TRUE(session_voided(r));
  TEST_ASSERT_EQUAL_UINT8(0, r.quadrants[0]);
}

void test_decode_record_no_score() {
  // Synthetic: take the normal record and blank the score byte (offset 33).
  uint8_t rec[SESSION_RECORD_SIZE];
  for (size_t i = 0; i < SESSION_RECORD_SIZE; i++)
    rec[i] = fixtures::SESSION_REC_NORMAL[i];
  rec[33] = SESSION_NO_SCORE;
  SessionRecord r;
  TEST_ASSERT_TRUE(decode_session_record(rec, &r));
  TEST_ASSERT_FALSE(r.has_score);
}

void test_decode_record_null_rejected() {
  SessionRecord r;
  TEST_ASSERT_FALSE(decode_session_record(nullptr, &r));
  TEST_ASSERT_FALSE(decode_session_record(fixtures::SESSION_REC_NORMAL, nullptr));
}

void test_record_newer_compares_timestamp() {
  SessionRecord a, b;
  decode_session_record(fixtures::SESSION_REC_NORMAL, &a);  // 05-01 14:25:13
  decode_session_record(fixtures::SESSION_REC_HIGH, &b);    // 05-02 09:09:44
  TEST_ASSERT_TRUE(session_record_newer(b, a));
  TEST_ASSERT_FALSE(session_record_newer(a, b));
  TEST_ASSERT_FALSE(session_record_newer(a, a));
}

// === session_record_epoch (civil fields treated as UTC, ordering/dedup key) ===

void test_session_epoch_known_date() {
  // 2026-06-05 16:37:14 UTC -> 1780677434. Build the record via the real decode
  // path so the field extraction is exercised too (byte 0 = year - 2000 = 0x1A).
  uint8_t rec[SESSION_RECORD_SIZE] = {0x1A, 0x06, 0x05, 0x10, 0x25, 0x0E};
  rec[33] = SESSION_NO_SCORE;
  SessionRecord r;
  TEST_ASSERT_TRUE(decode_session_record(rec, &r));
  TEST_ASSERT_EQUAL_UINT32(1780677434u, session_record_epoch(r));
}

void test_session_epoch_second_date() {
  // Pin the civil algorithm with a second real fixture: the aborted session at
  // 2026-05-02 09:11:44 UTC -> 1777713104.
  SessionRecord r;
  TEST_ASSERT_TRUE(decode_session_record(fixtures::SESSION_REC_ABORTED, &r));
  TEST_ASSERT_EQUAL_UINT32(1777713104u, session_record_epoch(r));
  // And the normal fixture: 2026-05-01 14:25:13 UTC -> 1777645513.
  SessionRecord n;
  TEST_ASSERT_TRUE(decode_session_record(fixtures::SESSION_REC_NORMAL, &n));
  TEST_ASSERT_EQUAL_UINT32(1777645513u, session_record_epoch(n));
}

void test_session_epoch_orders_like_newer() {
  // The epoch must order records the same way session_record_newer does, since
  // the history emit loop uses the epoch as its strictly-newer dedup key.
  SessionRecord older, newer;
  decode_session_record(fixtures::SESSION_REC_NORMAL, &older);   // 05-01 14:25:13
  decode_session_record(fixtures::SESSION_REC_ABORTED, &newer);  // 05-02 09:11:44
  TEST_ASSERT_TRUE(session_record_epoch(newer) > session_record_epoch(older));
  TEST_ASSERT_TRUE(session_record_newer(newer, older));
}

// === SessionAssembler ===

void test_assembler_fragmented_stream() {
  SessionAssembler asm_;
  asm_.reset();
  TEST_ASSERT_FALSE(asm_.feed(fixtures::SESSION_PKT_0, sizeof(fixtures::SESSION_PKT_0)));
  TEST_ASSERT_TRUE(asm_.started());  // header packet latches the stream open
  TEST_ASSERT_EQUAL_UINT16(3, asm_.record_count());
  TEST_ASSERT_FALSE(asm_.feed(fixtures::SESSION_PKT_1, sizeof(fixtures::SESSION_PKT_1)));
  TEST_ASSERT_FALSE(asm_.feed(fixtures::SESSION_PKT_2, sizeof(fixtures::SESSION_PKT_2)));
  TEST_ASSERT_FALSE(asm_.feed(fixtures::SESSION_PKT_3, sizeof(fixtures::SESSION_PKT_3)));
  TEST_ASSERT_FALSE(asm_.feed(fixtures::SESSION_PKT_4, sizeof(fixtures::SESSION_PKT_4)));
  TEST_ASSERT_FALSE(asm_.feed(fixtures::SESSION_PKT_5, sizeof(fixtures::SESSION_PKT_5)));
  TEST_ASSERT_TRUE(asm_.feed(fixtures::SESSION_PKT_6, sizeof(fixtures::SESSION_PKT_6)));
  TEST_ASSERT_TRUE(asm_.complete());

  SessionRecord r0, r2;
  TEST_ASSERT_TRUE(asm_.record(0, &r0));
  TEST_ASSERT_EQUAL_UINT8(82, r0.score);
  TEST_ASSERT_TRUE(asm_.record(2, &r2));
  TEST_ASSERT_EQUAL_UINT8(1, r2.score);
  TEST_ASSERT_FALSE(r2.has_score);
  TEST_ASSERT_EQUAL_UINT16(2, r2.valid_duration_s);
}

void test_assembler_newest_index() {
  SessionAssembler asm_;
  asm_.reset();
  asm_.feed(fixtures::SESSION_PKT_0, sizeof(fixtures::SESSION_PKT_0));
  asm_.feed(fixtures::SESSION_PKT_1, sizeof(fixtures::SESSION_PKT_1));
  asm_.feed(fixtures::SESSION_PKT_2, sizeof(fixtures::SESSION_PKT_2));
  asm_.feed(fixtures::SESSION_PKT_3, sizeof(fixtures::SESSION_PKT_3));
  asm_.feed(fixtures::SESSION_PKT_4, sizeof(fixtures::SESSION_PKT_4));
  asm_.feed(fixtures::SESSION_PKT_5, sizeof(fixtures::SESSION_PKT_5));
  asm_.feed(fixtures::SESSION_PKT_6, sizeof(fixtures::SESSION_PKT_6));
  // rec2 (05-02 09:11:44) is the newest of the three.
  TEST_ASSERT_EQUAL_INT(2, asm_.newest_index());
}

void test_assembler_out_of_range_record() {
  SessionAssembler asm_;
  asm_.reset();
  asm_.feed(fixtures::SESSION_PKT_0, sizeof(fixtures::SESSION_PKT_0));
  // Not complete yet: record() must refuse.
  SessionRecord r;
  TEST_ASSERT_FALSE(asm_.record(0, &r));
}

void test_assembler_bad_header_rejected() {
  SessionAssembler asm_;
  asm_.reset();
  const uint8_t bad[] = {0x03, 0x07, 0xAA, 0xBB, 0xCC, 0x00, 0x01};
  TEST_ASSERT_FALSE(asm_.feed(bad, sizeof(bad)));
  TEST_ASSERT_TRUE(asm_.failed());
  TEST_ASSERT_FALSE(asm_.complete());
}

void test_assembler_zero_count_rejected() {
  SessionAssembler asm_;
  asm_.reset();
  const uint8_t zero[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00};
  TEST_ASSERT_FALSE(asm_.feed(zero, sizeof(zero)));
  TEST_ASSERT_TRUE(asm_.failed());
}

void test_assembler_overlong_count_rejected() {
  SessionAssembler asm_;
  asm_.reset();
  const uint8_t big[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0xFF, 0xFF};
  TEST_ASSERT_FALSE(asm_.feed(big, sizeof(big)));
  TEST_ASSERT_TRUE(asm_.failed());
}

// === STATUS / SETTINGS response parsers (empirical bytes from both brushes) ===

void test_parse_status_response_valid() {
  // Brush A: 03 03 02 10 3A 64 1F 00 -> battery 0x64=100, charging byte2=0x02.
  const uint8_t st[] = {0x03, 0x03, 0x02, 0x10, 0x3A, 0x64, 0x1F, 0x00};
  StatusResponse out;
  TEST_ASSERT_TRUE(parse_status_response(st, sizeof(st), &out));
  TEST_ASSERT_EQUAL_UINT8(100, out.battery);
  TEST_ASSERT_EQUAL_UINT8(0x02, out.charging_raw);
  TEST_ASSERT_EQUAL_UINT16(4154, out.voltage_mv);
}

void test_parse_status_voltage_six_byte_frame() {
  // X Ultra 20 on the dock at 94%: the frame ends at the battery byte
  const uint8_t st[] = {0x03, 0x03, 0x01, 0x10, 0x1B, 0x5E};
  StatusResponse out;
  TEST_ASSERT_TRUE(parse_status_response(st, sizeof(st), &out));
  TEST_ASSERT_EQUAL_UINT8(94, out.battery);
  TEST_ASSERT_EQUAL_UINT16(4123, out.voltage_mv);
  TEST_ASSERT_TRUE(status_voltage_plausible(out.voltage_mv));
  // a Y3P at 40% read 3711 mV
  TEST_ASSERT_TRUE(status_voltage_plausible(3711));
  TEST_ASSERT_FALSE(status_voltage_plausible(0));
  TEST_ASSERT_FALSE(status_voltage_plausible(STATUS_VOLTAGE_MIN_MV - 1));
  TEST_ASSERT_FALSE(status_voltage_plausible(STATUS_VOLTAGE_MAX_MV + 1));
}

void test_status_dock_predicates() {
  // byte2 0x01 = charging on dock: charging and docked both true.
  const uint8_t charging[] = {0x03, 0x03, 0x01, 0x10, 0x3A, 0x64, 0x1F, 0x00};
  StatusResponse a;
  TEST_ASSERT_TRUE(parse_status_response(charging, sizeof(charging), &a));
  TEST_ASSERT_TRUE(status_is_charging(a.charging_raw));
  TEST_ASSERT_TRUE(status_is_docked(a.charging_raw));

  // byte2 0x02 = off dock: neither charging nor docked.
  const uint8_t off_dock[] = {0x03, 0x03, 0x02, 0x0F, 0xD0, 0x5F, 0x01, 0x00};
  StatusResponse b;
  TEST_ASSERT_TRUE(parse_status_response(off_dock, sizeof(off_dock), &b));
  TEST_ASSERT_FALSE(status_is_charging(b.charging_raw));
  TEST_ASSERT_FALSE(status_is_docked(b.charging_raw));

  // byte2 0x03 = on dock fully charged (battery 100%): docked but not charging.
  const uint8_t docked_full[] = {0x03, 0x03, 0x03, 0x10, 0x6A, 0x64, 0x00, 0x00};
  StatusResponse c;
  TEST_ASSERT_TRUE(parse_status_response(docked_full, sizeof(docked_full), &c));
  TEST_ASSERT_EQUAL_UINT8(100, c.battery);
  TEST_ASSERT_FALSE(status_is_charging(c.charging_raw));
  TEST_ASSERT_TRUE(status_is_docked(c.charging_raw));
}

void test_parse_status_response_bad_header_rejected() {
  const uint8_t st[] = {0x03, 0x02, 0x02, 0x10, 0x3A, 0x64};
  StatusResponse out;
  TEST_ASSERT_FALSE(parse_status_response(st, sizeof(st), &out));
}

void test_parse_status_response_over_100_rejected() {
  const uint8_t st[] = {0x03, 0x03, 0x02, 0x10, 0x3A, 101, 0x1F, 0x00};
  StatusResponse out;
  TEST_ASSERT_FALSE(parse_status_response(st, sizeof(st), &out));
}

void test_parse_status_response_short_rejected() {
  const uint8_t st[] = {0x03, 0x03, 0x02};
  StatusResponse out;
  TEST_ASSERT_FALSE(parse_status_response(st, sizeof(st), &out));
}

void test_parse_settings_clock_valid() {
  // Brush A: 03 02 1A 06 04 00 0C 22 ... -> 2026-06-04 00:12:34.
  const uint8_t se[] = {0x03, 0x02, 0x1A, 0x06, 0x04, 0x00, 0x0C, 0x22, 0x01, 0x01};
  SettingsClock out;
  TEST_ASSERT_TRUE(parse_settings_clock(se, sizeof(se), &out));
  TEST_ASSERT_EQUAL_UINT16(2026, out.year);
  TEST_ASSERT_EQUAL_UINT8(6, out.month);
  TEST_ASSERT_EQUAL_UINT8(4, out.day);
  TEST_ASSERT_EQUAL_UINT8(0, out.hour);
  TEST_ASSERT_EQUAL_UINT8(12, out.minute);
  TEST_ASSERT_EQUAL_UINT8(34, out.second);
}

void test_parse_settings_clock_bad_header_rejected() {
  const uint8_t se[] = {0x03, 0x03, 0x1A, 0x06, 0x04, 0x00, 0x0C, 0x22};
  SettingsClock out;
  TEST_ASSERT_FALSE(parse_settings_clock(se, sizeof(se), &out));
}

void test_parse_settings_clock_out_of_range_rejected() {
  // The device also sends a non-clock 0302 message; month 0x24=36 is impossible
  // and must be rejected by the range check rather than decoded.
  const uint8_t se[] = {0x03, 0x02, 0x23, 0x24, 0x0C, 0x01, 0x01, 0x00};
  SettingsClock out;
  TEST_ASSERT_FALSE(parse_settings_clock(se, sizeof(se), &out));
}

// === SettingsAssembler + parse_device_settings (two-frame 0302) ===

void test_settings_continuation_brush_a() {
  // Real continuation frame, Brush A: over-pressure ON, area-reminder ON,
  // head used 1051 days / 1223 sessions, clock 2026-06-04 00:12:34.
  const uint8_t cont[] = {0x03, 0x02, 0x1A, 0x06, 0x04, 0x00, 0x0C, 0x22, 0x01, 0x01, 0x0F,
                          0x00, 0xF0, 0x04, 0x1B, 0x04, 0xC7, 0x0C, 0x00, 0x00, 0x00};
  SettingsAssembler asm_;
  asm_.reset();
  TEST_ASSERT_FALSE(asm_.feed(cont, sizeof(cont)));  // continuation alone is not complete
  TEST_ASSERT_TRUE(asm_.has_cont());
  TEST_ASSERT_FALSE(asm_.complete());
  TEST_ASSERT_FALSE(asm_.has_start());
  // Reading continuation-region fields off a partial (start-missing) buffer is
  // intentional: only the continuation half is exercised here, the start half
  // stays zeroed and is not asserted.
  DeviceSettings ds;
  parse_device_settings(asm_.buffer(), &ds);
  TEST_ASSERT_TRUE(ds.clock_valid);
  TEST_ASSERT_EQUAL_UINT16(2026, ds.year);
  TEST_ASSERT_EQUAL_UINT8(6, ds.month);
  TEST_ASSERT_EQUAL_UINT8(4, ds.day);
  TEST_ASSERT_EQUAL_UINT8(12, ds.minute);
  TEST_ASSERT_EQUAL_UINT8(34, ds.second);
  TEST_ASSERT_TRUE(ds.over_pressure);
  TEST_ASSERT_TRUE(ds.area_reminder);
  TEST_ASSERT_EQUAL_UINT16(1051, ds.head_used_days);
  TEST_ASSERT_EQUAL_UINT16(1223, ds.head_used_times);
}

void test_settings_clock_invalid_fields_rejected() {
  // clock_valid is the only thing standing between a garbled settings frame and
  // an automatic 0201 write of a nonsense clock to the brush: both the drift
  // sensor and the auto-sync bail out on it. The assembler takes any 0302 frame
  // without the 23 24 prefix as a continuation, so a truncated or corrupted one
  // reaching parse_device_settings is a real wire case, not a hypothesis.
  uint8_t buf[SETTINGS_BUFFER_SIZE] = {};
  DeviceSettings ds;
  // All zeroes: month 0 and day 0 are outside the calendar.
  parse_device_settings(buf, &ds);
  TEST_ASSERT_FALSE(ds.clock_valid);
  // A plausible date except for the hour, which the device never sends as 24.
  buf[16] = 26;  // year - 2000
  buf[17] = 6;
  buf[18] = 4;
  buf[19] = 24;
  buf[20] = 0;
  buf[21] = 0;
  parse_device_settings(buf, &ds);
  TEST_ASSERT_FALSE(ds.clock_valid);
  // Same buffer with an hour the device can actually report.
  buf[19] = 23;
  parse_device_settings(buf, &ds);
  TEST_ASSERT_TRUE(ds.clock_valid);
}

void test_settings_continuation_brush_b() {
  // Real continuation frame, Brush B: over-pressure OFF, area-reminder ON,
  // head used 173 days / 213 sessions, clock 2026-06-04 00:20:10.
  const uint8_t cont[] = {0x03, 0x02, 0x1A, 0x06, 0x04, 0x00, 0x14, 0x0A, 0x00, 0x01, 0x0F,
                          0x00, 0xF0, 0x00, 0xAD, 0x00, 0xD5, 0x03, 0x00, 0x00, 0x00};
  SettingsAssembler asm_;
  asm_.reset();
  asm_.feed(cont, sizeof(cont));
  TEST_ASSERT_FALSE(asm_.complete());
  TEST_ASSERT_FALSE(asm_.has_start());
  // Reading continuation-region fields off a partial (start-missing) buffer is
  // intentional: only the continuation half is exercised here, the start half
  // stays zeroed and is not asserted.
  DeviceSettings ds;
  parse_device_settings(asm_.buffer(), &ds);
  TEST_ASSERT_EQUAL_UINT8(20, ds.minute);
  TEST_ASSERT_EQUAL_UINT8(10, ds.second);
  TEST_ASSERT_FALSE(ds.over_pressure);
  TEST_ASSERT_TRUE(ds.area_reminder);
  TEST_ASSERT_EQUAL_UINT16(173, ds.head_used_days);
  TEST_ASSERT_EQUAL_UINT16(213, ds.head_used_times);
  // Continuation-region read-parity fields.
  TEST_ASSERT_EQUAL_UINT8(15, ds.tz_index);
  TEST_ASSERT_EQUAL_UINT16(240, ds.head_max);
  TEST_ASSERT_EQUAL_UINT8(3, ds.device_language);
}

void test_settings_two_frame_complete() {
  // Start frame (03 02 23 24 + 16 payload bytes) places pNum at buffer[11], i.e.
  // start payload index 11 = wire byte 15. Set it to 72 and confirm readback.
  uint8_t start[20] = {0x03, 0x02, 0x23, 0x24};
  start[4 + 11] = 72;  // buffer[11] = active scheme pNum
  const uint8_t cont[] = {0x03, 0x02, 0x1A, 0x06, 0x04, 0x00, 0x0C, 0x22, 0x01, 0x01, 0x0F,
                          0x00, 0xF0, 0x04, 0x1B, 0x04, 0xC7, 0x0C, 0x00, 0x00, 0x00};
  SettingsAssembler asm_;
  asm_.reset();
  TEST_ASSERT_FALSE(asm_.feed(start, sizeof(start)));  // start alone is not complete
  TEST_ASSERT_TRUE(asm_.has_start());
  TEST_ASSERT_FALSE(asm_.complete());               // one frame: not yet complete
  TEST_ASSERT_TRUE(asm_.feed(cont, sizeof(cont)));  // both frames -> complete
  TEST_ASSERT_TRUE(asm_.complete());
  DeviceSettings ds;
  parse_device_settings(asm_.buffer(), &ds);
  TEST_ASSERT_EQUAL_UINT8(72, ds.scheme_pnum);
  TEST_ASSERT_TRUE(ds.over_pressure);  // continuation region still intact
}

void test_settings_short_frame_rejected() {
  const uint8_t shortf[] = {0x03, 0x02, 0x1A, 0x06};
  SettingsAssembler asm_;
  asm_.reset();
  TEST_ASSERT_FALSE(asm_.feed(shortf, sizeof(shortf)));
  TEST_ASSERT_FALSE(asm_.has_cont());
  TEST_ASSERT_FALSE(asm_.has_start());
}

void test_settings_start_fields_brush_b() {
  // Real start frame, Brush B: volume off, calendar on, scheme 0, head used time
  // 393. The toggle decode matches Brush A; the distinguishing values are the
  // active scheme (0) and head used time.
  const uint8_t start[] = {0x03, 0x02, 0x23, 0x24, 0x0C, 0x01, 0x01, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x89};
  SettingsAssembler asm_;
  asm_.reset();
  TEST_ASSERT_FALSE(asm_.feed(start, sizeof(start)));  // start alone is not complete
  TEST_ASSERT_TRUE(asm_.has_start());
  DeviceSettings ds;
  parse_device_settings(asm_.buffer(), &ds);
  TEST_ASSERT_EQUAL_UINT8(12, ds.device_theme);
  TEST_ASSERT_TRUE(ds.brush_pause);
  TEST_ASSERT_TRUE(ds.raise_wake);
  TEST_ASSERT_FALSE(ds.fill_brush);
  TEST_ASSERT_FALSE(ds.auto_mode);
  TEST_ASSERT_FALSE(ds.volume_enabled);  // buffer 8 = 0x0C, not zero -> off
  TEST_ASSERT_EQUAL_UINT8(1, ds.volume_index);
  TEST_ASSERT_TRUE(ds.calendar_enabled);  // buffer 10 = 0 -> on
  TEST_ASSERT_EQUAL_UINT8(0, ds.scheme_pnum);
  TEST_ASSERT_TRUE(ds.brush_mode_on);  // buffer 12 = 0x00, not 0xEC -> on
  TEST_ASSERT_FALSE(ds.splash_prevent);
  TEST_ASSERT_EQUAL_UINT16(393, ds.head_used_time);
}

void test_settings_two_frame_full_brush_a() {
  // Real two-frame settings, Brush A (after setting Quick Cleaning, scheme 88).
  // Pins every read-parity field including the inverted volume/calendar toggles,
  // the 0xEC brush-mode sentinel, the timezone index (16, not the assumed 15),
  // and the head-max readback (90, matching the 0217 write).
  const uint8_t start[] = {0x03, 0x02, 0x23, 0x24, 0x0C, 0x01, 0x01, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x0C, 0x01, 0x00, 0x58, 0x00, 0x00, 0x00, 0x02};
  const uint8_t cont[] = {0x03, 0x02, 0x1A, 0x06, 0x05, 0x09, 0x03, 0x0C, 0x01, 0x01, 0x10,
                          0x00, 0x5A, 0x00, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00};
  SettingsAssembler asm_;
  asm_.reset();
  asm_.feed(start, sizeof(start));
  TEST_ASSERT_TRUE(asm_.feed(cont, sizeof(cont)));
  TEST_ASSERT_TRUE(asm_.complete());
  DeviceSettings ds;
  parse_device_settings(asm_.buffer(), &ds);
  // Start region.
  TEST_ASSERT_EQUAL_UINT8(12, ds.device_theme);
  TEST_ASSERT_TRUE(ds.brush_pause);
  TEST_ASSERT_TRUE(ds.raise_wake);
  TEST_ASSERT_FALSE(ds.fill_brush);
  TEST_ASSERT_FALSE(ds.auto_mode);
  TEST_ASSERT_FALSE(ds.volume_enabled);
  TEST_ASSERT_EQUAL_UINT8(1, ds.volume_index);
  TEST_ASSERT_TRUE(ds.calendar_enabled);
  TEST_ASSERT_EQUAL_UINT8(88, ds.scheme_pnum);
  TEST_ASSERT_TRUE(ds.brush_mode_on);
  TEST_ASSERT_FALSE(ds.splash_prevent);
  TEST_ASSERT_EQUAL_UINT16(2, ds.head_used_time);
  // Continuation region.
  TEST_ASSERT_TRUE(ds.clock_valid);
  TEST_ASSERT_EQUAL_UINT16(2026, ds.year);
  TEST_ASSERT_EQUAL_UINT8(6, ds.month);
  TEST_ASSERT_EQUAL_UINT8(5, ds.day);
  TEST_ASSERT_EQUAL_UINT8(9, ds.hour);
  TEST_ASSERT_EQUAL_UINT8(3, ds.minute);
  TEST_ASSERT_EQUAL_UINT8(12, ds.second);
  TEST_ASSERT_TRUE(ds.over_pressure);
  TEST_ASSERT_TRUE(ds.area_reminder);
  TEST_ASSERT_EQUAL_UINT8(16, ds.tz_index);
  TEST_ASSERT_EQUAL_UINT16(90, ds.head_max);
  TEST_ASSERT_EQUAL_UINT16(1, ds.head_used_days);
  TEST_ASSERT_EQUAL_UINT16(1, ds.head_used_times);
  TEST_ASSERT_EQUAL_UINT8(3, ds.device_language);
}

// === decode_brush_areas_push (enrichment push 021f / 2604) ===

void test_brush_areas_push_y3p_valid() {
  // Synthetic fixture: there is no real Y3P capture of this push. The firmware
  // never emits a brush-areas push (021f) on Y3P, so the decoder is exercised
  // here purely against a hand-built frame matching the documented layout.
  // 02 1f 00 00 0f 00 0f 21 [11 23 01 0d 12 0f 01 0f] -> values at bytes 8-15.
  const uint8_t p[] = {0x02, 0x1F, 0x00, 0x00, 0x0F, 0x00, 0x0F, 0x21, 0x11,
                       0x23, 0x01, 0x0D, 0x12, 0x0F, 0x01, 0x0F, 0x12, 0x00};
  BrushAreasPush out;
  TEST_ASSERT_TRUE(decode_brush_areas_push(p, sizeof(p), &out));
  const uint8_t expected[] = {0x11, 0x23, 0x01, 0x0D, 0x12, 0x0F, 0x01, 0x0F};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out.values, 8);
}

void test_brush_areas_push_t1_prefix() {
  const uint8_t p[] = {0x26, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  BrushAreasPush out;
  TEST_ASSERT_TRUE(decode_brush_areas_push(p, sizeof(p), &out));
  TEST_ASSERT_EQUAL_UINT8(0x01, out.values[0]);
  TEST_ASSERT_EQUAL_UINT8(0x08, out.values[7]);
}

void test_brush_areas_push_bad_prefix_rejected() {
  const uint8_t p[] = {0x03, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  BrushAreasPush out;
  TEST_ASSERT_FALSE(decode_brush_areas_push(p, sizeof(p), &out));
}

void test_brush_areas_push_short_rejected() {
  // Correct prefix but too short to carry all 8 values (needs >= 16 bytes).
  const uint8_t p[] = {0x02, 0x1F, 0x00, 0x00, 0x0F, 0x00, 0x0F, 0x21, 0x11};
  BrushAreasPush out;
  TEST_ASSERT_FALSE(decode_brush_areas_push(p, sizeof(p), &out));
}

void test_raw_record_matches_input() {
  // Single-record stream: header then 42 record bytes; raw_record(0) must point
  // at the unmodified record bytes.
  uint8_t rec[SESSION_RECORD_SIZE];
  for (size_t i = 0; i < SESSION_RECORD_SIZE; i++)
    rec[i] = uint8_t(i + 1);
  uint8_t stream[SESSION_HEADER_LEN + SESSION_RECORD_SIZE];
  const uint8_t header[SESSION_HEADER_LEN] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x01};
  for (size_t i = 0; i < SESSION_HEADER_LEN; i++)
    stream[i] = header[i];
  for (size_t i = 0; i < SESSION_RECORD_SIZE; i++)
    stream[SESSION_HEADER_LEN + i] = rec[i];

  SessionAssembler asm_;
  asm_.reset();
  TEST_ASSERT_TRUE(asm_.feed(stream, sizeof(stream)));
  const uint8_t *raw = asm_.raw_record(0);
  TEST_ASSERT_NOT_NULL(raw);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rec, raw, SESSION_RECORD_SIZE);
  TEST_ASSERT_NULL(asm_.raw_record(1));
}

// === Brush-scheme builder ===

void test_encode_scheme_gear_table() {
  TEST_ASSERT_EQUAL_UINT8(5, encode_scheme_gear(1));
  TEST_ASSERT_EQUAL_UINT8(8, encode_scheme_gear(4));
  TEST_ASSERT_EQUAL_UINT8(17, encode_scheme_gear(5));
  TEST_ASSERT_EQUAL_UINT8(24, encode_scheme_gear(12));
  // Gears outside 1-12 (whitening / massage / extended) encode as 0.
  TEST_ASSERT_EQUAL_UINT8(0, encode_scheme_gear(0));
  TEST_ASSERT_EQUAL_UINT8(0, encode_scheme_gear(16));
  TEST_ASSERT_EQUAL_UINT8(0, encode_scheme_gear(37));
  TEST_ASSERT_EQUAL_UINT8(0, encode_scheme_gear(41));
}

void test_build_scheme_single_frame() {
  // pNum 2, steps (g2,30)(g2,30)(g3,60): fits one write.
  std::vector<SchemeStep> steps = {{2, 30}, {2, 30}, {3, 60}};
  auto packets = build_scheme_packets(2, steps);
  TEST_ASSERT_EQUAL_UINT(1, packets.size());
  const uint8_t expected[] = {0x02, 0x06, 0x02, 0x03, 0x06, 0x02, 0x1E, 0x06, 0x02, 0x1E, 0x07, 0x03, 0x3C, 0x00, 0x05};
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), packets[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, packets[0].data(), sizeof(expected));
}

void test_build_scheme_split_frames() {
  // pNum 72, six steps: 24-byte program exceeds 20, so it splits across two
  // writes. All six gears are above 12, so each encodes as 0.
  std::vector<SchemeStep> steps = {{16, 30}, {16, 30}, {24, 30}, {16, 30}, {16, 30}, {37, 30}};
  auto packets = build_scheme_packets(0x48, steps);
  TEST_ASSERT_EQUAL_UINT(2, packets.size());

  const uint8_t pkt1[] = {0x02, 0x06, 0x48, 0x06, 0x00, 0x10, 0x1E, 0x00, 0x10,
                          0x1E, 0x00, 0x18, 0x1E, 0x00, 0x10, 0x1E, 0x2A, 0x2B};
  TEST_ASSERT_EQUAL_UINT(sizeof(pkt1), packets[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pkt1, packets[0].data(), sizeof(pkt1));

  const uint8_t pkt2[] = {0x02, 0x0B, 0x00, 0x10, 0x1E, 0x00, 0x25, 0x1E, 0x00, 0x05};
  TEST_ASSERT_EQUAL_UINT(sizeof(pkt2), packets[1].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pkt2, packets[1].data(), sizeof(pkt2));
}

void test_build_scheme_split_shortest_second_frame() {
  // Five steps is 21 payload bytes: the first size that splits, and the one that
  // leaves the shortest possible continuation frame. A six-step program would
  // hide an off-by-one in the 16-byte slice; this one does not.
  std::vector<SchemeStep> steps = {{2, 30}, {2, 30}, {2, 30}, {2, 30}, {2, 30}};
  auto packets = build_scheme_packets(0x50, steps);
  TEST_ASSERT_EQUAL_UINT(2, packets.size());

  const uint8_t pkt1[] = {0x02, 0x06, 0x50, 0x05, 0x06, 0x02, 0x1E, 0x06, 0x02,
                          0x1E, 0x06, 0x02, 0x1E, 0x06, 0x02, 0x1E, 0x2A, 0x2B};
  TEST_ASSERT_EQUAL_UINT(sizeof(pkt1), packets[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pkt1, packets[0].data(), sizeof(pkt1));

  const uint8_t pkt2[] = {0x02, 0x0B, 0x06, 0x02, 0x1E, 0x00, 0x05};
  TEST_ASSERT_EQUAL_UINT(sizeof(pkt2), packets[1].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pkt2, packets[1].data(), sizeof(pkt2));
}

void test_build_scheme_rejects_bad_step_count() {
  // Empty and over-8-step programs are rejected, returning no packets.
  TEST_ASSERT_EQUAL_UINT(0, build_scheme_packets(2, {}).size());
  std::vector<SchemeStep> nine(9, {2, 30});
  TEST_ASSERT_EQUAL_UINT(0, build_scheme_packets(2, nine).size());
}

// === Config-toggle write encoding ===

void test_build_toggle_default_on_off() {
  // brush-pause (0222) uses the default on 0x01 / off 0x00.
  auto on = build_toggle_command(0x02, 0x22, 0x01, 0x00, true);
  const uint8_t on_expected[] = {0x02, 0x22, 0x01};
  TEST_ASSERT_EQUAL_UINT(sizeof(on_expected), on.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(on_expected, on.data(), sizeof(on_expected));
  auto off = build_toggle_command(0x02, 0x22, 0x01, 0x00, false);
  const uint8_t off_expected[] = {0x02, 0x22, 0x00};
  TEST_ASSERT_EQUAL_UINT(sizeof(off_expected), off.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(off_expected, off.data(), sizeof(off_expected));
}

void test_build_toggle_brush_mode_off_sentinel() {
  // brush-mode (0209) uses a non-zero off sentinel: off is 0xEC, on is 0x01.
  auto off = build_toggle_command(0x02, 0x09, 0x01, 0xEC, false);
  const uint8_t off_expected[] = {0x02, 0x09, 0xEC};
  TEST_ASSERT_EQUAL_UINT(sizeof(off_expected), off.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(off_expected, off.data(), sizeof(off_expected));
  auto on = build_toggle_command(0x02, 0x09, 0x01, 0xEC, true);
  const uint8_t on_expected[] = {0x02, 0x09, 0x01};
  TEST_ASSERT_EQUAL_UINT(sizeof(on_expected), on.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(on_expected, on.data(), sizeof(on_expected));
}

void test_build_language_command() {
  // Polish is id 12; English is id 3. Wire layout: 02 16 <langId>.
  auto pl = build_language_command(12);
  const uint8_t pl_expected[] = {0x02, 0x16, 0x0C};
  TEST_ASSERT_EQUAL_UINT(sizeof(pl_expected), pl.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pl_expected, pl.data(), sizeof(pl_expected));
  auto en = build_language_command(3);
  const uint8_t en_expected[] = {0x02, 0x16, 0x03};
  TEST_ASSERT_EQUAL_UINT(sizeof(en_expected), en.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(en_expected, en.data(), sizeof(en_expected));
}

void test_build_set_cloud_host_command() {
  const std::string url = "http://10.0.0.5:8099";
  auto cmd = build_set_cloud_host_command(url);
  TEST_ASSERT_EQUAL_UINT(5 + url.size(), cmd.size());
  TEST_ASSERT_EQUAL_UINT8(0x02, cmd[0]);
  TEST_ASSERT_EQUAL_UINT8(0x33, cmd[1]);
  TEST_ASSERT_EQUAL_UINT8(0x2A, cmd[2]);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(url.size()), cmd[3]);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(url.size()), cmd[4]);
  auto *url_bytes = reinterpret_cast<const uint8_t *>(url.data());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(url_bytes, cmd.data() + 5, url.size());
}

void test_build_set_cloud_host_command_empty() {
  // empty url writes the fallback trigger
  auto cmd = build_set_cloud_host_command("");
  const uint8_t expected[] = {0x02, 0x33, 0x2A, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), cmd.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cmd.data(), sizeof(expected));
}

void test_build_set_cloud_host_command_clamps_over_limit() {
  // the one-byte length and the firmware's 59-byte store cap a longer url
  const std::string url(80, 'a');
  auto cmd = build_set_cloud_host_command(url);
  TEST_ASSERT_EQUAL_UINT(5 + CLOUD_HOST_MAX_LEN, cmd.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CLOUD_HOST_MAX_LEN), cmd[3]);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CLOUD_HOST_MAX_LEN), cmd[4]);
}

void test_build_blufi_frame_control() {
  // control frame, no data: [type_byte][fc=0][seq][len=0]
  auto sec = build_blufi_frame(BLUFI_TYPE_CTRL, BLUFI_CTRL_SET_SEC_MODE, nullptr, 0, 0);
  const uint8_t sec_expected[] = {0x04, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT(sizeof(sec_expected), sec.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(sec_expected, sec.data(), sizeof(sec_expected));
  auto status = build_blufi_frame(BLUFI_TYPE_CTRL, BLUFI_CTRL_GET_STATUS, nullptr, 0, 5);
  const uint8_t status_expected[] = {0x14, 0x00, 0x05, 0x00};
  TEST_ASSERT_EQUAL_UINT(sizeof(status_expected), status.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(status_expected, status.data(), sizeof(status_expected));
}

void test_build_blufi_frame_data() {
  // ssid data frame: type DATA, subtype 2, seq 2, the ascii ssid after the header
  const uint8_t ssid[] = {'e', 's', 'p'};
  auto f = build_blufi_frame(BLUFI_TYPE_DATA, BLUFI_DATA_SSID, ssid, sizeof(ssid), 2);
  const uint8_t expected[] = {0x09, 0x00, 0x02, 0x03, 'e', 's', 'p'};
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), f.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, f.data(), sizeof(expected));
}

void test_parse_blufi_wifi_status() {
  uint8_t opmode = 0xFF;
  uint8_t sta = 0xFF;
  // data subtype 15, opmode 1, sta_state 0 (connected)
  const uint8_t report[] = {0x3D, 0x00, 0x00, 0x02, 0x01, 0x00};
  TEST_ASSERT_TRUE(parse_blufi_wifi_status(report, sizeof(report), &opmode, &sta));
  TEST_ASSERT_EQUAL_UINT8(1, opmode);
  TEST_ASSERT_EQUAL_UINT8(0, sta);
  // a control frame is not a status report
  const uint8_t ctrl[] = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_FALSE(parse_blufi_wifi_status(ctrl, sizeof(ctrl), &opmode, &sta));
  // a data frame of another subtype (ssid) is not a status report
  const uint8_t ssid[] = {0x09, 0x00, 0x02, 0x03, 0x65, 0x73};
  TEST_ASSERT_FALSE(parse_blufi_wifi_status(ssid, sizeof(ssid), &opmode, &sta));
  const uint8_t short_frame[] = {0x3D, 0x00, 0x00, 0x02};
  TEST_ASSERT_FALSE(parse_blufi_wifi_status(short_frame, sizeof(short_frame), &opmode, &sta));
}

void test_cloud_body_field() {
  // flat JSON: pull string values, including a mac whose value holds colons
  const std::string body =
      "{\"mac\":\"aa:bb:cc:dd:ee:01\",\"model\":\"OCLEANV20\",\"brushdata\":\"2a4201\",\"security\":\"0011\"}";
  std::string mac;
  std::string data;
  std::string model;
  TEST_ASSERT_TRUE(cloud_body_field(body.data(), body.size(), "mac", &mac));
  TEST_ASSERT_EQUAL_STRING("aa:bb:cc:dd:ee:01", mac.c_str());
  TEST_ASSERT_TRUE(cloud_body_field(body.data(), body.size(), "brushdata", &data));
  TEST_ASSERT_EQUAL_STRING("2a4201", data.c_str());
  TEST_ASSERT_TRUE(cloud_body_field(body.data(), body.size(), "model", &model));
  TEST_ASSERT_EQUAL_STRING("OCLEANV20", model.c_str());
  std::string miss;
  TEST_ASSERT_FALSE(cloud_body_field(body.data(), body.size(), "absent", &miss));
  // whitespace around the colon and value is tolerated
  const std::string spaced = "{ \"brushdata\" : \"ab\" }";
  std::string spaced_val;
  TEST_ASSERT_TRUE(cloud_body_field(spaced.data(), spaced.size(), "brushdata", &spaced_val));
  TEST_ASSERT_EQUAL_STRING("ab", spaced_val.c_str());
  const std::string num = "{\"n\":123}";
  std::string num_val;
  TEST_ASSERT_FALSE(cloud_body_field(num.data(), num.size(), "n", &num_val));
  // the name showing up first as another key's value does not hide the key
  const std::string decoy = "{\"model\":\"mac\",\"mac\":\"aa:bb:cc:dd:ee:01\"}";
  std::string decoy_val;
  TEST_ASSERT_TRUE(cloud_body_field(decoy.data(), decoy.size(), "mac", &decoy_val));
  TEST_ASSERT_EQUAL_STRING("aa:bb:cc:dd:ee:01", decoy_val.c_str());
}

void test_parse_hex_bytes() {
  std::vector<uint8_t> out;
  TEST_ASSERT_TRUE(parse_hex_bytes("2a4201ff", &out));
  const uint8_t expected[] = {0x2A, 0x42, 0x01, 0xFF};
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), out.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out.data(), sizeof(expected));
  TEST_ASSERT_TRUE(parse_hex_bytes("AB", &out));
  TEST_ASSERT_EQUAL_UINT8(0xAB, out[0]);
  TEST_ASSERT_FALSE(parse_hex_bytes("abc", &out));
  TEST_ASSERT_FALSE(parse_hex_bytes("zz", &out));
}

void test_parse_mac_u64() {
  uint64_t v = 0;
  TEST_ASSERT_TRUE(parse_mac_u64("aa:bb:cc:dd:ee:01", &v));
  TEST_ASSERT_EQUAL_UINT64(0xAABBCCDDEE01ULL, v);
  // the firmware's trailing-colon "%02x:" form resolves to the same address
  TEST_ASSERT_TRUE(parse_mac_u64("aa:bb:cc:dd:ee:01:", &v));
  TEST_ASSERT_EQUAL_UINT64(0xAABBCCDDEE01ULL, v);
  TEST_ASSERT_FALSE(parse_mac_u64("aa:bb:cc:dd:ee", &v));
  TEST_ASSERT_FALSE(parse_mac_u64("zz:bb:cc:dd:ee:01", &v));
}

void test_build_birthday_command() {
  // gender, age, then month and day
  const uint8_t mar7[] = {0x02, 0x11, 0x02, 0x09, 0x03, 0x07};
  std::vector<uint8_t> const cmd = build_birthday_command(2, 9, 3, 7);
  TEST_ASSERT_EQUAL_UINT(sizeof(mar7), cmd.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(mar7, cmd.data(), sizeof(mar7));
  // unset: a date that never matches, with the defaults (unknown, adult)
  const uint8_t unset[] = {0x02, 0x11, 0x00, 0x12, 0xFF, 0xFF};
  std::vector<uint8_t> const clear =
      build_birthday_command(USER_GENDER_DEFAULT, USER_AGE_DEFAULT, BIRTHDAY_UNSET, BIRTHDAY_UNSET);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(unset, clear.data(), sizeof(unset));
  // the app's clamps: an age of 0xFF would skip the date, a gender past 2 is 1
  std::vector<uint8_t> const clamped = build_birthday_command(7, 0xFF, 3, 7);
  TEST_ASSERT_EQUAL_UINT8(1, clamped[2]);
  TEST_ASSERT_EQUAL_UINT8(18, clamped[3]);
  TEST_ASSERT_EQUAL_UINT8(3, build_birthday_command(0, 1, 3, 7)[3]);
}

void test_sync_fingerprint() {
  const uint8_t a[] = {0x02, 0x11, 0x00, 0x12, 0x03, 0x07};
  const uint8_t b[] = {0x02, 0x11, 0x00, 0x12, 0x03, 0x08};
  uint64_t const mac = 0xAABBCCDDEEFFULL;
  uint32_t const fp = sync_fingerprint(mac, SyncSlot::BIRTHDAY, a, sizeof(a));
  TEST_ASSERT_NOT_EQUAL(0, fp);
  TEST_ASSERT_EQUAL_UINT32(fp, sync_fingerprint(mac, SyncSlot::BIRTHDAY, a, sizeof(a)));
  // another value, slot or brush has to be written again
  TEST_ASSERT_NOT_EQUAL(fp, sync_fingerprint(mac, SyncSlot::BIRTHDAY, b, sizeof(b)));
  TEST_ASSERT_NOT_EQUAL(fp, sync_fingerprint(mac, SyncSlot::CLOUD_HOST, a, sizeof(a)));
  TEST_ASSERT_NOT_EQUAL(fp, sync_fingerprint(mac + 1, SyncSlot::BIRTHDAY, a, sizeof(a)));
  // pinned: a stored fingerprint has to match after a rebuild
  TEST_ASSERT_EQUAL_UINT32(0x462A7C27U, sync_fingerprint(0, SyncSlot::BIRTHDAY, nullptr, 0));
}

void test_cloud_host_matches() {
  // the brush leaves the port out of the Host header for 80
  TEST_ASSERT_TRUE(cloud_host_matches("192.0.2.10", "http://192.0.2.10:80"));
  TEST_ASSERT_TRUE(cloud_host_matches("192.0.2.10:80", "http://192.0.2.10:80"));
  TEST_ASSERT_TRUE(cloud_host_matches("192.0.2.10:8099", "http://192.0.2.10:8099"));
  TEST_ASSERT_FALSE(cloud_host_matches("192.0.2.10", "http://192.0.2.10:8099"));
  TEST_ASSERT_FALSE(cloud_host_matches("192.0.2.11", "http://192.0.2.10:80"));
  TEST_ASSERT_FALSE(cloud_host_matches("", "http://192.0.2.10:80"));
  TEST_ASSERT_FALSE(cloud_host_matches("192.0.2.10", "192.0.2.10:80"));
}

void test_brush_weather_code() {
  // every Home Assistant condition lands on one of the brush's seven icons
  TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(brush_weather_code("sunny")));
  TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(brush_weather_code("clear-night")));
  TEST_ASSERT_EQUAL_UINT8(4, static_cast<uint8_t>(brush_weather_code("partlycloudy")));
  TEST_ASSERT_EQUAL_UINT8(4, static_cast<uint8_t>(brush_weather_code("cloudy")));
  TEST_ASSERT_EQUAL_UINT8(4, static_cast<uint8_t>(brush_weather_code("fog")));
  TEST_ASSERT_EQUAL_UINT8(5, static_cast<uint8_t>(brush_weather_code("rainy")));
  TEST_ASSERT_EQUAL_UINT8(5, static_cast<uint8_t>(brush_weather_code("pouring")));
  TEST_ASSERT_EQUAL_UINT8(6, static_cast<uint8_t>(brush_weather_code("lightning")));
  TEST_ASSERT_EQUAL_UINT8(6, static_cast<uint8_t>(brush_weather_code("lightning-rainy")));
  TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(brush_weather_code("snowy")));
  TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(brush_weather_code("snowy-rainy")));
  TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(brush_weather_code("hail")));
  TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(brush_weather_code("windy")));
  TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(brush_weather_code("windy-variant")));
  TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(brush_weather_code("exceptional")));
  TEST_ASSERT_TRUE(brush_weather_code("unavailable") == BrushWeather::NONE);
  TEST_ASSERT_TRUE(brush_weather_code("") == BrushWeather::NONE);
  TEST_ASSERT_TRUE(brush_weather_code("Sunny") == BrushWeather::NONE);
}

void test_parse_iso8601_epoch() {
  int64_t const noon = civil_to_epoch(2026, 10, 8, 12, 0, 0);
  int64_t v = 0;
  TEST_ASSERT_TRUE(parse_iso8601_epoch("2026-10-08T12:00:00+00:00", &v));
  TEST_ASSERT_EQUAL_INT64(noon, v);
  TEST_ASSERT_TRUE(parse_iso8601_epoch("2026-10-08T12:00:00Z", &v));
  TEST_ASSERT_EQUAL_INT64(noon, v);
  TEST_ASSERT_TRUE(parse_iso8601_epoch("2026-10-08T14:00:00+02:00", &v));
  TEST_ASSERT_EQUAL_INT64(noon, v);
  TEST_ASSERT_TRUE(parse_iso8601_epoch("2026-10-08T07:00:00-0500", &v));
  TEST_ASSERT_EQUAL_INT64(noon, v);
  TEST_ASSERT_TRUE(parse_iso8601_epoch("2026-10-08T12:00:00.250000+00:00", &v));
  TEST_ASSERT_EQUAL_INT64(noon, v);
  // no seconds and no offset: UTC
  TEST_ASSERT_TRUE(parse_iso8601_epoch("2026-10-08 12:00", &v));
  TEST_ASSERT_EQUAL_INT64(noon, v);
  TEST_ASSERT_FALSE(parse_iso8601_epoch("2026-10-08", &v));
  TEST_ASSERT_FALSE(parse_iso8601_epoch("2026-13-08T12:00:00Z", &v));
  TEST_ASSERT_FALSE(parse_iso8601_epoch("2026-10-08T12:00:00X", &v));
  TEST_ASSERT_FALSE(parse_iso8601_epoch("not a date at all", &v));
}

static WeatherSnapshot two_day_forecast(int32_t today) {
  WeatherSnapshot s;
  s.current_code = BrushWeather::RAIN;
  s.current_temp = 11.4F;
  s.days[0] = WeatherDay{.local_day = today, .code = BrushWeather::CLOUDY, .high = 15.6F, .low = 7.2F};
  s.days[1] = WeatherDay{.local_day = today + 1, .code = BrushWeather::SUNNY, .high = 18.0F, .low = -2.5F};
  s.day_count = 2;
  return s;
}

void test_pick_weather_today_then_tomorrow() {
  int32_t const today = 20734;
  WeatherSnapshot const s = two_day_forecast(today);
  WeatherPick morning = pick_weather(s, today, 7);
  TEST_ASSERT_TRUE(morning.valid);
  TEST_ASSERT_FALSE(morning.tomorrow);
  TEST_ASSERT_TRUE(morning.code == BrushWeather::CLOUDY);
  TEST_ASSERT_EQUAL_INT(16, morning.high);
  TEST_ASSERT_EQUAL_INT(7, morning.low);
  // the evening reply carries tomorrow, rounded half away from zero
  WeatherPick evening = pick_weather(s, today, WEATHER_TOMORROW_FROM_HOUR);
  TEST_ASSERT_TRUE(evening.tomorrow);
  TEST_ASSERT_TRUE(evening.code == BrushWeather::SUNNY);
  TEST_ASSERT_EQUAL_INT(18, evening.high);
  TEST_ASSERT_EQUAL_INT(-3, evening.low);
  // a forecast from yesterday: its second day is today
  WeatherPick stale = pick_weather(s, today + 1, 9);
  TEST_ASSERT_TRUE(stale.valid);
  TEST_ASSERT_FALSE(stale.tomorrow);
  TEST_ASSERT_TRUE(stale.code == BrushWeather::SUNNY);
}

void test_pick_weather_falls_back() {
  int32_t const today = 20734;
  WeatherSnapshot s = two_day_forecast(today);
  // only tomorrow left (a provider that already dropped today): shown as tomorrow
  s.days[0] = s.days[1];
  s.day_count = 1;
  WeatherPick only_tomorrow = pick_weather(s, today, 9);
  TEST_ASSERT_TRUE(only_tomorrow.tomorrow);
  // no forecast day matches: the current condition, its temperature both bounds
  WeatherPick current = pick_weather(s, today + 5, 9);
  TEST_ASSERT_TRUE(current.valid);
  TEST_ASSERT_FALSE(current.tomorrow);
  TEST_ASSERT_TRUE(current.code == BrushWeather::RAIN);
  TEST_ASSERT_EQUAL_INT(11, current.high);
  TEST_ASSERT_EQUAL_INT(11, current.low);
  // nothing to draw without a current temperature
  s.current_temp = NAN;
  TEST_ASSERT_FALSE(pick_weather(s, today + 5, 9).valid);
  // two digits at most
  WeatherSnapshot hot;
  hot.current_code = BrushWeather::SUNNY;
  hot.current_temp = 123.0F;
  TEST_ASSERT_EQUAL_INT(99, pick_weather(hot, 0, 12).high);
  hot.current_temp = -140.0F;
  TEST_ASSERT_EQUAL_INT(-99, pick_weather(hot, 0, 12).low);
}

void test_build_weather_reply() {
  WeatherPick pick;
  pick.valid = true;
  pick.code = BrushWeather::STORM;
  pick.tomorrow = true;
  pick.high = 21;
  pick.low = -4;
  TEST_ASSERT_EQUAL_STRING(R"({"state":true,"data":{"conditionCode":"6","todayOrTomorrow":"1",)"
                           R"("temperatureMax":"21","temperatureMin":"-4","presentTime":"07:05"}})",
                           build_weather_reply(pick, "07:05").c_str());
  // presentTime only when it fits the firmware's 8 bytes and is plain digits
  TEST_ASSERT_EQUAL_STRING(R"({"state":true,"data":{"conditionCode":"6","todayOrTomorrow":"1",)"
                           R"("temperatureMax":"21","temperatureMin":"-4"}})",
                           build_weather_reply(pick, "07:05:30:1").c_str());
  TEST_ASSERT_EQUAL_STRING(R"({"state":true,"data":{"conditionCode":"6","todayOrTomorrow":"1",)"
                           R"("temperatureMax":"21","temperatureMin":"-4"}})",
                           build_weather_reply(pick, "7\"5").c_str());
  TEST_ASSERT_EQUAL_STRING(R"({"state":false})", build_weather_reply(WeatherPick{}, "07:05").c_str());
}

void test_decode_cloud_brushdata_record() {
  // the full v20 record the cloud body carries, length prefix included: the
  // score at [28] is the datum BLE on this firmware never hands over
  std::vector<uint8_t> rec(SESSION_V20_RECORD_MIN, 0);
  rec[1] = static_cast<uint8_t>(SESSION_V20_RECORD_MIN);
  // 2026-10-07 21:41:24, scheme 3, program 120 s, brushed 118 s
  const uint8_t head[] = {26, 10, 7, 21, 41, 24, 3, 0, 120, 0, 118};
  for (size_t i = 0; i < sizeof(head); i++)
    rec[2 + i] = head[i];
  rec[SESSION_V20_TZ_OFFSET] = 16;
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    rec[SESSION_V20_ZONES_OFFSET + i] = static_cast<uint8_t>(10 + i);
  rec[SESSION_V20_SCORE_OFFSET] = 88;
  std::string hex;
  for (uint8_t b : rec) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", b);
    hex += buf;
  }
  std::vector<uint8_t> bytes;
  TEST_ASSERT_TRUE(parse_hex_bytes(hex, &bytes));
  SessionRecord out{};
  TEST_ASSERT_TRUE(decode_session_record_v20(bytes.data(), bytes.size(), &out));
  TEST_ASSERT_EQUAL_UINT16(2026, out.year);
  TEST_ASSERT_EQUAL_UINT8(10, out.month);
  TEST_ASSERT_EQUAL_UINT8(7, out.day);
  TEST_ASSERT_EQUAL_UINT16(120, out.duration_s);
  TEST_ASSERT_EQUAL_UINT16(118, out.valid_duration_s);
  TEST_ASSERT_EQUAL_UINT8(16, out.tz_index);
  TEST_ASSERT_TRUE(out.has_score);
  TEST_ASSERT_EQUAL_UINT8(88, out.score);
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    TEST_ASSERT_EQUAL_UINT8(10 + i, out.zones[i]);
}

void test_decode_v20_record_zones_real() {
  // real cloud UploadBrushRecord captured 2026-10-08 (score 65); gestureArray at
  // [20..27] = 04 0e 01 05 03 37 06 13. The inline-only BLE path never carries it.
  const std::string hex =
      "006f40041115223503007800780a144600000010040e0105033706134103000000050100"
      "ffffffffffffffffffffffffffffff717139ff150a201c3225292245262b156623452d34"
      "474c432e2b192f2c641c2856273f393b232614";
  std::vector<uint8_t> bytes;
  TEST_ASSERT_TRUE(parse_hex_bytes(hex, &bytes));
  SessionRecord out{};
  TEST_ASSERT_TRUE(decode_session_record_v20(bytes.data(), bytes.size(), &out));
  TEST_ASSERT_EQUAL_UINT8(65, out.score);
  const uint8_t expected_zones[] = {0x04, 0x0e, 0x01, 0x05, 0x03, 0x37, 0x06, 0x13};
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    TEST_ASSERT_EQUAL_UINT8(expected_zones[i], out.zones[i]);
}

void test_timezone_index_to_string() {
  // Wire index is 1-based into the 33-entry GMT table.
  TEST_ASSERT_EQUAL_STRING("GMT-12:00", timezone_index_to_string(1));
  TEST_ASSERT_EQUAL_STRING("GMT+01:00", timezone_index_to_string(15));
  TEST_ASSERT_EQUAL_STRING("GMT+02:00", timezone_index_to_string(16));
  TEST_ASSERT_EQUAL_STRING("GMT+13:00", timezone_index_to_string(33));
  TEST_ASSERT_EQUAL_STRING("unknown", timezone_index_to_string(0));
  TEST_ASSERT_EQUAL_STRING("unknown", timezone_index_to_string(34));
}

void test_tz_index_for_offset_seconds() {
  // Exact match returns the 1-based wire index.
  TEST_ASSERT_EQUAL_UINT8(15, tz_index_for_offset_seconds(3600));
  TEST_ASSERT_EQUAL_UINT8(16, tz_index_for_offset_seconds(7200));
  TEST_ASSERT_EQUAL_UINT8(14, tz_index_for_offset_seconds(0));
  TEST_ASSERT_EQUAL_UINT8(1, tz_index_for_offset_seconds(-43200));
  TEST_ASSERT_EQUAL_UINT8(33, tz_index_for_offset_seconds(46800));
  // Non-whole-hour table entries.
  TEST_ASSERT_EQUAL_UINT8(10, tz_index_for_offset_seconds(-12600));
  TEST_ASSERT_EQUAL_UINT8(22, tz_index_for_offset_seconds(19800));
  TEST_ASSERT_EQUAL_UINT8(23, tz_index_for_offset_seconds(20700));
  // No table entry returns 0.
  TEST_ASSERT_EQUAL_UINT8(0, tz_index_for_offset_seconds(5400));
  TEST_ASSERT_EQUAL_UINT8(0, tz_index_for_offset_seconds(-1));
}

void test_tz_index_offset_seconds_round_trips() {
  for (uint8_t idx = 1; idx <= 33; idx++) {
    int32_t offset = 0;
    TEST_ASSERT_TRUE(tz_index_offset_seconds(idx, &offset));
    TEST_ASSERT_EQUAL_UINT8(idx, tz_index_for_offset_seconds(offset));
  }
  int32_t offset = 1;
  TEST_ASSERT_FALSE(tz_index_offset_seconds(0, &offset));
  TEST_ASSERT_FALSE(tz_index_offset_seconds(34, &offset));
  TEST_ASSERT_FALSE(tz_index_offset_seconds(15, nullptr));
  TEST_ASSERT_EQUAL_INT32(1, offset);
}

void test_session_utc_offset_prefers_the_record_zone() {
  // recorded in CEST (16), read after the switch to CET: the record still says +2
  SessionRecord r{};
  r.tz_index = 16;
  TEST_ASSERT_EQUAL_INT64(7200, session_utc_offset_seconds(r, 3600));
  // the X Ultra 20 factory clock: UTC+8 (27)
  r.tz_index = 27;
  TEST_ASSERT_EQUAL_INT64(28800, session_utc_offset_seconds(r, 7200));
  // no zone in the record (inline head, or a record from before it was read)
  r.tz_index = 0;
  TEST_ASSERT_EQUAL_INT64(3600, session_utc_offset_seconds(r, 3600));
  r.tz_index = 0xFF;
  TEST_ASSERT_EQUAL_INT64(-18000, session_utc_offset_seconds(r, -18000));
}

void test_max_language_id_per_model() {
  const char *y3pd = "OCLEANY3PD";
  const char *y3p = "OCLEANY3P";
  const char *v20 = "OCLEANV20B";
  TEST_ASSERT_EQUAL_UINT8(13, max_language_id(y3pd, strlen(y3pd)));
  TEST_ASSERT_EQUAL_UINT8(14, max_language_id(y3p, strlen(y3p)));
  TEST_ASSERT_EQUAL_UINT8(0, max_language_id(v20, strlen(v20)));
  // DIS not read yet
  TEST_ASSERT_EQUAL_UINT8(0, max_language_id("", 0));
  TEST_ASSERT_EQUAL_UINT8(0, max_language_id(nullptr, 0));
  // a truncated string must not match the longer prefix
  TEST_ASSERT_EQUAL_UINT8(14, max_language_id(y3pd, strlen(y3p)));
}

void test_is_write_refusal() {
  const uint8_t er[] = {0x02, 0x06, 0x45, 0x52};
  const uint8_t ok[] = {0x02, 0x06, 0x4F, 0x4B};
  const uint8_t read_er[] = {0x03, 0x06, 0x45, 0x52};
  const uint8_t longer[] = {0x02, 0x12, 0x45, 0x52, 0x00};
  TEST_ASSERT_TRUE(is_write_refusal(er, sizeof(er)));
  TEST_ASSERT_FALSE(is_write_refusal(ok, sizeof(ok)));
  TEST_ASSERT_FALSE(is_write_refusal(read_er, sizeof(read_er)));
  TEST_ASSERT_FALSE(is_write_refusal(longer, sizeof(longer)));
  TEST_ASSERT_FALSE(is_write_refusal(er, 3));
  TEST_ASSERT_FALSE(is_write_refusal(nullptr, 4));
}

// === Set-clock builder (0201) ===

void test_build_set_clock_summer() {
  // 2026-06-05 16:37:14, Friday (weekday 5), tz_index 15 (UTC+1). Every field is
  // decimal, not BCD: minute 37 -> 0x25, second 14 -> 0x0E.
  auto cmd = build_set_clock_command(2026, 6, 5, 16, 37, 14, 5, 15);
  const uint8_t expected[] = {0x02, 0x01, 0x1A, 0x06, 0x05, 0x10, 0x25, 0x0E, 0x05, 0x0F};
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), cmd.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cmd.data(), sizeof(expected));
}

void test_build_set_clock_winter_sunday_midnight_fields() {
  // 2026-01-04 09:03:00, Sunday (weekday 0), tz_index 14 (UTC+0). Exercises the
  // zero weekday and a zero second so no field is mistaken for BCD.
  auto cmd = build_set_clock_command(2026, 1, 4, 9, 3, 0, 0, 14);
  const uint8_t expected[] = {0x02, 0x01, 0x1A, 0x01, 0x04, 0x09, 0x03, 0x00, 0x00, 0x0E};
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), cmd.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cmd.data(), sizeof(expected));
}

void test_build_set_clock_before_2000_clamps_year_byte() {
  // Defensive branch: an unsynced clock (1970) must not wrap the year byte into
  // a plausible-looking future date on the brush.
  auto cmd = build_set_clock_command(1970, 1, 1, 0, 0, 0, 4, 15);
  TEST_ASSERT_EQUAL_UINT(10, cmd.size());
  TEST_ASSERT_EQUAL_UINT8(0x00, cmd[2]);
}

// === Clock-drift decision (auto clock-sync) ===

void test_civil_to_epoch_matches_record_epoch() {
  // The shared civil helper must produce the same value the record epoch pins.
  // 2026-06-05 16:37:14 UTC -> 1780677434.
  TEST_ASSERT_EQUAL_INT64(1780677434LL, civil_to_epoch(2026, 6, 5, 16, 37, 14));
}

void test_civil_to_epoch_january_february_branch() {
  // days-from-civil shifts January and February into the previous March-based
  // year, a branch no other test reaches (they all use May or June). An error
  // there is a whole year of drift on winter sessions, which would poison the
  // dedup watermark for good.
  TEST_ASSERT_EQUAL_INT64(1767225600LL, civil_to_epoch(2026, 1, 1, 0, 0, 0));
  TEST_ASSERT_EQUAL_INT64(1772323199LL, civil_to_epoch(2026, 2, 28, 23, 59, 59));
  // first day past the branch boundary
  TEST_ASSERT_EQUAL_INT64(1772323200LL, civil_to_epoch(2026, 3, 1, 0, 0, 0));
}

void test_civil_to_epoch_leap_day() {
  // 29 February only exists on the leap path through the same branch.
  TEST_ASSERT_EQUAL_INT64(1709208000LL, civil_to_epoch(2024, 2, 29, 12, 0, 0));
  // record year floor: byte 0 of a record is year-2000
  TEST_ASSERT_EQUAL_INT64(946684800LL, civil_to_epoch(2000, 1, 1, 0, 0, 0));
}

void test_should_resync_below_threshold() {
  // Brush 30 s behind local, threshold 120 s: no resync.
  int64_t local = civil_to_epoch(2026, 6, 6, 12, 0, 0);
  int64_t brush = civil_to_epoch(2026, 6, 6, 11, 59, 30);
  TEST_ASSERT_FALSE(should_resync_clock(brush, local, 120));
}

void test_should_resync_above_threshold_either_sign() {
  // 64 minutes of drift in either direction exceeds the 120 s threshold.
  int64_t local = civil_to_epoch(2026, 6, 6, 14, 39, 0);
  int64_t behind = civil_to_epoch(2026, 6, 6, 13, 35, 0);  // brush slow
  int64_t ahead = civil_to_epoch(2026, 6, 6, 15, 43, 0);   // brush fast
  TEST_ASSERT_TRUE(should_resync_clock(behind, local, 120));
  TEST_ASSERT_TRUE(should_resync_clock(ahead, local, 120));
}

void test_should_resync_exact_threshold_not_exceeded() {
  // Drift exactly equal to the threshold does not trigger (strictly greater).
  int64_t local = civil_to_epoch(2026, 6, 6, 12, 0, 0);
  int64_t brush = civil_to_epoch(2026, 6, 6, 12, 2, 0);  // 120 s
  TEST_ASSERT_FALSE(should_resync_clock(brush, local, 120));
}

void test_should_resync_zero_threshold_syncs_on_any_diff() {
  // A zero threshold resyncs whenever the clocks differ by at least one second,
  // but never when they are identical.
  int64_t local = civil_to_epoch(2026, 6, 6, 12, 0, 0);
  TEST_ASSERT_TRUE(should_resync_clock(local + 1, local, 0));
  TEST_ASSERT_FALSE(should_resync_clock(local, local, 0));
}

// === Hold-the-link decision ===

void test_should_hold_link_requires_every_condition() {
  // Full truth table: the link is only held with the option on, BLE enabled, the
  // brush docked, and this round having actually seen a STATUS reply. Any one of
  // them missing means disconnect, so a silent brush cannot pin a BLE slot.
  for (int mask = 0; mask < 16; mask++) {
    bool option = (mask & 1) != 0;
    bool ble = (mask & 2) != 0;
    bool docked = (mask & 4) != 0;
    bool status = (mask & 8) != 0;
    bool expected = option && ble && docked && status;
    TEST_ASSERT_EQUAL(expected, should_hold_link(option, ble, docked, status));
  }
}

// === Poll tick decision ===

static PollTickState ready_tick_state() {
  // A hub past the boot window, off dock, with the slow interval elapsed.
  PollTickState s{};
  s.now_ms = 20000000;
  s.last_poll_ms = 20000000 - 14400000;
  s.boot_stagger_ms = 90000;
  s.charging_interval_ms = 600000;
  s.battery_interval_ms = 14400000;
  s.ble_enabled = true;
  s.link_busy = false;
  s.poll_pending = false;
  s.adaptive = true;
  s.docked = false;
  s.boot_stagger_done = true;
  return s;
}

void test_plan_poll_tick_polls_when_due() {
  TEST_ASSERT_EQUAL(PollAction::POLL, plan_poll_tick(ready_tick_state()).action);
}

void test_plan_poll_tick_skip_reasons_are_ordered() {
  // BLE off outranks everything, then a busy link: neither may be masked by the
  // cadence gate, or a disabled hub would still open connections.
  PollTickState s = ready_tick_state();
  s.ble_enabled = false;
  s.link_busy = true;
  TEST_ASSERT_EQUAL(PollAction::SKIP_BLE_OFF, plan_poll_tick(s).action);
  s.ble_enabled = true;
  TEST_ASSERT_EQUAL(PollAction::SKIP_LINK_BUSY, plan_poll_tick(s).action);
}

void test_plan_poll_tick_off_dock_waits_for_slow_interval() {
  PollTickState s = ready_tick_state();
  s.last_poll_ms = s.now_ms - 600000;  // docked cadence would already be due
  TEST_ASSERT_EQUAL(PollAction::SKIP_NOT_DUE, plan_poll_tick(s).action);
  s.docked = true;
  TEST_ASSERT_EQUAL(PollAction::POLL, plan_poll_tick(s).action);
}

void test_plan_poll_tick_boot_stagger_defers_once() {
  // Second hub inside its boot window: defer by the remainder.
  PollTickState s = ready_tick_state();
  s.now_ms = 20000;
  s.poll_pending = true;
  s.boot_stagger_done = false;
  PollDecision d = plan_poll_tick(s);
  TEST_ASSERT_EQUAL(PollAction::DEFER_BOOT_STAGGER, d.action);
  TEST_ASSERT_EQUAL_UINT32(70000, d.defer_ms);
  // Latched by the caller: the same tick must then poll, so a millis() wrap
  // cannot re-defer a hub that is already running.
  s.boot_stagger_done = true;
  TEST_ASSERT_EQUAL(PollAction::POLL, plan_poll_tick(s).action);
}

void test_plan_poll_tick_pending_poll_bypasses_cadence() {
  // A pending poll (BLE switched back on, poll-now, or a failed connect) lifts
  // the off-dock gate for one cycle.
  PollTickState s = ready_tick_state();
  s.last_poll_ms = s.now_ms;  // just polled
  TEST_ASSERT_EQUAL(PollAction::SKIP_NOT_DUE, plan_poll_tick(s).action);
  s.poll_pending = true;
  TEST_ASSERT_EQUAL(PollAction::POLL, plan_poll_tick(s).action);
}

// === Session ring ingest plan ===

static SessionRecord ingest_record(uint8_t day, uint8_t hour, uint8_t score) {
  SessionRecord r{};
  r.year = 2026;
  r.month = 6;
  r.day = day;
  r.hour = hour;
  r.scheme = 0;
  r.duration_s = 120;
  r.valid_duration_s = 120;
  r.score = score;
  r.has_score = true;
  return r;
}

static SessionClocks node_only(int64_t now) {
  return SessionClocks{.node_local = now, .brush = 0};
}

void test_plan_ingest_sorts_new_records_oldest_first() {
  // The ring is unordered; the live state has to settle on the newest session,
  // so the plan hands them back oldest-first with the newest last.
  std::vector<SessionRecord> ring = {ingest_record(6, 18, 99), ingest_record(5, 8, 70), ingest_record(6, 7, 83)};
  int64_t now = civil_to_epoch(2026, 6, 7, 12, 0, 0);
  SessionIngestPlan plan = plan_session_ingest(ring, 0, 0, node_only(now));
  TEST_ASSERT_EQUAL_UINT(3, plan.to_publish.size());
  TEST_ASSERT_EQUAL_UINT8(5, plan.to_publish[0].day);
  TEST_ASSERT_EQUAL_UINT8(7, plan.to_publish[1].hour);
  TEST_ASSERT_EQUAL_UINT8(18, plan.to_publish[2].hour);
  TEST_ASSERT_EQUAL_UINT32(session_record_epoch(ring[0]), plan.new_watermark);
}

void test_plan_ingest_skips_at_or_below_watermark() {
  // Already-delivered sessions must not fire again after a reboot.
  std::vector<SessionRecord> ring = {ingest_record(5, 8, 70), ingest_record(6, 7, 83)};
  int64_t now = civil_to_epoch(2026, 6, 7, 12, 0, 0);
  uint32_t wm = session_record_epoch(ring[0]);
  SessionIngestPlan plan = plan_session_ingest(ring, wm, 0, node_only(now));
  TEST_ASSERT_EQUAL_UINT(1, plan.to_publish.size());
  TEST_ASSERT_EQUAL_UINT8(6, plan.to_publish[0].day);
}

void test_plan_ingest_future_record_never_moves_watermark() {
  // A bogus future date would push the watermark past every real session and
  // mute them for good, so it is dropped without advancing anything.
  std::vector<SessionRecord> ring = {ingest_record(6, 7, 83)};
  ring[0].year = 2099;
  int64_t now = civil_to_epoch(2026, 6, 7, 12, 0, 0);
  SessionIngestPlan plan = plan_session_ingest(ring, 100, 0, node_only(now));
  TEST_ASSERT_EQUAL_UINT(0, plan.to_publish.size());
  TEST_ASSERT_EQUAL_UINT(1, plan.implausible.size());
  TEST_ASSERT_EQUAL_UINT32(100, plan.new_watermark);
  TEST_ASSERT_FALSE(plan.persist_newest);
  TEST_ASSERT_FALSE(plan.newest_plausible);
}

void test_plan_ingest_persists_only_strictly_newer() {
  // A peer re-serving the same ring on every poll would otherwise grind flash.
  std::vector<SessionRecord> ring = {ingest_record(6, 7, 83)};
  int64_t now = civil_to_epoch(2026, 6, 7, 12, 0, 0);
  uint32_t epoch = session_record_epoch(ring[0]);
  SessionIngestPlan first = plan_session_ingest(ring, 0, 0, node_only(now));
  TEST_ASSERT_TRUE(first.persist_newest);
  TEST_ASSERT_EQUAL_UINT32(epoch, first.new_newest_epoch);
  SessionIngestPlan again = plan_session_ingest(ring, epoch, epoch, node_only(now));
  TEST_ASSERT_FALSE(again.persist_newest);
  TEST_ASSERT_EQUAL_UINT(0, again.to_publish.size());
  // Nothing new, but the newest is still sound: the caller republishes it to
  // keep the live state right without adding a history row.
  TEST_ASSERT_TRUE(again.newest_plausible);
}

void test_plan_ingest_unsynced_clock_cannot_judge() {
  // 0 means the node clock is unsynced, which must not reject real sessions.
  std::vector<SessionRecord> ring = {ingest_record(6, 7, 83)};
  SessionIngestPlan plan = plan_session_ingest(ring, 0, 0, node_only(0));
  TEST_ASSERT_EQUAL_UINT(1, plan.to_publish.size());
  TEST_ASSERT_TRUE(plan.persist_newest);
}

void test_plan_ingest_picks_newest_itself() {
  // The newest record is found here, not passed in as an index: an index from
  // the reassembler would address a different record than this vector as soon as
  // one entry failed to decode.
  std::vector<SessionRecord> ring = {ingest_record(5, 8, 70), ingest_record(6, 18, 99), ingest_record(6, 7, 83)};
  int64_t now = civil_to_epoch(2026, 6, 7, 12, 0, 0);
  SessionIngestPlan plan = plan_session_ingest(ring, 0, 0, node_only(now));
  TEST_ASSERT_TRUE(plan.have_newest);
  TEST_ASSERT_EQUAL_UINT8(6, plan.newest.day);
  TEST_ASSERT_EQUAL_UINT8(18, plan.newest.hour);
  TEST_ASSERT_EQUAL_UINT8(99, plan.newest.score);
}

void test_confirm_sessions_only_with_nothing_held_back() {
  std::vector<SessionRecord> ring = {ingest_record(6, 7, 83)};
  int64_t const now = civil_to_epoch(2026, 6, 7, 12, 0, 0);
  SessionIngestPlan plan = plan_session_ingest(ring, 0, 0, node_only(now));
  TEST_ASSERT_TRUE(should_confirm_sessions(true, plan));
  TEST_ASSERT_FALSE(should_confirm_sessions(false, plan));
  // already emitted: the batch is still the brush's to drop
  plan = plan_session_ingest(ring, session_record_epoch(ring[0]), 0, node_only(now));
  TEST_ASSERT_TRUE(should_confirm_sessions(true, plan));
  // a record too far ahead stays unread on the brush until it is plausible
  ring.push_back(ingest_record(6, 9, 90));
  ring[1].year = 2099;
  plan = plan_session_ingest(ring, 0, 0, node_only(now));
  TEST_ASSERT_EQUAL_UINT(1, plan.implausible.size());
  TEST_ASSERT_FALSE(should_confirm_sessions(true, plan));
}

void test_plan_ingest_empty_ring_is_inert() {
  SessionIngestPlan plan = plan_session_ingest({}, 42, 7, node_only(0));
  TEST_ASSERT_EQUAL_UINT(0, plan.to_publish.size());
  TEST_ASSERT_EQUAL_UINT32(42, plan.new_watermark);
  TEST_ASSERT_EQUAL_UINT32(7, plan.new_newest_epoch);
  TEST_ASSERT_FALSE(plan.persist_newest);
  TEST_ASSERT_FALSE(plan.newest_plausible);
  TEST_ASSERT_FALSE(plan.have_newest);
}

// === Inline (count=0) record precedence ===

void test_accept_inline_only_when_strictly_newer() {
  // The inline fragment has no zones and no score, so publishing it blanks both.
  // A brush at rest sends one on every poll, which is why "not older" is not
  // enough: it has to be strictly newer than what the entities already show.
  SessionClocks const clocks = node_only(civil_to_epoch(2026, 6, 7, 12, 0, 0));
  SessionRecord const inl = ingest_record(7, 7, 0);
  uint32_t const epoch = session_record_epoch(inl);
  TEST_ASSERT_FALSE(accept_inline_record(inl, epoch, nullptr, clocks));
  TEST_ASSERT_FALSE(accept_inline_record(inl, epoch + 1, nullptr, clocks));
  TEST_ASSERT_TRUE(accept_inline_record(inl, epoch - 1, nullptr, clocks));
}

void test_accept_inline_rejects_implausible_future() {
  SessionRecord far = ingest_record(1, 0, 0);
  far.year = 2030;
  TEST_ASSERT_FALSE(accept_inline_record(far, 0, nullptr, node_only(civil_to_epoch(2026, 6, 7, 12, 0, 0))));
  // An unsynced node clock cannot judge, so it must not reject the only session
  // data available.
  TEST_ASSERT_TRUE(accept_inline_record(far, 0, nullptr, node_only(0)));
}

void test_accept_inline_never_republishes_the_shown_session() {
  // A clock set back 6 h moves the gate back with it, while the head the brush
  // keeps sending still carries its old, later date.
  SessionRecord const shown = ingest_record(7, 20, 91);
  uint32_t const moved = session_record_epoch(shown) - (6 * 3600);
  SessionRecord inl = shown;
  inl.valid_duration_s = 0;
  inl.has_score = false;
  SessionClocks const clocks = node_only(civil_to_epoch(2026, 6, 8, 12, 0, 0));
  TEST_ASSERT_FALSE(accept_inline_record(inl, moved, &shown, clocks));
  inl.second = 1;
  TEST_ASSERT_TRUE(accept_inline_record(inl, moved, &shown, clocks));
}

void test_plan_ingest_judges_by_brush_clock() {
  // A brush 6 h ahead dates its ring by its own clock, so the ring goes out
  // whole. Once the clock is set back, the same dates predate the set.
  std::vector<SessionRecord> ring = {ingest_record(7, 14, 80), ingest_record(7, 17, 90)};
  int64_t const now = civil_to_epoch(2026, 6, 7, 12, 0, 0);
  SessionIngestPlan plan = plan_session_ingest(ring, 0, 0, SessionClocks{.node_local = now, .brush = now + (6 * 3600)});
  TEST_ASSERT_EQUAL_UINT(2, plan.to_publish.size());
  plan = plan_session_ingest(ring, 0, 0, SessionClocks{.node_local = now, .brush = now});
  TEST_ASSERT_EQUAL_UINT(0, plan.to_publish.size());
  TEST_ASSERT_EQUAL_UINT(2, plan.implausible.size());
  TEST_ASSERT_EQUAL_UINT32(0, plan.new_watermark);
}

void test_same_session_ignores_fields_a_head_lacks() {
  SessionRecord full = ingest_record(7, 20, 91);
  full.valid_duration_s = 110;
  SessionRecord head = full;
  head.valid_duration_s = 0;
  head.has_score = false;
  head.score = SESSION_NO_SCORE;
  TEST_ASSERT_TRUE(same_session(full, head));
  head.duration_s = 180;
  TEST_ASSERT_FALSE(same_session(full, head));
  head = full;
  head.minute = 1;
  TEST_ASSERT_FALSE(same_session(full, head));
}

void test_clock_set_shift_and_confirmation() {
  // 2026-10-05: brush read 03:46:31 on its UTC+8 clock, set 2 s later to 21:45:14
  int64_t const shift =
      clock_set_shift(civil_to_epoch(2026, 10, 6, 3, 46, 31), 2000, civil_to_epoch(2026, 10, 5, 21, 45, 14));
  TEST_ASSERT_EQUAL_INT64((6 * 3600) + 79, shift);
  TEST_ASSERT_TRUE(clock_shift_applies(shift));
  TEST_ASSERT_TRUE(clock_set_confirmed(shift, 1));
  TEST_ASSERT_TRUE(clock_set_confirmed(shift, -1));
  TEST_ASSERT_FALSE(clock_set_confirmed(shift, shift));
  TEST_ASSERT_FALSE(clock_set_confirmed(0, 0));
  TEST_ASSERT_FALSE(clock_shift_applies(-3600));
  TEST_ASSERT_FALSE(clock_shift_applies((int64_t)SESSION_FUTURE_MARGIN_S + 1));
}

void test_shift_epoch_back() {
  TEST_ASSERT_EQUAL_UINT32(1000000 - 3600, shift_epoch_back(1000000, 3600));
  TEST_ASSERT_EQUAL_UINT32(1000000, shift_epoch_back(1000000, -3600));
  TEST_ASSERT_EQUAL_UINT32(1000000, shift_epoch_back(1000000, (int64_t)SESSION_FUTURE_MARGIN_S + 1));
  TEST_ASSERT_EQUAL_UINT32(0, shift_epoch_back(0, 3600));
}

void test_restored_newest_epoch() {
  TEST_ASSERT_EQUAL_UINT32(500, restored_newest_epoch(500, 0));
  TEST_ASSERT_EQUAL_UINT32(500, restored_newest_epoch(500, 900));
  // watermark moved back below the stored record by a clock set
  TEST_ASSERT_EQUAL_UINT32(300, restored_newest_epoch(500, 300));
}

void test_set_clock_command_epoch() {
  // written to the X Ultra 20 on 2026-10-05 22:04:22
  const uint8_t cmd[] = {0x02, 0x01, 0x1A, 0x0A, 0x05, 0x16, 0x04, 0x16, 0x01, 0x10};
  TEST_ASSERT_EQUAL_INT64(civil_to_epoch(2026, 10, 5, 22, 4, 22), set_clock_command_epoch(cmd, sizeof(cmd)));
  TEST_ASSERT_EQUAL_INT64(0, set_clock_command_epoch(cmd, sizeof(cmd) - 1));
  TEST_ASSERT_EQUAL_INT64(0, set_clock_command_epoch(nullptr, sizeof(cmd)));
  const uint8_t other[] = {0x02, 0x02, 0x1A, 0x0A, 0x05, 0x16, 0x04, 0x16, 0x01, 0x10};
  TEST_ASSERT_EQUAL_INT64(0, set_clock_command_epoch(other, sizeof(other)));
}

void test_settings_query_per_profile() {
  const ProfileCmd *q = settings_query(PROFILE_TYPE_V20);
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQUAL_UINT8(3, q->len);
  TEST_ASSERT_EQUAL_HEX8(0x01, q->bytes[2]);
  TEST_ASSERT_NOT_NULL(settings_query(PROFILE_TYPE1));
  TEST_ASSERT_NULL(settings_query(PROFILE_UNKNOWN));
}

// === Coverage percent ===

void test_coverage_percent_is_whole_and_bounded() {
  // Rounded at the source: the raw quotient reaches the recorder and the card
  // verbatim, and 100*83/120 in float32 renders as 69.1666641235352.
  TEST_ASSERT_EQUAL_FLOAT(69.0f, session_coverage_percent(83, 120));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, session_coverage_percent(120, 120));
  // valid > duration comes only from a malformed record; clamp instead of
  // reporting over 100 percent.
  TEST_ASSERT_EQUAL_FLOAT(100.0f, session_coverage_percent(200, 120));
}

void test_coverage_percent_zero_duration_is_nan() {
  // No duration means no ratio, not a division by zero.
  TEST_ASSERT_TRUE(std::isnan(session_coverage_percent(0, 0)));
  TEST_ASSERT_TRUE(std::isnan(session_coverage_percent(50, 0)));
}

// === Charging-aware adaptive poll cadence ===

void test_poll_is_due_charging_uses_fast_interval() {
  // On the dock the fast (charging) interval applies: not due before it, due at
  // or after it, regardless of the longer battery interval.
  const uint32_t charging_ms = 600000;   // 10 min
  const uint32_t battery_ms = 14400000;  // 4 h
  TEST_ASSERT_FALSE(poll_is_due(599999, true, charging_ms, battery_ms));
  TEST_ASSERT_TRUE(poll_is_due(600000, true, charging_ms, battery_ms));
  TEST_ASSERT_TRUE(poll_is_due(700000, true, charging_ms, battery_ms));
}

void test_poll_is_due_off_dock_uses_slow_interval() {
  // Off the dock the slow (battery) interval applies: a gap that would already
  // be due on the dock is still skipped, until the battery interval elapses.
  const uint32_t charging_ms = 600000;
  const uint32_t battery_ms = 14400000;
  TEST_ASSERT_FALSE(poll_is_due(600000, false, charging_ms, battery_ms));
  TEST_ASSERT_FALSE(poll_is_due(14399999, false, charging_ms, battery_ms));
  TEST_ASSERT_TRUE(poll_is_due(14400000, false, charging_ms, battery_ms));
}

// === Runtime profile selection ===

void test_profile_select_y3p() {
  const char *m = "OCLEANY3P";
  const OcleanProfile *p = profile_for_model(m, strlen(m));
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE1, p);
  TEST_ASSERT_EQUAL_UINT8(2, p->confidence);
}

void test_profile_select_y3pd() {
  // OCLEANY3PD starts with OCLEANY3P, so it matches the validated Y3P entry.
  const char *m = "OCLEANY3PD";
  const OcleanProfile *p = profile_for_model(m, strlen(m));
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE1, p);
  TEST_ASSERT_EQUAL_UINT8(2, p->confidence);
}

void test_profile_select_prefix_order() {
  // The shorter OCLEANY3 prefix must not shadow the longer OCLEANY3P: both are
  // TYPE1, but OCLEANY3 (no P) maps via the generic entry while OCLEANY3P maps
  // via the validated entry. Longest-first ordering is what makes this hold.
  const char *generic = "OCLEANY3";
  const OcleanProfile *pg = profile_for_model(generic, strlen(generic));
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE1, pg);
  const char *elite = "OCLEANY3P";
  const OcleanProfile *pe = profile_for_model(elite, strlen(elite));
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE1, pe);
  TEST_ASSERT_EQUAL_UINT8(2, pe->confidence);
}

void test_profile_select_unknown_fallback() {
  // An unrecognised model, an empty string and a null pointer all fall back to
  // UNKNOWN without crashing or returning null.
  const char *k1 = "OCLEANK1";
  TEST_ASSERT_EQUAL_PTR(&PROFILE_UNKNOWN, profile_for_model(k1, strlen(k1)));
  const char *empty = "";
  TEST_ASSERT_EQUAL_PTR(&PROFILE_UNKNOWN, profile_for_model(empty, 0));
  TEST_ASSERT_EQUAL_PTR(&PROFILE_UNKNOWN, profile_for_model(nullptr, 0));
}

void test_profile_select_wifi_oos() {
  // The WiFi model is out of BLE scope and maps to UNKNOWN.
  const char *c1 = "OCLEANC1";
  TEST_ASSERT_EQUAL_PTR(&PROFILE_UNKNOWN, profile_for_model(c1, strlen(c1)));
}

void test_profile_select_legacy_after_specific() {
  // OCLEANA1e / OCLEANA1f are TYPE1; the bare OCLEANA1 legacy model is UNKNOWN.
  // The specific A1e / A1f prefixes must win over the shorter A1 entry.
  const char *a1e = "OCLEANA1e";
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE1, profile_for_model(a1e, strlen(a1e)));
  const char *a1 = "OCLEANA1";
  TEST_ASSERT_EQUAL_PTR(&PROFILE_UNKNOWN, profile_for_model(a1, strlen(a1)));
}

void test_type1_routing() {
  // The TYPE1 query sequence is status and settings on the main tx char, then
  // the session download on the session tx char, with the exact command bytes.
  TEST_ASSERT_EQUAL_UINT8(3, PROFILE_TYPE1.query_cmd_count);

  const ProfileCmd &status = PROFILE_TYPE1.query_cmds[0];
  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, status.target);
  TEST_ASSERT_EQUAL_UINT8(2, status.len);
  const uint8_t status_bytes[] = {0x03, 0x03};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(status_bytes, status.bytes, 2);

  const ProfileCmd &settings = PROFILE_TYPE1.query_cmds[1];
  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, settings.target);
  TEST_ASSERT_EQUAL_UINT8(3, settings.len);
  const uint8_t settings_bytes[] = {0x03, 0x02, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(settings_bytes, settings.bytes, 3);

  const ProfileCmd &download = PROFILE_TYPE1.query_cmds[2];
  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_SESSION, download.target);
  TEST_ASSERT_EQUAL_UINT8(2, download.len);
  const uint8_t download_bytes[] = {0x03, 0x07};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(download_bytes, download.bytes, 2);

  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, PROFILE_TYPE1.config_write_target);
  TEST_ASSERT_EQUAL_INT(SettingsKind::SETTINGS_TYPE1_34B, PROFILE_TYPE1.settings_kind);
}

void test_record_decode_type1_known() {
  // The TYPE1 record decoder is the shared decode_session_record, reached through
  // the profile function pointer. Fed a known Y3P record it yields the same
  // fields the direct decode test pins, with gesture zones from offset 23.
  TEST_ASSERT_NOT_NULL(PROFILE_TYPE1.decode_record);
  SessionRecord r;
  TEST_ASSERT_TRUE(PROFILE_TYPE1.decode_record(fixtures::SESSION_REC_NORMAL, &r));
  TEST_ASSERT_EQUAL_UINT16(2026, r.year);
  TEST_ASSERT_EQUAL_UINT16(120, r.duration_s);
  TEST_ASSERT_TRUE(r.has_score);
  TEST_ASSERT_EQUAL_UINT8(82, r.score);
  const uint8_t zones[SESSION_ZONES_COUNT] = {10, 21, 11, 2, 14, 22, 12, 3};
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    TEST_ASSERT_EQUAL_UINT8(zones[i], r.zones[i]);
}

void test_record_decode_unknown_is_null() {
  // UNKNOWN has no per-record session decode and no session record size.
  TEST_ASSERT_NULL(PROFILE_UNKNOWN.decode_record);
  TEST_ASSERT_EQUAL_INT(SettingsKind::SETTINGS_NONE, PROFILE_UNKNOWN.settings_kind);
}

void test_profile_unknown_contract() {
  // UNKNOWN reads battery from the standard characteristic but has no session
  // decode, no record size and no settings framing. The hub relies on the null
  // decoder to skip the session reassembler for an unrecognised model.
  TEST_ASSERT_NULL(PROFILE_UNKNOWN.decode_record);
  TEST_ASSERT_EQUAL_INT(SettingsKind::SETTINGS_NONE, PROFILE_UNKNOWN.settings_kind);
  TEST_ASSERT_EQUAL_UINT8(1, PROFILE_UNKNOWN.confidence);
}

void test_profile_unknown_query_status_only() {
  // The only query UNKNOWN sends is status on the main tx char; everything else
  // is model-specific and could mis-decode on an unrecognised device.
  TEST_ASSERT_EQUAL_UINT8(1, PROFILE_UNKNOWN.query_cmd_count);
  const ProfileCmd &status = PROFILE_UNKNOWN.query_cmds[0];
  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, status.target);
  TEST_ASSERT_EQUAL_UINT8(2, status.len);
  const uint8_t status_bytes[] = {0x03, 0x03};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(status_bytes, status.bytes, 2);
}

void test_profile_type1_contract() {
  // TYPE1 is the hardware-validated profile: it decodes session records through
  // the shared decoder, frames settings as the 34-byte buffer and reads battery
  // from the standard characteristic.
  TEST_ASSERT_NOT_NULL(PROFILE_TYPE1.decode_record);
  TEST_ASSERT_EQUAL_INT(SettingsKind::SETTINGS_TYPE1_34B, PROFILE_TYPE1.settings_kind);
  TEST_ASSERT_EQUAL_UINT8(2, PROFILE_TYPE1.confidence);
}

void test_profile_select_y5_z1() {
  // The Z1 model maps to its own TYPE_Z1 profile, not the UNKNOWN fallback.
  const char *m = "OCLEANY5";
  const OcleanProfile *p = profile_for_model(m, strlen(m));
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE_Z1, p);
  TEST_ASSERT_EQUAL_UINT8(1, p->confidence);
}

void test_profile_z1_contract() {
  // Z1 reuses the TYPE1 record decoder, record size and settings framing; it is
  // a ported profile (confidence 1) until an OCLEANY5 capture confirms it.
  const OcleanProfile *p = &PROFILE_TYPE_Z1;
  TEST_ASSERT_NOT_NULL(p->decode_record);
  TEST_ASSERT_EQUAL_INT(SettingsKind::SETTINGS_TYPE1_34B, p->settings_kind);
  TEST_ASSERT_EQUAL_UINT8(1, p->confidence);
  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, p->config_write_target);
}

void test_profile_z1_routing() {
  // Z1 reuses the TYPE1 query sequence: status and settings on the main tx char,
  // then the session download on the session tx char, with the exact bytes.
  TEST_ASSERT_EQUAL_UINT8(3, PROFILE_TYPE_Z1.query_cmd_count);

  const ProfileCmd &status = PROFILE_TYPE_Z1.query_cmds[0];
  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, status.target);
  TEST_ASSERT_EQUAL_UINT8(2, status.len);
  const uint8_t status_bytes[] = {0x03, 0x03};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(status_bytes, status.bytes, 2);

  const ProfileCmd &settings = PROFILE_TYPE_Z1.query_cmds[1];
  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, settings.target);
  TEST_ASSERT_EQUAL_UINT8(3, settings.len);
  const uint8_t settings_bytes[] = {0x03, 0x02, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(settings_bytes, settings.bytes, 3);

  const ProfileCmd &download = PROFILE_TYPE_Z1.query_cmds[2];
  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_SESSION, download.target);
  TEST_ASSERT_EQUAL_UINT8(2, download.len);
  const uint8_t download_bytes[] = {0x03, 0x07};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(download_bytes, download.bytes, 2);

  TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, PROFILE_TYPE_Z1.config_write_target);
  TEST_ASSERT_EQUAL_INT(SettingsKind::SETTINGS_TYPE1_34B, PROFILE_TYPE_Z1.settings_kind);
}

void test_profile_select_v20() {
  const char *m = "OCLEANV20B";
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE_V20, profile_for_model(m, strlen(m)));
  // X Pro 20 and the first X Ultra answer like the X Ultra 20, not like TYPE1
  const char *v1 = "OCLEANV1a";
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE_V20_FAMILY, profile_for_model(v1, strlen(v1)));
  const char *x20 = "OCLEANX20";
  TEST_ASSERT_EQUAL_PTR(&PROFILE_TYPE_V20_FAMILY, profile_for_model(x20, strlen(x20)));
}

void test_profile_v20_family_keeps_the_store() {
  const OcleanProfile &f = PROFILE_TYPE_V20_FAMILY;
  TEST_ASSERT_EQUAL_UINT8(1, f.confidence);
  TEST_ASSERT_EQUAL_INT(SessionFormat::VARIABLE, f.session_format);
  TEST_ASSERT_EQUAL_INT(SettingsKind::SETTINGS_V20_34B, f.settings_kind);
  TEST_ASSERT_EQUAL_INT(BrushModel::X_ULTRA_20, f.entity_model);
  TEST_ASSERT_TRUE(f.inline_is_session);
  TEST_ASSERT_FALSE(f.clears_inline_when_docked);
  TEST_ASSERT_FALSE(f.sends_clear_running_data);
  const uint8_t clear[] = {0x02, 0x02};
  TEST_ASSERT_FALSE(command_permitted(false, f, clear, sizeof(clear)));
  TEST_ASSERT_TRUE(command_permitted(false, PROFILE_TYPE_V20, clear, sizeof(clear)));
  TEST_ASSERT_EQUAL_UINT8(PROFILE_TYPE_V20.query_cmd_count, f.query_cmd_count);
}

void test_profile_v20_contract() {
  TEST_ASSERT_EQUAL_UINT8(2, PROFILE_TYPE_V20.confidence);
  TEST_ASSERT_EQUAL_INT(SessionFormat::VARIABLE, PROFILE_TYPE_V20.session_format);
  TEST_ASSERT_EQUAL_INT(SettingsKind::SETTINGS_V20_34B, PROFILE_TYPE_V20.settings_kind);
  TEST_ASSERT_FALSE(PROFILE_TYPE_V20.allows_writes);
  TEST_ASSERT_TRUE(PROFILE_TYPE_V20.allows_clock_write);
  TEST_ASSERT_FALSE(PROFILE_TYPE_V20.skip_cccd_write);
  TEST_ASSERT_FALSE(PROFILE_TYPE_V20.sends_clear_running_data);
  TEST_ASSERT_TRUE(PROFILE_TYPE_V20.inline_is_session);
  TEST_ASSERT_TRUE(PROFILE_TYPE_V20.clears_inline_when_docked);
  // the mode byte shares its numbering with the settings readback
  TEST_ASSERT_TRUE(PROFILE_TYPE_V20.cloud_scheme_ids);
  TEST_ASSERT_FALSE(PROFILE_TYPE_V20.battery_char_reliable);
  // TYPE1's queries first, then the four reads only this model answers
  TEST_ASSERT_EQUAL_UINT8(PROFILE_TYPE1.query_cmd_count + 4, PROFILE_TYPE_V20.query_cmd_count);
  for (uint8_t i = 0; i < PROFILE_TYPE1.query_cmd_count; i++) {
    TEST_ASSERT_EQUAL_UINT8(PROFILE_TYPE1.query_cmds[i].len, PROFILE_TYPE_V20.query_cmds[i].len);
    TEST_ASSERT_EQUAL_MEMORY(PROFILE_TYPE1.query_cmds[i].bytes, PROFILE_TYPE_V20.query_cmds[i].bytes,
                             PROFILE_TYPE1.query_cmds[i].len);
    TEST_ASSERT_EQUAL_INT(PROFILE_TYPE1.query_cmds[i].target, PROFILE_TYPE_V20.query_cmds[i].target);
  }
  const uint8_t extra[4][2] = {{0x03, 0x14}, {0x03, 0x16}, {0x03, 0xA0}, {0x02, 0x34}};
  for (uint8_t i = 0; i < 4; i++) {
    const ProfileCmd &q = PROFILE_TYPE_V20.query_cmds[PROFILE_TYPE1.query_cmd_count + i];
    TEST_ASSERT_EQUAL_UINT8(2, q.len);
    TEST_ASSERT_EQUAL_MEMORY(extra[i], q.bytes, 2);
    TEST_ASSERT_EQUAL_INT(WriteTarget::TX_MAIN, q.target);
  }
}

// The keys are what the mismatch warning tells the user to type, so they must
// match the yaml schema.
void test_profile_entity_model() {
  TEST_ASSERT_EQUAL_INT(BrushModel::X_ULTRA_20, PROFILE_TYPE_V20.entity_model);
  TEST_ASSERT_EQUAL_INT(BrushModel::X_PRO_ELITE, PROFILE_TYPE1.entity_model);
  TEST_ASSERT_EQUAL_INT(BrushModel::X_PRO_ELITE, PROFILE_TYPE_Z1.entity_model);
  TEST_ASSERT_EQUAL_INT(BrushModel::X_PRO_ELITE, PROFILE_UNKNOWN.entity_model);
  TEST_ASSERT_EQUAL_STRING("x_ultra_20", brush_model_key(BrushModel::X_ULTRA_20));
  TEST_ASSERT_EQUAL_STRING("x_pro_elite", brush_model_key(BrushModel::X_PRO_ELITE));
}

void test_validated_profiles_keep_writes_and_cccd_skip() {
  TEST_ASSERT_TRUE(PROFILE_TYPE1.allows_writes);
  TEST_ASSERT_TRUE(PROFILE_TYPE1.allows_clock_write);
  TEST_ASSERT_TRUE(PROFILE_TYPE1.skip_cccd_write);
  TEST_ASSERT_TRUE(PROFILE_TYPE1.sends_clear_running_data);
  // an inline head here is a record already handed over, never a new session
  TEST_ASSERT_FALSE(PROFILE_TYPE1.inline_is_session);
  TEST_ASSERT_FALSE(PROFILE_TYPE1.clears_inline_when_docked);
  TEST_ASSERT_FALSE(PROFILE_TYPE_Z1.inline_is_session);
  TEST_ASSERT_FALSE(PROFILE_UNKNOWN.clears_inline_when_docked);
  TEST_ASSERT_TRUE(PROFILE_TYPE1.cloud_scheme_ids);
  TEST_ASSERT_TRUE(PROFILE_TYPE1.battery_char_reliable);
  TEST_ASSERT_TRUE(PROFILE_TYPE_Z1.battery_char_reliable);
  TEST_ASSERT_TRUE(PROFILE_UNKNOWN.battery_char_reliable);
  TEST_ASSERT_EQUAL_INT(SessionFormat::FIXED_42, PROFILE_TYPE1.session_format);
  TEST_ASSERT_TRUE(PROFILE_TYPE_Z1.allows_writes);
  TEST_ASSERT_TRUE(PROFILE_TYPE_Z1.skip_cccd_write);
  TEST_ASSERT_TRUE(PROFILE_TYPE_Z1.sends_clear_running_data);
  TEST_ASSERT_TRUE(PROFILE_UNKNOWN.allows_writes);
  TEST_ASSERT_TRUE(PROFILE_UNKNOWN.skip_cccd_write);
  TEST_ASSERT_FALSE(PROFILE_UNKNOWN.sends_clear_running_data);
  TEST_ASSERT_EQUAL_INT(SessionFormat::NONE, PROFILE_UNKNOWN.session_format);
}

void test_clock_write_permitted() {
  TEST_ASSERT_TRUE(clock_write_permitted(false, PROFILE_TYPE1));
  TEST_ASSERT_FALSE(clock_write_permitted(true, PROFILE_TYPE1));
  TEST_ASSERT_TRUE(clock_write_permitted(false, PROFILE_TYPE_V20));
  TEST_ASSERT_FALSE(clock_write_permitted(true, PROFILE_TYPE_V20));
}

void test_kept_values_only_on_the_x_ultra_20_family() {
  TEST_ASSERT_TRUE(kept_values_permitted(PROFILE_TYPE_V20));
  TEST_ASSERT_TRUE(kept_values_permitted(PROFILE_TYPE_V20_FAMILY));
  // TYPE1 lets the cloud host frame through like any write: only this gate
  // keeps it off an X Pro Elite
  std::vector<uint8_t> const host = build_set_cloud_host_command("http://192.0.2.10:80");
  TEST_ASSERT_TRUE(command_permitted(false, PROFILE_TYPE1, host.data(), host.size()));
  TEST_ASSERT_FALSE(kept_values_permitted(PROFILE_TYPE1));
  TEST_ASSERT_FALSE(kept_values_permitted(PROFILE_TYPE_Z1));
  TEST_ASSERT_FALSE(kept_values_permitted(PROFILE_UNKNOWN));
}

static const uint8_t STATUS_CMD[] = {0x03, 0x03};
static const uint8_t SETTINGS_CMD[] = {0x03, 0x02, 0x01};
static const uint8_t DOWNLOAD_CMD[] = {0x03, 0x07};
static const uint8_t CLEAR_RUNNING_DATA_CMD[] = {0x02, 0x02};
static const uint8_t SET_CLOCK_CMD[] = {0x02, 0x01, 0x1A, 0x0A, 0x05, 0x13, 0x2D, 0x00, 0x00, 0x10};
static const uint8_t AREA_REMINDER_ON_CMD[] = {0x02, 0x0D, 0x01};
static const uint8_t FORCE_UPDATE_CMD[] = {0x03, 0x13, 0x00, 0x08, 0x03, 0x07};

void test_command_permitted_read_only_allows_only_profile_reads() {
  const uint8_t status_prefix[] = {0x03};
  for (const OcleanProfile *p : {&PROFILE_TYPE1, &PROFILE_TYPE_V20}) {
    TEST_ASSERT_TRUE(command_permitted(true, *p, STATUS_CMD, sizeof(STATUS_CMD)));
    TEST_ASSERT_TRUE(command_permitted(true, *p, SETTINGS_CMD, sizeof(SETTINGS_CMD)));
    TEST_ASSERT_TRUE(command_permitted(true, *p, DOWNLOAD_CMD, sizeof(DOWNLOAD_CMD)));
    TEST_ASSERT_FALSE(command_permitted(true, *p, CLEAR_RUNNING_DATA_CMD, sizeof(CLEAR_RUNNING_DATA_CMD)));
    TEST_ASSERT_FALSE(command_permitted(true, *p, SET_CLOCK_CMD, sizeof(SET_CLOCK_CMD)));
    TEST_ASSERT_FALSE(command_permitted(true, *p, AREA_REMINDER_ON_CMD, sizeof(AREA_REMINDER_ON_CMD)));
    TEST_ASSERT_FALSE(command_permitted(true, *p, FORCE_UPDATE_CMD, sizeof(FORCE_UPDATE_CMD)));
    TEST_ASSERT_FALSE(command_permitted(true, *p, status_prefix, sizeof(status_prefix)));
    TEST_ASSERT_FALSE(command_permitted(true, *p, nullptr, 0));
  }
}

void test_command_permitted_v20_passes_only_listed_writes() {
  const OcleanProfile &p = PROFILE_TYPE_V20;
  TEST_ASSERT_TRUE(command_permitted(false, p, STATUS_CMD, sizeof(STATUS_CMD)));
  TEST_ASSERT_TRUE(command_permitted(false, p, DOWNLOAD_CMD, sizeof(DOWNLOAD_CMD)));
  TEST_ASSERT_TRUE(command_permitted(false, p, SET_CLOCK_CMD, sizeof(SET_CLOCK_CMD)));
  TEST_ASSERT_FALSE(command_permitted(false, p, SET_CLOCK_CMD, sizeof(SET_CLOCK_CMD) - 1));
  TEST_ASSERT_TRUE(command_permitted(false, p, AREA_REMINDER_ON_CMD, sizeof(AREA_REMINDER_ON_CMD)));
  const uint8_t voice[] = {0x02, 0x31, 0x01, 0x00, 0x01, 0x00};
  TEST_ASSERT_TRUE(command_permitted(false, p, voice, sizeof(voice)));
  const uint8_t head_reset[] = {0x02, 0x0F};
  TEST_ASSERT_TRUE(command_permitted(false, p, head_reset, sizeof(head_reset)));
  // the store clear after an inline record, the program, teaching, retail,
  // cloud-host and birthday writes
  const uint8_t teaching[] = {0x02, 0x30, 0x01};
  const uint8_t scheme[] = {0x02, 0x06, 0x79, 0x01, 0x00, 0x2F, 0xB4, 0x00, 0x05};
  const uint8_t scheme_tail[] = {0x02, 0x0B, 0x00, 0x10, 0x1E, 0x00, 0x05};
  const uint8_t demo[] = {0x02, 0xA0, 0x01};
  const uint8_t cloud_host[] = {0x02, 0x33, 0x2A, 0x01, 0x01, 0x68};
  const std::vector<uint8_t> birthday = build_birthday_command(0, 18, 3, 7);
  TEST_ASSERT_TRUE(command_permitted(false, p, CLEAR_RUNNING_DATA_CMD, sizeof(CLEAR_RUNNING_DATA_CMD)));
  TEST_ASSERT_TRUE(command_permitted(false, p, teaching, sizeof(teaching)));
  TEST_ASSERT_TRUE(command_permitted(false, p, scheme, sizeof(scheme)));
  TEST_ASSERT_TRUE(command_permitted(false, p, scheme_tail, sizeof(scheme_tail)));
  TEST_ASSERT_TRUE(command_permitted(false, p, demo, sizeof(demo)));
  TEST_ASSERT_TRUE(command_permitted(false, p, cloud_host, sizeof(cloud_host)));
  TEST_ASSERT_TRUE(command_permitted(false, p, birthday.data(), birthday.size()));
  // a read-only hub still refuses the cloud-host and birthday writes
  TEST_ASSERT_FALSE(command_permitted(true, p, cloud_host, sizeof(cloud_host)));
  TEST_ASSERT_FALSE(command_permitted(true, p, birthday.data(), birthday.size()));
  // firmware update, OTA host and factory reset stay out
  const uint8_t ota_url[] = {0x03, 0xA1, 0x68};
  const uint8_t factory_reset[] = {0x09, 0xED, 0xEF};
  const uint8_t clear_with_payload[] = {0x02, 0x02, 0x00};
  const uint8_t opcode_half[] = {0x02};
  TEST_ASSERT_FALSE(command_permitted(false, p, FORCE_UPDATE_CMD, sizeof(FORCE_UPDATE_CMD)));
  TEST_ASSERT_FALSE(command_permitted(false, p, ota_url, sizeof(ota_url)));
  TEST_ASSERT_FALSE(command_permitted(false, p, factory_reset, sizeof(factory_reset)));
  TEST_ASSERT_FALSE(command_permitted(false, p, clear_with_payload, sizeof(clear_with_payload)));
  TEST_ASSERT_FALSE(command_permitted(false, p, opcode_half, sizeof(opcode_half)));
  TEST_ASSERT_FALSE(command_permitted(true, p, AREA_REMINDER_ON_CMD, sizeof(AREA_REMINDER_ON_CMD)));
  TEST_ASSERT_FALSE(command_permitted(true, p, CLEAR_RUNNING_DATA_CMD, sizeof(CLEAR_RUNNING_DATA_CMD)));
}

void test_command_permitted_v20_extra_reads() {
  const uint8_t wifi_check[] = {0x02, 0x34};
  const uint8_t running[] = {0x03, 0x14};
  TEST_ASSERT_TRUE(command_permitted(true, PROFILE_TYPE_V20, running, sizeof(running)));
  TEST_ASSERT_FALSE(command_permitted(true, PROFILE_TYPE_V20, wifi_check, sizeof(wifi_check)));
  TEST_ASSERT_TRUE(command_permitted(false, PROFILE_TYPE_V20, wifi_check, sizeof(wifi_check)));
  TEST_ASSERT_FALSE(command_permitted(true, PROFILE_TYPE1, wifi_check, sizeof(wifi_check)));
}

void test_voice_prompts_command() {
  const std::array<bool, VOICE_PROMPT_COUNT> flags = {true, false, true};
  const std::vector<uint8_t> cmd = build_voice_prompts_command(flags);
  const uint8_t expect[] = {0x02, 0x31, 0x01, 0x00, 0x01, 0x00};
  TEST_ASSERT_EQUAL_size_t(sizeof(expect), cmd.size());
  TEST_ASSERT_EQUAL_MEMORY(expect, cmd.data(), sizeof(expect));
}

void test_parse_status_reply() {
  uint8_t status = 0xFF;
  const uint8_t running[] = {0x03, 0x14, 0x01};
  TEST_ASSERT_TRUE(parse_status_reply(running, sizeof(running), 0x03, 0x14, &status));
  TEST_ASSERT_EQUAL_UINT8(1, status);
  const uint8_t wifi[] = {0x02, 0x34, 0x00, 0x4E, 0x47};
  TEST_ASSERT_TRUE(parse_status_reply(wifi, sizeof(wifi), 0x02, 0x34, &status));
  TEST_ASSERT_EQUAL_UINT8(0, status);
  TEST_ASSERT_FALSE(parse_status_reply(running, sizeof(running), 0x03, 0x16, &status));
  TEST_ASSERT_FALSE(parse_status_reply(running, 2, 0x03, 0x14, &status));
  TEST_ASSERT_FALSE(parse_status_reply(nullptr, 3, 0x03, 0x14, &status));
  TEST_ASSERT_FALSE(parse_status_reply(running, sizeof(running), 0x03, 0x14, nullptr));
}

// captured 2026-10-05 from an X Ultra 20 (OCLEANV20B, firmware 0.0.2.1)
static const uint8_t V20_SETTINGS_START[] = {0x03, 0x02, 0x23, 0x24, 0x5E, 0x00, 0x01, 0x01, 0x00, 0x05,
                                             0x00, 0x01, 0x01, 0x01, 0x01, 0x05, 0x00, 0x02, 0x00, 0x00};
static const uint8_t V20_SETTINGS_CONT[] = {0x03, 0x02, 0x1A, 0x0A, 0x06, 0x03, 0x2E, 0x1F, 0x00, 0x00,
                                            0x1B, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00};
// after the clock write at 22:04:22
static const uint8_t V20_SETTINGS_CONT_SYNCED[] = {0x03, 0x02, 0x1A, 0x0A, 0x05, 0x16, 0x04, 0x17, 0x00, 0x00,
                                                   0x10, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00};
static const uint8_t V20_INLINE_0307[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x8D, 0x1A, 0x0A, 0x06, 0x02, 0x27, 0x25, 0x02, 0x00, 0xB4};

void test_v20_settings_real_frames() {
  SettingsAssembler s;
  s.reset();
  s.feed(V20_SETTINGS_START, sizeof(V20_SETTINGS_START));
  TEST_ASSERT_TRUE(s.feed(V20_SETTINGS_CONT, sizeof(V20_SETTINGS_CONT)));
  DeviceSettingsV20Start start{};
  parse_device_settings_v20_start(s.buffer(), &start);
  TEST_ASSERT_EQUAL_UINT8(94, start.battery);
  TEST_ASSERT_EQUAL_UINT8(0, start.network_status);
  TEST_ASSERT_TRUE(start.raise_wake);
  TEST_ASSERT_TRUE(start.auto_update);
  TEST_ASSERT_FALSE(start.auto_mode);
  TEST_ASSERT_EQUAL_UINT8(5, start.mode_num);
  TEST_ASSERT_TRUE(start.voice);
  TEST_ASSERT_TRUE(start.voice_fast_brushing);
  TEST_ASSERT_TRUE(start.voice_pressure);
  TEST_ASSERT_TRUE(start.festival_reminder);
  TEST_ASSERT_EQUAL_UINT8(5, start.mode);
  TEST_ASSERT_TRUE(start.brush_mode_on);
  TEST_ASSERT_EQUAL_UINT8(2, start.scheme_type);
  TEST_ASSERT_EQUAL_UINT16(0, start.head_used_time);
  DeviceSettings ds{};
  parse_device_settings(s.buffer(), &ds);
  TEST_ASSERT_TRUE(ds.clock_valid);
  TEST_ASSERT_EQUAL_UINT16(2026, ds.year);
  TEST_ASSERT_EQUAL_UINT8(10, ds.month);
  TEST_ASSERT_EQUAL_UINT8(6, ds.day);
  TEST_ASSERT_EQUAL_UINT8(3, ds.hour);
  TEST_ASSERT_EQUAL_UINT8(46, ds.minute);
  TEST_ASSERT_EQUAL_UINT8(31, ds.second);
  TEST_ASSERT_FALSE(ds.over_pressure);
  TEST_ASSERT_FALSE(ds.area_reminder);
  TEST_ASSERT_EQUAL_STRING("GMT+08:00", timezone_index_to_string(ds.tz_index));
  TEST_ASSERT_EQUAL_UINT16(240, ds.head_max);
  TEST_ASSERT_EQUAL_UINT16(0, ds.head_used_days);
  TEST_ASSERT_EQUAL_UINT16(0, ds.head_used_times);
  TEST_ASSERT_EQUAL_UINT8(3, ds.device_language);
}

void test_v20_settings_after_clock_write() {
  SettingsAssembler s;
  s.reset();
  s.feed(V20_SETTINGS_CONT_SYNCED, sizeof(V20_SETTINGS_CONT_SYNCED));
  DeviceSettings ds{};
  parse_device_settings(s.buffer(), &ds);
  TEST_ASSERT_EQUAL_UINT8(5, ds.day);
  TEST_ASSERT_EQUAL_UINT8(22, ds.hour);
  TEST_ASSERT_EQUAL_UINT8(4, ds.minute);
  TEST_ASSERT_EQUAL_UINT8(23, ds.second);
  TEST_ASSERT_EQUAL_STRING("GMT+02:00", timezone_index_to_string(ds.tz_index));
}

void test_v20_inline_0307_real_frame() {
  SessionRecord r{};
  TEST_ASSERT_TRUE(decode_inline_0307_v20(V20_INLINE_0307, sizeof(V20_INLINE_0307), &r));
  TEST_ASSERT_EQUAL_UINT16(2026, r.year);
  TEST_ASSERT_EQUAL_UINT8(10, r.month);
  TEST_ASSERT_EQUAL_UINT8(6, r.day);
  TEST_ASSERT_EQUAL_UINT8(2, r.hour);
  TEST_ASSERT_EQUAL_UINT8(39, r.minute);
  TEST_ASSERT_EQUAL_UINT8(37, r.second);
  TEST_ASSERT_EQUAL_UINT8(2, r.scheme);
  TEST_ASSERT_EQUAL_UINT16(180, r.duration_s);
  // length 0x8D = 141: 2 * (141 - 51) = 180 s brushed, the whole program
  TEST_ASSERT_EQUAL_UINT16(180, r.valid_duration_s);
  TEST_ASSERT_FALSE(r.has_score);
  TEST_ASSERT_EQUAL_UINT8(SESSION_ZONE_ABSENT, r.zones[0]);
  TEST_ASSERT_FALSE(decode_inline_0307_v20(V20_INLINE_0307, sizeof(V20_INLINE_0307) - 1, &r));
  const uint8_t y3p_inline[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00, 0x1A, 0x06, 0x0A,
                                0x07, 0x22, 0x2B, 0x00, 0x00, 0x78, 0x00, 0x44, 0x05, 0x16};
  TEST_ASSERT_FALSE(decode_inline_0307_v20(y3p_inline, sizeof(y3p_inline), &r));
}

void test_v20_inline_against_brush_clock() {
  SessionRecord r{};
  TEST_ASSERT_TRUE(decode_inline_0307_v20(V20_INLINE_0307, sizeof(V20_INLINE_0307), &r));
  // read 2026-10-05 21:45:12 UTC+2 while the brush clock said 03:46:31 (UTC+8)
  SessionClocks const factory{.node_local = civil_to_epoch(2026, 10, 5, 21, 45, 12),
                              .brush = civil_to_epoch(2026, 10, 6, 3, 46, 31)};
  TEST_ASSERT_TRUE(accept_inline_record(r, 0, nullptr, factory));
  // after the 22:04 set the brush cannot have written 02:39:37 yet
  SessionClocks const set{.node_local = civil_to_epoch(2026, 10, 5, 23, 0, 0),
                          .brush = civil_to_epoch(2026, 10, 5, 23, 0, 2)};
  TEST_ASSERT_FALSE(accept_inline_record(r, 0, nullptr, set));
  SessionClocks const morning{.node_local = civil_to_epoch(2026, 10, 6, 7, 0, 0),
                              .brush = civil_to_epoch(2026, 10, 6, 7, 0, 2)};
  TEST_ASSERT_TRUE(accept_inline_record(r, 0, nullptr, morning));
}

static std::vector<uint8_t> v20_record(size_t len, uint8_t hour, uint8_t minute, uint8_t mode, uint16_t program,
                                       uint16_t brushed, uint8_t score) {
  std::vector<uint8_t> r(len, 0xFF);
  r[0] = static_cast<uint8_t>(len >> 8);
  r[1] = static_cast<uint8_t>(len);
  const uint8_t when[] = {0x1A, 0x0A, 0x06, hour, minute, 0x00};
  for (size_t i = 0; i < sizeof(when); i++)
    r[2 + i] = when[i];
  r[8] = mode;
  r[9] = static_cast<uint8_t>(program >> 8);
  r[10] = static_cast<uint8_t>(program);
  r[11] = static_cast<uint8_t>(brushed >> 8);
  r[12] = static_cast<uint8_t>(brushed);
  const uint8_t unread[] = {10, 20, 70, 0, 0, 0, 0x10};
  for (size_t i = 0; i < sizeof(unread); i++)
    r[13 + i] = unread[i];
  r[SESSION_V20_SCORE_OFFSET] = score;
  return r;
}

void test_v20_brushed_seconds_from_length() {
  // the shortest record the firmware keeps: 15 s brushed, length 58
  TEST_ASSERT_EQUAL_UINT16(14, v20_brushed_seconds(58, 120));
  TEST_ASSERT_EQUAL_UINT16(180, v20_brushed_seconds(141, 180));
  TEST_ASSERT_EQUAL_UINT16(260, v20_brushed_seconds(181, 300));
  // cut at 182: 262 s or more, known only when the program is no longer
  TEST_ASSERT_EQUAL_UINT16(262, v20_brushed_seconds(182, 262));
  TEST_ASSERT_EQUAL_UINT16(0, v20_brushed_seconds(182, 300));
  TEST_ASSERT_EQUAL_UINT16(0, v20_brushed_seconds(57, 120));
  TEST_ASSERT_EQUAL_UINT16(0, v20_brushed_seconds(183, 120));
}

void test_v20_record_carries_its_time_zone() {
  std::vector<uint8_t> rec(SESSION_V20_RECORD_MIN, 0xFF);
  rec[0] = 0;
  rec[1] = static_cast<uint8_t>(rec.size());
  const uint8_t when[] = {0x1A, 0x0A, 0x06, 0x02, 0x27, 0x25};
  for (size_t i = 0; i < sizeof(when); i++)
    rec[2 + i] = when[i];
  rec[SESSION_V20_TZ_OFFSET] = 27;
  SessionRecord r{};
  TEST_ASSERT_TRUE(decode_session_record_v20(rec.data(), rec.size(), &r));
  TEST_ASSERT_EQUAL_UINT8(27, r.tz_index);
  TEST_ASSERT_EQUAL_INT64(28800, session_utc_offset_seconds(r, 7200));
  TEST_ASSERT_FALSE(decode_session_record_v20(rec.data(), SESSION_V20_RECORD_MIN - 1, &r));
}

void test_should_clear_inline_only_on_the_dock() {
  SessionRecord inl{};
  TEST_ASSERT_TRUE(decode_inline_0307_v20(V20_INLINE_0307, sizeof(V20_INLINE_0307), &inl));
  uint32_t const epoch = session_record_epoch(inl);
  SessionIngestPlan plan = plan_session_ingest({inl}, 0, 0, node_only(civil_to_epoch(2026, 10, 7, 9, 0, 0)));
  TEST_ASSERT_TRUE(should_clear_inline(true, true, true, plan, epoch, 0));
  // off the dock, a round without STATUS, a profile that keeps the store
  TEST_ASSERT_FALSE(should_clear_inline(true, true, false, plan, epoch, 0));
  TEST_ASSERT_FALSE(should_clear_inline(true, false, true, plan, epoch, 0));
  TEST_ASSERT_FALSE(should_clear_inline(false, true, true, plan, epoch, 0));
  // acked once already: the same head stays until a new session overwrites it
  TEST_ASSERT_FALSE(should_clear_inline(true, true, true, plan, epoch, epoch));
  // an implausible date stays in the store until it can be judged
  plan = plan_session_ingest({inl}, 0, 0, node_only(civil_to_epoch(2026, 10, 1, 9, 0, 0)));
  TEST_ASSERT_EQUAL_UINT(1, plan.implausible.size());
  TEST_ASSERT_FALSE(should_clear_inline(true, true, true, plan, epoch, 0));
}

void test_is_bare_ack() {
  const uint8_t ack[] = {0x02, 0x02, 0x4F, 0x4B};
  const uint8_t echo[] = {0x02, 0x12, 0x01, 0x4F, 0x4B};
  TEST_ASSERT_TRUE(is_bare_ack(ack, sizeof(ack), 0x02, 0x02));
  TEST_ASSERT_FALSE(is_bare_ack(ack, sizeof(ack), 0x02, 0x30));
  TEST_ASSERT_FALSE(is_bare_ack(echo, sizeof(echo), 0x02, 0x12));
  TEST_ASSERT_FALSE(is_bare_ack(nullptr, 4, 0x02, 0x02));
}

void test_voice_prompt_flags_need_the_main_one() {
  TEST_ASSERT_TRUE(voice_prompt_write_takes(0, false));
  TEST_ASSERT_TRUE(voice_prompt_write_takes(0, true));
  TEST_ASSERT_TRUE(voice_prompt_write_takes(1, true));
  TEST_ASSERT_FALSE(voice_prompt_write_takes(1, false));
  TEST_ASSERT_FALSE(voice_prompt_write_takes(2, false));
}

void test_v20_assembler_splits_length_prefixed_records() {
  std::vector<uint8_t> const a = v20_record(58, 7, 30, 2, 180, 80, 75);
  std::vector<uint8_t> const b = v20_record(68, 21, 10, 3, 120, 20, 30);
  std::vector<uint8_t> body(a);
  body.insert(body.end(), b.begin(), b.end());
  std::vector<uint8_t> first = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x02, 0x00, static_cast<uint8_t>(body.size())};
  first.insert(first.end(), body.begin(), body.begin() + 11);
  VarSessionAssembler s;
  TEST_ASSERT_FALSE(s.feed(first.data(), first.size()));
  TEST_ASSERT_FALSE(s.feed(body.data() + 11, 60));
  TEST_ASSERT_TRUE(s.feed(body.data() + 71, body.size() - 71));
  TEST_ASSERT_EQUAL_UINT(2, s.record_count());
  SessionRecord r{};
  TEST_ASSERT_TRUE(s.record(0, &r));
  TEST_ASSERT_EQUAL_UINT8(7, r.hour);
  TEST_ASSERT_EQUAL_UINT16(180, r.duration_s);
  TEST_ASSERT_EQUAL_UINT16(80, r.valid_duration_s);
  TEST_ASSERT_EQUAL_UINT8(75, r.score);
  TEST_ASSERT_TRUE(r.has_score);
  TEST_ASSERT_TRUE(s.record(1, &r));
  TEST_ASSERT_EQUAL_UINT8(21, r.hour);
  TEST_ASSERT_EQUAL_UINT8(10, r.minute);
  TEST_ASSERT_EQUAL_UINT8(3, r.scheme);
  TEST_ASSERT_EQUAL_UINT16(120, r.duration_s);
  TEST_ASSERT_EQUAL_UINT16(20, r.valid_duration_s);
  TEST_ASSERT_EQUAL_UINT8(SESSION_ZONE_ABSENT, r.quadrants[0]);
  TEST_ASSERT_EQUAL_UINT8(SESSION_ZONE_ABSENT, r.zones[7]);
  TEST_ASSERT_EQUAL_INT(1, s.newest_index());
  size_t raw_len = 0;
  TEST_ASSERT_NOT_NULL(s.raw_record(1, &raw_len));
  TEST_ASSERT_EQUAL_UINT(68, raw_len);
  TEST_ASSERT_TRUE(s.feed(first.data(), first.size()));
}

void test_v20_assembler_header_cases() {
  const uint8_t empty[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00, 0x00, 0x00};
  VarSessionAssembler e;
  TEST_ASSERT_FALSE(e.feed(empty, sizeof(empty)));
  TEST_ASSERT_TRUE(e.empty());
  TEST_ASSERT_FALSE(e.failed());
  const uint8_t bad_magic[] = {0x03, 0x07, 0x2A, 0x42, 0x24, 0x00, 0x01, 0x00, 0x33};
  VarSessionAssembler m;
  m.feed(bad_magic, sizeof(bad_magic));
  TEST_ASSERT_TRUE(m.failed());
  const uint8_t too_many[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x22, 0x00, 0x33};
  VarSessionAssembler t;
  t.feed(too_many, sizeof(too_many));
  TEST_ASSERT_TRUE(t.failed());
  const uint8_t too_long[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x01, 0x18, 0x00};
  VarSessionAssembler l;
  l.feed(too_long, sizeof(too_long));
  TEST_ASSERT_TRUE(l.failed());
  VarSessionAssembler n;
  TEST_ASSERT_FALSE(n.feed(nullptr, 9));
  TEST_ASSERT_TRUE(n.failed());
}

void test_v20_assembler_stops_at_a_bad_length() {
  std::vector<uint8_t> body = v20_record(58, 7, 30, 2, 180, 80, 75);
  std::vector<uint8_t> bogus = v20_record(58, 8, 0, 2, 180, 80, 75);
  bogus[1] = 200;
  body.insert(body.end(), bogus.begin(), bogus.end());
  std::vector<uint8_t> frame = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x02, 0x00, static_cast<uint8_t>(body.size())};
  frame.insert(frame.end(), body.begin(), body.end());
  VarSessionAssembler s;
  TEST_ASSERT_TRUE(s.feed(frame.data(), frame.size()));
  TEST_ASSERT_EQUAL_UINT(1, s.record_count());
}

void test_command_permitted_unlocked_type1_unchanged() {
  TEST_ASSERT_TRUE(command_permitted(false, PROFILE_TYPE1, CLEAR_RUNNING_DATA_CMD, sizeof(CLEAR_RUNNING_DATA_CMD)));
  TEST_ASSERT_TRUE(command_permitted(false, PROFILE_TYPE1, AREA_REMINDER_ON_CMD, sizeof(AREA_REMINDER_ON_CMD)));
  TEST_ASSERT_TRUE(command_permitted(false, PROFILE_TYPE1, SET_CLOCK_CMD, sizeof(SET_CLOCK_CMD)));
}

void test_decode_hw_revision_code() {
  const uint8_t coded[] = {'H', 'H', 0x00, 0x05, 0x00, 0x01};
  HwRevisionCode code{};
  TEST_ASSERT_TRUE(decode_hw_revision_code(coded, sizeof(coded), &code));
  TEST_ASSERT_EQUAL_HEX16(0x0005, code.protocol);
  TEST_ASSERT_EQUAL_HEX16(0x0001, code.ota_type);
  TEST_ASSERT_FALSE(decode_hw_revision_code(coded, 5, &code));
  const uint8_t ascii[] = {'R', 'e', 'v', '.', 'D', 0x00};
  TEST_ASSERT_FALSE(decode_hw_revision_code(ascii, sizeof(ascii), &code));
  TEST_ASSERT_FALSE(decode_hw_revision_code(nullptr, 6, &code));
  TEST_ASSERT_FALSE(decode_hw_revision_code(coded, sizeof(coded), nullptr));
}

void test_hw_revision_text() {
  const uint8_t coded[] = {'H', 'H', 0x00, 0x05, 0x00, 0x01};
  TEST_ASSERT_EQUAL_STRING("HH 0005/0001", hw_revision_text(coded, sizeof(coded)).c_str());
  const uint8_t ascii[] = {'R', 'e', 'v', '.', 'D'};
  TEST_ASSERT_EQUAL_STRING("Rev.D", hw_revision_text(ascii, sizeof(ascii)).c_str());
  const uint8_t short_hh[] = {'H', 'H', 0x00, 0x05};
  TEST_ASSERT_EQUAL_STRING("HH", hw_revision_text(short_hh, sizeof(short_hh)).c_str());
}

void test_dis_printable_text() {
  const uint8_t raw[] = {'O', 'C', 0x00, 'L', 0x7F, 'E', 0x1F, 'A', 0xFF, 'N'};
  TEST_ASSERT_EQUAL_STRING("OCLEAN", dis_printable_text(raw, sizeof(raw)).c_str());
  TEST_ASSERT_EQUAL_STRING("", dis_printable_text(nullptr, 4).c_str());
}

void test_gatt_props_text() {
  TEST_ASSERT_EQUAL_STRING("RN", gatt_props_text(0x12).c_str());
  TEST_ASSERT_EQUAL_STRING("RW", gatt_props_text(0x0A).c_str());
  TEST_ASSERT_EQUAL_STRING("w", gatt_props_text(0x04).c_str());
  TEST_ASSERT_EQUAL_STRING("I", gatt_props_text(0x20).c_str());
  TEST_ASSERT_EQUAL_STRING("RWwNI", gatt_props_text(0x3E).c_str());
  TEST_ASSERT_EQUAL_STRING("-", gatt_props_text(0x00).c_str());
}

void test_ble_uuid_text() {
  const uint8_t u16[] = {0x0A, 0x18};
  TEST_ASSERT_EQUAL_STRING("0x180A", ble_uuid_text(u16, sizeof(u16)).c_str());
  const uint8_t u32[] = {0x78, 0x56, 0x34, 0x12};
  TEST_ASSERT_EQUAL_STRING("0x12345678", ble_uuid_text(u32, sizeof(u32)).c_str());
  const uint8_t service[] = {0x18, 0xCC, 0x54, 0xB9, 0xF9, 0x56, 0xC6, 0x91,
                             0x21, 0x40, 0xA6, 0x41, 0xA8, 0xCA, 0x82, 0x80};
  TEST_ASSERT_EQUAL_STRING("8082caa8-41a6-4021-91c6-56f9b954cc18", ble_uuid_text(service, sizeof(service)).c_str());
  TEST_ASSERT_EQUAL_STRING("?", ble_uuid_text(service, 3).c_str());
  TEST_ASSERT_EQUAL_STRING("?", ble_uuid_text(nullptr, 16).c_str());
}

// Real inline count=0 frames captured 2026-06-10 from both brushes.
void test_decode_inline_0307() {
  static const uint8_t G[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00, 0x1A, 0x06, 0x0A,
                              0x07, 0x22, 0x2B, 0x00, 0x00, 0x78, 0x00, 0x44, 0x05, 0x16};
  SessionRecord r;
  TEST_ASSERT_TRUE(decode_inline_0307(G, sizeof(G), &r));
  TEST_ASSERT_EQUAL_UINT16(2026, r.year);
  TEST_ASSERT_EQUAL_UINT8(6, r.month);
  TEST_ASSERT_EQUAL_UINT8(10, r.day);
  TEST_ASSERT_EQUAL_UINT8(7, r.hour);
  TEST_ASSERT_EQUAL_UINT8(34, r.minute);
  TEST_ASSERT_EQUAL_UINT8(43, r.second);
  TEST_ASSERT_EQUAL_UINT8(0, r.scheme);
  TEST_ASSERT_EQUAL_UINT16(120, r.duration_s);
  TEST_ASSERT_EQUAL_UINT16(68, r.valid_duration_s);
  // 13 bytes of the record: no time zone and no quadrants in them
  TEST_ASSERT_EQUAL_UINT8(0, r.tz_index);
  TEST_ASSERT_EQUAL_UINT8(SESSION_ZONE_ABSENT, r.quadrants[3]);
  TEST_ASSERT_FALSE(r.has_score);
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    TEST_ASSERT_EQUAL_UINT8(0, r.zones[i]);

  static const uint8_t A[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00, 0x1A, 0x06, 0x0A,
                              0x07, 0x14, 0x0D, 0x00, 0x00, 0x78, 0x00, 0x78, 0x09, 0x12};
  TEST_ASSERT_TRUE(decode_inline_0307(A, sizeof(A), &r));
  TEST_ASSERT_EQUAL_UINT8(20, r.minute);
  TEST_ASSERT_EQUAL_UINT8(13, r.second);
  TEST_ASSERT_EQUAL_UINT16(120, r.valid_duration_s);
}

void test_decode_inline_0307_rejects() {
  SessionRecord r;
  // Nonzero record count: a real stream header, not the inline form.
  static const uint8_t STREAM[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x20, 0x1A, 0x06, 0x0A,
                                   0x07, 0x22, 0x2B, 0x00, 0x00, 0x78, 0x00, 0x44, 0x05, 0x16};
  TEST_ASSERT_FALSE(decode_inline_0307(STREAM, sizeof(STREAM), &r));
  // Too short to carry the fixed record head.
  static const uint8_t SHORT_F[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00, 0x1A, 0x06, 0x0A};
  TEST_ASSERT_FALSE(decode_inline_0307(SHORT_F, sizeof(SHORT_F), &r));
  // All-zero date: device holds no session at all.
  static const uint8_t EMPTY_F[] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_FALSE(decode_inline_0307(EMPTY_F, sizeof(EMPTY_F), &r));
  TEST_ASSERT_FALSE(decode_inline_0307(nullptr, 20, &r));
}

// === Watermark-poisoning guards ===

void test_session_epoch_clamps_out_of_range() {
  // A far-future civil date overflows uint32 seconds and clamps to 0, so a bogus
  // record can never plant a dedup watermark above every real session.
  SessionRecord r{};
  r.year = 2200;
  r.month = 1;
  r.day = 1;
  TEST_ASSERT_EQUAL_UINT32(0u, session_record_epoch(r));
  // A pre-1970 date is negative seconds; also clamped to 0.
  SessionRecord p{};
  p.year = 1969;
  p.month = 12;
  p.day = 31;
  p.hour = 23;
  TEST_ASSERT_EQUAL_UINT32(0u, session_record_epoch(p));
}

void test_session_epoch_plausible() {
  const int64_t now = 1780000000;  // 2026, same civil-as-UTC basis
  SessionClocks const node = node_only(now);
  TEST_ASSERT_FALSE(session_epoch_plausible(now + 7 * 86400, node));
  TEST_ASSERT_FALSE(session_epoch_plausible(now + 86400 + 1, node));
  // UTC+8 factory clock read by a node in UTC+2 (+6 h) and in UTC-12 (+20 h)
  TEST_ASSERT_TRUE(session_epoch_plausible(now + 6 * 3600, node));
  TEST_ASSERT_TRUE(session_epoch_plausible(now + 20 * 3600, node));
  // Within the margin, exactly now, and any past date: plausible.
  TEST_ASSERT_TRUE(session_epoch_plausible(now + 3600, node));
  TEST_ASSERT_TRUE(session_epoch_plausible((uint32_t)now, node));
  TEST_ASSERT_TRUE(session_epoch_plausible((uint32_t)(now - 100 * 86400), node));
  // No synced clock (now <= 0): cannot judge, so accept.
  TEST_ASSERT_TRUE(session_epoch_plausible(now + 7 * 86400, node_only(0)));
}

void test_session_epoch_plausible_brush_clock() {
  const int64_t now = 1780000000;
  SessionClocks const ahead{.node_local = now, .brush = now + (6 * 3600)};
  TEST_ASSERT_TRUE(session_epoch_plausible(now + 5 * 3600, ahead));
  TEST_ASSERT_FALSE(session_epoch_plausible(now + 6 * 3600 + SESSION_BRUSH_CLOCK_MARGIN_S + 1, ahead));
  // set to local time: a record still 6 h ahead predates the set
  SessionClocks const set{.node_local = now, .brush = now};
  TEST_ASSERT_FALSE(session_epoch_plausible(now + 6 * 3600, set));
  TEST_ASSERT_TRUE(session_epoch_plausible(now + SESSION_BRUSH_CLOCK_MARGIN_S, set));
  // a clock lost to 2000 judges nothing, the node bound still does
  SessionClocks const lost{.node_local = now, .brush = civil_to_epoch(2000, 1, 1, 0, 0, 0)};
  TEST_ASSERT_TRUE(session_epoch_plausible(now - 3600, lost));
  TEST_ASSERT_FALSE(session_epoch_plausible(now + 2 * 86400, lost));
  // a brush clock agreeing with a far date does not lift the node bound
  SessionClocks const far{.node_local = now, .brush = now + (30 * 86400)};
  TEST_ASSERT_FALSE(session_epoch_plausible(now + 29 * 86400, far));
  SessionClocks const brush_only{.node_local = 0, .brush = now};
  TEST_ASSERT_FALSE(session_epoch_plausible(now + 3600, brush_only));
}

void test_clamp_session_duration() {
  // 120 s is the standard program; the bound itself must survive the clamp
  TEST_ASSERT_EQUAL_UINT16(0, clamp_session_duration(0));
  TEST_ASSERT_EQUAL_UINT16(120, clamp_session_duration(120));
  TEST_ASSERT_EQUAL_UINT16(SESSION_MAX_DURATION_S, clamp_session_duration(SESSION_MAX_DURATION_S));
  TEST_ASSERT_EQUAL_UINT16(SESSION_MAX_DURATION_S, clamp_session_duration(SESSION_MAX_DURATION_S + 1));
  TEST_ASSERT_EQUAL_UINT16(SESSION_MAX_DURATION_S, clamp_session_duration(65535));
}

void test_clamp_head_counter() {
  // 1223 is the largest counter seen on hardware, so it must pass untouched
  TEST_ASSERT_EQUAL_UINT16(0, clamp_head_counter(0));
  TEST_ASSERT_EQUAL_UINT16(1223, clamp_head_counter(1223));
  TEST_ASSERT_EQUAL_UINT16(SETTINGS_HEAD_COUNTER_MAX, clamp_head_counter(SETTINGS_HEAD_COUNTER_MAX));
  TEST_ASSERT_EQUAL_UINT16(SETTINGS_HEAD_COUNTER_MAX, clamp_head_counter(SETTINGS_HEAD_COUNTER_MAX + 1));
  TEST_ASSERT_EQUAL_UINT16(SETTINGS_HEAD_COUNTER_MAX, clamp_head_counter(65535));
}

void test_clamp_clock_drift() {
  TEST_ASSERT_EQUAL_INT64(0, clamp_clock_drift(0));
  TEST_ASSERT_EQUAL_INT64(-21, clamp_clock_drift(-21));
  TEST_ASSERT_EQUAL_INT64(3600, clamp_clock_drift(3600));
  TEST_ASSERT_EQUAL_INT64(CLOCK_DRIFT_CLAMP_S, clamp_clock_drift(CLOCK_DRIFT_CLAMP_S));
  TEST_ASSERT_EQUAL_INT64(-CLOCK_DRIFT_CLAMP_S, clamp_clock_drift(-CLOCK_DRIFT_CLAMP_S));
  // a year byte of 0xFF is ~7.2e9 s out: saturates, sign kept
  TEST_ASSERT_EQUAL_INT64(CLOCK_DRIFT_CLAMP_S, clamp_clock_drift(7200000000LL));
  TEST_ASSERT_EQUAL_INT64(-CLOCK_DRIFT_CLAMP_S, clamp_clock_drift(-820000000LL));
}

// === SessionAssembler edge branches ===

void test_assembler_oversized_final_packet_truncates() {
  SessionAssembler asm_;
  asm_.reset();
  // count=1 needs 42 bytes; a first packet with 60 inline bytes overshoots, and
  // append_ clamps to the exact record boundary.
  uint8_t pkt[7 + 60] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x01};
  pkt[7] = 0x1A;  // year byte, plausible
  pkt[8] = 0x06;
  pkt[9] = 0x05;
  TEST_ASSERT_TRUE(asm_.feed(pkt, sizeof(pkt)));
  TEST_ASSERT_TRUE(asm_.complete());
  TEST_ASSERT_EQUAL_UINT16(1, asm_.record_count());
  SessionRecord r;
  TEST_ASSERT_TRUE(asm_.record(0, &r));
  TEST_ASSERT_FALSE(asm_.record(1, &r));  // no phantom second record
}

void test_assembler_feed_after_complete_ignored() {
  SessionAssembler asm_;
  asm_.reset();
  uint8_t pkt[7 + 42] = {0x03, 0x07, 0x2A, 0x42, 0x23, 0x00, 0x01};
  TEST_ASSERT_TRUE(asm_.feed(pkt, sizeof(pkt)));
  uint8_t extra[10] = {0};
  TEST_ASSERT_TRUE(asm_.feed(extra, sizeof(extra)));  // already complete, ignored
  TEST_ASSERT_EQUAL_UINT16(1, asm_.record_count());
}

void test_assembler_short_first_packet_rejected() {
  SessionAssembler asm_;
  asm_.reset();
  const uint8_t shortp[] = {0x03, 0x07, 0x2A};  // fewer than the 7 header bytes
  TEST_ASSERT_FALSE(asm_.feed(shortp, sizeof(shortp)));
  TEST_ASSERT_TRUE(asm_.failed());
}

void test_assembler_null_feed_rejected() {
  // A notify can arrive with value_len 0 and a null value pointer; the same
  // guard the DIS reader needs applies to the stream reassembler.
  SessionAssembler asm_;
  asm_.reset();
  TEST_ASSERT_FALSE(asm_.feed(nullptr, 7));
  TEST_ASSERT_TRUE(asm_.failed());
}

// === SettingsAssembler edge branches ===

void test_settings_null_feed_rejected() {
  SettingsAssembler s;
  s.reset();
  TEST_ASSERT_FALSE(s.feed(nullptr, 20));
  TEST_ASSERT_FALSE(s.has_start());
  TEST_ASSERT_FALSE(s.has_cont());
}

void test_settings_cont_then_start_completes() {
  SettingsAssembler s;
  s.reset();
  uint8_t cont[20] = {0x03, 0x02};  // not the 23 24 start prefix
  TEST_ASSERT_FALSE(s.feed(cont, sizeof(cont)));
  TEST_ASSERT_TRUE(s.has_cont());
  TEST_ASSERT_FALSE(s.complete());
  uint8_t start[20] = {0x03, 0x02, 0x23, 0x24};
  TEST_ASSERT_TRUE(s.feed(start, sizeof(start)));  // either order completes
  TEST_ASSERT_TRUE(s.complete());
}

void test_settings_non_0302_frame_rejected() {
  SettingsAssembler s;
  s.reset();
  uint8_t bad[20] = {0x03, 0x03};  // wrong opcode
  TEST_ASSERT_FALSE(s.feed(bad, sizeof(bad)));
  TEST_ASSERT_FALSE(s.has_start());
  TEST_ASSERT_FALSE(s.has_cont());
}

void test_settings_duplicate_start_overwrites() {
  SettingsAssembler s;
  s.reset();
  uint8_t start[20] = {0x03, 0x02, 0x23, 0x24};
  start[4] = 0x11;  // -> buffer[0]
  TEST_ASSERT_FALSE(s.feed(start, sizeof(start)));
  start[4] = 0x22;
  TEST_ASSERT_FALSE(s.feed(start, sizeof(start)));  // overwrites, still no cont
  TEST_ASSERT_EQUAL_UINT8(0x22, s.buffer()[0]);
  TEST_ASSERT_FALSE(s.complete());
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_parse_battery_valid);
  RUN_TEST(test_parse_battery_zero);
  RUN_TEST(test_parse_battery_full);
  RUN_TEST(test_parse_battery_over_100_rejected);
  RUN_TEST(test_parse_battery_empty_rejected);
  RUN_TEST(test_parse_battery_multibyte_takes_first);

  RUN_TEST(test_u16be_basic);

  RUN_TEST(test_assembler_rejects_wrong_magic);

  RUN_TEST(test_decode_record_normal);
  RUN_TEST(test_decode_record_quadrants_follow_zone_pairs);
  RUN_TEST(test_decode_record_gesture_zones_offset23);
  RUN_TEST(test_decode_record_aborted);
  RUN_TEST(test_decode_record_no_score);
  RUN_TEST(test_decode_record_null_rejected);
  RUN_TEST(test_record_newer_compares_timestamp);
  RUN_TEST(test_session_epoch_known_date);
  RUN_TEST(test_session_epoch_second_date);
  RUN_TEST(test_session_epoch_orders_like_newer);

  RUN_TEST(test_assembler_fragmented_stream);
  RUN_TEST(test_assembler_newest_index);
  RUN_TEST(test_assembler_out_of_range_record);
  RUN_TEST(test_assembler_bad_header_rejected);
  RUN_TEST(test_assembler_zero_count_rejected);
  RUN_TEST(test_assembler_overlong_count_rejected);

  RUN_TEST(test_parse_status_response_valid);
  RUN_TEST(test_parse_status_voltage_six_byte_frame);
  RUN_TEST(test_status_dock_predicates);
  RUN_TEST(test_parse_status_response_bad_header_rejected);
  RUN_TEST(test_parse_status_response_over_100_rejected);
  RUN_TEST(test_parse_status_response_short_rejected);
  RUN_TEST(test_parse_settings_clock_valid);
  RUN_TEST(test_parse_settings_clock_bad_header_rejected);
  RUN_TEST(test_parse_settings_clock_out_of_range_rejected);

  RUN_TEST(test_settings_continuation_brush_a);
  RUN_TEST(test_settings_clock_invalid_fields_rejected);
  RUN_TEST(test_settings_continuation_brush_b);
  RUN_TEST(test_settings_two_frame_complete);
  RUN_TEST(test_settings_short_frame_rejected);
  RUN_TEST(test_settings_start_fields_brush_b);
  RUN_TEST(test_settings_two_frame_full_brush_a);

  RUN_TEST(test_brush_areas_push_y3p_valid);
  RUN_TEST(test_brush_areas_push_t1_prefix);
  RUN_TEST(test_brush_areas_push_bad_prefix_rejected);
  RUN_TEST(test_brush_areas_push_short_rejected);

  RUN_TEST(test_raw_record_matches_input);

  RUN_TEST(test_encode_scheme_gear_table);
  RUN_TEST(test_build_scheme_single_frame);
  RUN_TEST(test_build_scheme_split_frames);
  RUN_TEST(test_build_scheme_split_shortest_second_frame);
  RUN_TEST(test_build_scheme_rejects_bad_step_count);

  RUN_TEST(test_build_toggle_default_on_off);
  RUN_TEST(test_build_toggle_brush_mode_off_sentinel);
  RUN_TEST(test_build_language_command);
  RUN_TEST(test_build_set_cloud_host_command);
  RUN_TEST(test_build_set_cloud_host_command_empty);
  RUN_TEST(test_build_set_cloud_host_command_clamps_over_limit);
  RUN_TEST(test_build_blufi_frame_control);
  RUN_TEST(test_build_blufi_frame_data);
  RUN_TEST(test_parse_blufi_wifi_status);
  RUN_TEST(test_cloud_body_field);
  RUN_TEST(test_parse_hex_bytes);
  RUN_TEST(test_parse_mac_u64);
  RUN_TEST(test_build_birthday_command);
  RUN_TEST(test_sync_fingerprint);
  RUN_TEST(test_cloud_host_matches);
  RUN_TEST(test_brush_weather_code);
  RUN_TEST(test_parse_iso8601_epoch);
  RUN_TEST(test_pick_weather_today_then_tomorrow);
  RUN_TEST(test_pick_weather_falls_back);
  RUN_TEST(test_build_weather_reply);
  RUN_TEST(test_decode_cloud_brushdata_record);
  RUN_TEST(test_decode_v20_record_zones_real);
  RUN_TEST(test_timezone_index_to_string);
  RUN_TEST(test_tz_index_for_offset_seconds);
  RUN_TEST(test_tz_index_offset_seconds_round_trips);
  RUN_TEST(test_session_utc_offset_prefers_the_record_zone);
  RUN_TEST(test_max_language_id_per_model);
  RUN_TEST(test_is_write_refusal);

  RUN_TEST(test_build_set_clock_summer);
  RUN_TEST(test_build_set_clock_winter_sunday_midnight_fields);
  RUN_TEST(test_build_set_clock_before_2000_clamps_year_byte);

  RUN_TEST(test_civil_to_epoch_matches_record_epoch);
  RUN_TEST(test_civil_to_epoch_january_february_branch);
  RUN_TEST(test_civil_to_epoch_leap_day);
  RUN_TEST(test_should_resync_below_threshold);
  RUN_TEST(test_should_resync_above_threshold_either_sign);
  RUN_TEST(test_should_resync_exact_threshold_not_exceeded);
  RUN_TEST(test_should_resync_zero_threshold_syncs_on_any_diff);
  RUN_TEST(test_should_hold_link_requires_every_condition);
  RUN_TEST(test_plan_poll_tick_polls_when_due);
  RUN_TEST(test_plan_poll_tick_skip_reasons_are_ordered);
  RUN_TEST(test_plan_poll_tick_off_dock_waits_for_slow_interval);
  RUN_TEST(test_plan_poll_tick_boot_stagger_defers_once);
  RUN_TEST(test_plan_poll_tick_pending_poll_bypasses_cadence);
  RUN_TEST(test_plan_ingest_sorts_new_records_oldest_first);
  RUN_TEST(test_plan_ingest_skips_at_or_below_watermark);
  RUN_TEST(test_plan_ingest_future_record_never_moves_watermark);
  RUN_TEST(test_plan_ingest_persists_only_strictly_newer);
  RUN_TEST(test_plan_ingest_unsynced_clock_cannot_judge);
  RUN_TEST(test_plan_ingest_picks_newest_itself);
  RUN_TEST(test_confirm_sessions_only_with_nothing_held_back);
  RUN_TEST(test_plan_ingest_empty_ring_is_inert);
  RUN_TEST(test_accept_inline_only_when_strictly_newer);
  RUN_TEST(test_accept_inline_rejects_implausible_future);
  RUN_TEST(test_accept_inline_never_republishes_the_shown_session);
  RUN_TEST(test_plan_ingest_judges_by_brush_clock);
  RUN_TEST(test_same_session_ignores_fields_a_head_lacks);
  RUN_TEST(test_clock_set_shift_and_confirmation);
  RUN_TEST(test_shift_epoch_back);
  RUN_TEST(test_restored_newest_epoch);
  RUN_TEST(test_set_clock_command_epoch);
  RUN_TEST(test_settings_query_per_profile);
  RUN_TEST(test_coverage_percent_is_whole_and_bounded);
  RUN_TEST(test_coverage_percent_zero_duration_is_nan);
  RUN_TEST(test_poll_is_due_charging_uses_fast_interval);
  RUN_TEST(test_poll_is_due_off_dock_uses_slow_interval);

  RUN_TEST(test_profile_select_y3p);
  RUN_TEST(test_profile_select_y3pd);
  RUN_TEST(test_profile_select_prefix_order);
  RUN_TEST(test_profile_select_unknown_fallback);
  RUN_TEST(test_profile_select_wifi_oos);
  RUN_TEST(test_profile_select_legacy_after_specific);
  RUN_TEST(test_type1_routing);
  RUN_TEST(test_record_decode_type1_known);
  RUN_TEST(test_record_decode_unknown_is_null);
  RUN_TEST(test_profile_unknown_contract);
  RUN_TEST(test_profile_unknown_query_status_only);
  RUN_TEST(test_profile_type1_contract);
  RUN_TEST(test_profile_select_y5_z1);
  RUN_TEST(test_profile_z1_contract);
  RUN_TEST(test_profile_z1_routing);
  RUN_TEST(test_profile_select_v20);
  RUN_TEST(test_profile_v20_family_keeps_the_store);
  RUN_TEST(test_profile_v20_contract);
  RUN_TEST(test_profile_entity_model);
  RUN_TEST(test_validated_profiles_keep_writes_and_cccd_skip);
  RUN_TEST(test_clock_write_permitted);
  RUN_TEST(test_kept_values_only_on_the_x_ultra_20_family);
  RUN_TEST(test_command_permitted_read_only_allows_only_profile_reads);
  RUN_TEST(test_command_permitted_v20_passes_only_listed_writes);
  RUN_TEST(test_command_permitted_v20_extra_reads);
  RUN_TEST(test_voice_prompts_command);
  RUN_TEST(test_parse_status_reply);
  RUN_TEST(test_v20_settings_real_frames);
  RUN_TEST(test_v20_settings_after_clock_write);
  RUN_TEST(test_v20_inline_0307_real_frame);
  RUN_TEST(test_v20_inline_against_brush_clock);
  RUN_TEST(test_v20_brushed_seconds_from_length);
  RUN_TEST(test_v20_record_carries_its_time_zone);
  RUN_TEST(test_should_clear_inline_only_on_the_dock);
  RUN_TEST(test_is_bare_ack);
  RUN_TEST(test_voice_prompt_flags_need_the_main_one);
  RUN_TEST(test_v20_assembler_splits_length_prefixed_records);
  RUN_TEST(test_v20_assembler_header_cases);
  RUN_TEST(test_v20_assembler_stops_at_a_bad_length);
  RUN_TEST(test_command_permitted_unlocked_type1_unchanged);
  RUN_TEST(test_decode_hw_revision_code);
  RUN_TEST(test_hw_revision_text);
  RUN_TEST(test_dis_printable_text);
  RUN_TEST(test_gatt_props_text);
  RUN_TEST(test_ble_uuid_text);
  RUN_TEST(test_decode_inline_0307);
  RUN_TEST(test_decode_inline_0307_rejects);

  RUN_TEST(test_session_epoch_clamps_out_of_range);
  RUN_TEST(test_session_epoch_plausible);
  RUN_TEST(test_session_epoch_plausible_brush_clock);
  RUN_TEST(test_clamp_session_duration);
  RUN_TEST(test_clamp_head_counter);
  RUN_TEST(test_clamp_clock_drift);
  RUN_TEST(test_assembler_oversized_final_packet_truncates);
  RUN_TEST(test_assembler_feed_after_complete_ignored);
  RUN_TEST(test_assembler_short_first_packet_rejected);
  RUN_TEST(test_assembler_null_feed_rejected);
  RUN_TEST(test_settings_cont_then_start_completes);
  RUN_TEST(test_settings_non_0302_frame_rejected);
  RUN_TEST(test_settings_null_feed_rejected);
  RUN_TEST(test_settings_duplicate_start_overwrites);

  return UNITY_END();
}
