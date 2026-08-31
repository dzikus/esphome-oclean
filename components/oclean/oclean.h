#pragma once

#include <map>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#ifdef USE_ESP32
#ifdef USE_API
#include "esphome/components/api/custom_api_device.h"
#endif
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#include <esp_gattc_api.h>

#include "oclean_profile.h"
#include "oclean_protocol.h"

namespace esphome::oclean {

namespace espbt = esphome::esp32_ble_tracker;

// one per newly downloaded session, for automations that do not want to go
// through the Home Assistant event
class OcleanSessionTrigger : public Trigger<const SessionRecord &> {};

class OcleanCaptureButton;
class OcleanCommandSwitch;
class OcleanSchemeSelect;
class OcleanLanguageSelect;
class OcleanHeadDaysNumber;
class OcleanSyncTimeButton;

// Connect-poll-disconnect: the brush streams nothing live, it buffers sessions
// and hands them over on request, so the link stays down between polls and the
// brush battery is spared.
class OcleanHub : public ble_client::BLEClientNode,
                  public PollingComponent
#ifdef USE_API
    ,
                  public esphome::api::CustomAPIDevice
#endif
{
 public:
  void setup() override;
  void loop() override {}
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void add_on_session_trigger(OcleanSessionTrigger *t) { this->session_triggers_.push_back(t); }

  void set_hub_index(int i) { this->hub_index_ = i; }
  void set_total_hubs(int n) { this->total_hubs_ = n; }
  void set_expose_dev_sensors(bool en) { this->expose_dev_sensors_ = en; }

  // Identically named auto-created entities share an object-id hash, so without
  // a per-hub salt two hubs collide in the same flash slot. Also the base for
  // the session preference keys.
  uint32_t pref_salt() const {
    uint64_t const addr = this->parent_ != nullptr ? this->parent_->get_address() : 0;
    return (uint32_t)addr ^ (uint32_t)(addr >> 32);
  }

#ifdef USE_TIME
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
#endif
  void set_tz_index(uint8_t tz) { this->tz_index_ = tz; }

  void set_auto_sync_time(bool en) { this->auto_sync_time_ = en; }
  void set_sync_drift_threshold(uint32_t seconds) { this->sync_drift_threshold_s_ = seconds; }

  // Off drops an in-flight cycle and any queued write, freeing the brush for
  // the official app.
  void set_ble_user_enabled(bool en);

  void set_adaptive_poll(bool en) { this->adaptive_poll_ = en; }
  void set_charging_interval(uint32_t ms) { this->charging_interval_ms_ = ms; }
  void set_battery_interval(uint32_t ms) { this->battery_interval_ms_ = ms; }

  void set_hold_connection_while_docked(bool en) { this->hold_while_docked_ = en; }

  void set_battery_sensor(sensor::Sensor *s) { this->battery_sensor_ = s; }
  void set_model_text_sensor(text_sensor::TextSensor *s) { this->model_text_sensor_ = s; }
  void set_hw_revision_text_sensor(text_sensor::TextSensor *s) { this->hw_rev_text_sensor_ = s; }
  void set_sw_version_text_sensor(text_sensor::TextSensor *s) { this->sw_rev_text_sensor_ = s; }
  void set_charging_binary_sensor(binary_sensor::BinarySensor *s) { this->charging_binary_sensor_ = s; }
  void set_docked_binary_sensor(binary_sensor::BinarySensor *s) { this->docked_binary_sensor_ = s; }
  void set_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->connected_binary_sensor_ = s; }
  void set_capture_button(OcleanCaptureButton *b) { this->capture_button_ = b; }

  // fed from the newest decoded record
  void set_session_score_sensor(sensor::Sensor *s) { this->session_score_sensor_ = s; }
  void set_session_duration_sensor(sensor::Sensor *s) { this->session_duration_sensor_ = s; }
  void set_session_valid_duration_sensor(sensor::Sensor *s) { this->session_valid_duration_sensor_ = s; }
  void set_session_mode_text_sensor(text_sensor::TextSensor *s) { this->session_mode_text_sensor_ = s; }
  void set_session_coverage_sensor(sensor::Sensor *s) { this->session_coverage_sensor_ = s; }
  void set_gesture_zone_sensor(int i, sensor::Sensor *s) {
    if (i >= 0 && i < (int)SESSION_ZONES_COUNT)
      this->zone_sensors_[i] = s;
  }
  void set_session_time_text_sensor(text_sensor::TextSensor *s) { this->session_time_text_sensor_ = s; }
  void set_device_clock_text_sensor(text_sensor::TextSensor *s) { this->device_clock_text_sensor_ = s; }
  void set_mac_text_sensor(text_sensor::TextSensor *s) { this->mac_text_sensor_ = s; }
  void set_last_seen_text_sensor(text_sensor::TextSensor *s) { this->last_seen_text_sensor_ = s; }

  // Brush-head usage counters read back from the settings buffer.
  void set_head_used_days_sensor(sensor::Sensor *s) { this->head_used_days_sensor_ = s; }
  void set_head_used_times_sensor(sensor::Sensor *s) { this->head_used_times_sensor_ = s; }

  // binary sensors, not switches: the device rejects writes to these
  void set_volume_enabled_binary_sensor(binary_sensor::BinarySensor *s) { this->volume_enabled_binary_sensor_ = s; }
  void set_calendar_enabled_binary_sensor(binary_sensor::BinarySensor *s) { this->calendar_enabled_binary_sensor_ = s; }
  void set_splash_prevent_binary_sensor(binary_sensor::BinarySensor *s) { this->splash_prevent_binary_sensor_ = s; }
  void set_fill_brush_binary_sensor(binary_sensor::BinarySensor *s) { this->fill_brush_binary_sensor_ = s; }
  void set_auto_mode_binary_sensor(binary_sensor::BinarySensor *s) { this->auto_mode_binary_sensor_ = s; }

  // Settings-buffer scalar fields (raw indices and a usage counter).
  void set_device_theme_sensor(sensor::Sensor *s) { this->device_theme_sensor_ = s; }
  void set_volume_index_sensor(sensor::Sensor *s) { this->volume_index_sensor_ = s; }
  void set_head_used_time_sensor(sensor::Sensor *s) { this->head_used_time_sensor_ = s; }
  void set_clock_drift_sensor(sensor::Sensor *s) { this->clock_drift_sensor_ = s; }
  void set_timezone_text_sensor(text_sensor::TextSensor *s) { this->timezone_text_sensor_ = s; }
  // The head-replacement number doubles as a readback target for buffer 25-26.
  void set_head_max_number(OcleanHeadDaysNumber *n) { this->head_max_number_ = n; }

  // back-pointers, so the settings readback can correct each optimistic publish
  void set_area_reminder_switch(OcleanCommandSwitch *s) { this->area_reminder_switch_ = s; }
  void set_over_pressure_switch(OcleanCommandSwitch *s) { this->over_pressure_switch_ = s; }
  void set_brush_pause_switch(OcleanCommandSwitch *s) { this->brush_pause_switch_ = s; }
  void set_raise_wake_switch(OcleanCommandSwitch *s) { this->raise_wake_switch_ = s; }
  void set_brush_mode_switch(OcleanCommandSwitch *s) { this->brush_mode_switch_ = s; }
  void set_scheme_select(OcleanSchemeSelect *s) { this->scheme_select_ = s; }
  void set_language_select(OcleanLanguageSelect *s) { this->language_select_ = s; }

  // resend reprograms the brush, debounced, but only while custom is selected
  void set_custom_scheme_param(uint8_t kind, uint8_t index, uint8_t value, bool resend);

  // arms a flag when the link is down, so the command still fires once
  // discovery completes
  void trigger_session_capture();

  // bypasses the adaptive off-dock gate
  void trigger_immediate_poll();

  // a clock write is rebuilt at flush time, so the queue carries the kind
  // instead of switching on the log label
  enum class WriteKind : uint8_t {
    PLAIN,
    CLOCK,
  };

  // bytes are the whole command, opcode included; they flush at the start of the
  // next query window. name labels the log line and must outlive the queue entry
  // (a literal). False means nothing was queued (BLE off or queue full), so the
  // caller must skip its optimistic publish.
  bool send_command(std::vector<uint8_t> bytes, const char *name, WriteKind kind = WriteKind::PLAIN);

  // Warns and writes nothing until the local clock is synced. A mutation, so it
  // runs on a button press only, never on boot or a poll.
  void sync_clock();

 protected:
  // per-cycle flags plus the watchdog, for a link that is already up.
  // stamp_cadence = count this as a real poll; write and capture cycles do not
  void arm_cycle_(bool stamp_cadence);

  // link down to up, then arm the cycle
  void begin_connect_cycle_(bool stamp_cadence);

  // query window open: a record stream may be in flight, and a second round
  // would reset the assemblers mid-transfer
  bool query_round_open_() const { return this->capture_active_; }

  // caller guarantees a valid local time; reason labels the log line
  void queue_set_clock_(const char *reason);

  // A queued sync-clock can wait a whole off-dock interval, so the flush path
  // rebuilds the bytes here at write time rather than sending a stale sample.
  bool build_clock_command_(std::vector<uint8_t> *out);

  // node offset, DST-aware; falls back to tz_index_ when it has no table entry
  uint8_t effective_tz_index_();

  // No-op unless the drift exceeds the threshold, and never stacks a second
  // write while one is queued.
  void maybe_auto_sync_clock_(const DeviceSettings &ds);

  // reports drift even when auto-correction is off
  void publish_clock_drift_(const DeviceSettings &ds);

  // Civil-as-UTC, the same basis as session_record_epoch; 0 when unsynced, which
  // the plausibility check reads as "cannot judge".
  int64_t local_now_epoch_();

  // capture_mode only lengthens the hold, for late or pushed replies
  void query_device_(bool capture_mode);

  enum class State {
    IDLE,
    CONNECTING,
    DISCOVERING,
    POLLING,
    DISCONNECTING,
  };

  void set_state_(State s);
  static const char *state_name_(State s);

  // must run inside SEARCH_CMPL: a deferred characteristic lookup returns
  // nullptr for everything
  void resolve_handles_();
  void read_handle_(uint16_t handle, const char *name);
  void register_notify_handle_(uint16_t handle, const char *name);
  bool write_raw_(uint16_t handle, const uint8_t *bytes, size_t len, const char *name);
  // Staggered: Write With Response allows one outstanding write. Returns the ms
  // offset at which the read queries may start without colliding.
  uint32_t flush_pending_writes_();

  void handle_dis_read_(uint16_t uuid16, const uint8_t *data, size_t len);
  void handle_battery_(const uint8_t *data, size_t len);
  void handle_session_notify_(const uint8_t *data, size_t len);
  // A partial record (inline fragment) has no zones or score; those entities go
  // unknown rather than keep an older session's values under a newer timestamp.
  void publish_session_record_(const SessionRecord &r, bool partial);
  // One record per loop iteration: same-entity publishes inside one iteration
  // coalesce, so a backfilled ring would collapse to a single recorder row.
  void schedule_next_session_publish_();
  // ts is the ordering/dedup epoch, not the timestamp sent to Home Assistant
  void emit_session_event_(const SessionRecord &r, uint32_t ts);
  void handle_main_notify_(const uint8_t *data, size_t len);
  // notify on the session channel after the stream completed: enrichment push
  void handle_enrichment_notify_(const uint8_t *data, size_t len);

  void maybe_finish_poll_();
  void disconnect_();
  void start_watchdog_();

  // Ends a query window: holds the link only while docked with BLE enabled,
  // otherwise disconnects. Both window-end paths funnel through here.
  void finish_or_hold_(const char *reason);
  void enter_hold_();
  // one re-query round on the live link, under a per-round watchdog
  void held_requery_();
  void clear_hold_();

  // RFC3339 stamp taken when a poll reaches the brush, so a stale value flags an
  // unreachable brush even when no reading changed.
  void publish_last_seen_();

  // null-safe publish: entities are only created when configured, so every
  // publish site would otherwise repeat the same guard
  static void publish_(sensor::Sensor *s, float value);
  static void publish_(binary_sensor::BinarySensor *s, bool value);
  static void publish_(text_sensor::TextSensor *s, const std::string &value);

  sensor::Sensor *battery_sensor_{nullptr};
  text_sensor::TextSensor *model_text_sensor_{nullptr};
  text_sensor::TextSensor *hw_rev_text_sensor_{nullptr};
  text_sensor::TextSensor *sw_rev_text_sensor_{nullptr};
  binary_sensor::BinarySensor *charging_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *docked_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_binary_sensor_{nullptr};
  OcleanCaptureButton *capture_button_{nullptr};

  sensor::Sensor *session_score_sensor_{nullptr};
  sensor::Sensor *session_duration_sensor_{nullptr};
  sensor::Sensor *session_valid_duration_sensor_{nullptr};
  text_sensor::TextSensor *session_mode_text_sensor_{nullptr};
  sensor::Sensor *session_coverage_sensor_{nullptr};
  sensor::Sensor *zone_sensors_[SESSION_ZONES_COUNT]{};
  text_sensor::TextSensor *session_time_text_sensor_{nullptr};
  text_sensor::TextSensor *device_clock_text_sensor_{nullptr};
  text_sensor::TextSensor *mac_text_sensor_{nullptr};
  text_sensor::TextSensor *last_seen_text_sensor_{nullptr};

  sensor::Sensor *head_used_days_sensor_{nullptr};
  sensor::Sensor *head_used_times_sensor_{nullptr};
  binary_sensor::BinarySensor *volume_enabled_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *calendar_enabled_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *splash_prevent_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *fill_brush_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *auto_mode_binary_sensor_{nullptr};
  sensor::Sensor *device_theme_sensor_{nullptr};
  sensor::Sensor *volume_index_sensor_{nullptr};
  sensor::Sensor *head_used_time_sensor_{nullptr};
  sensor::Sensor *clock_drift_sensor_{nullptr};
  text_sensor::TextSensor *timezone_text_sensor_{nullptr};
  OcleanHeadDaysNumber *head_max_number_{nullptr};
  OcleanCommandSwitch *area_reminder_switch_{nullptr};
  OcleanCommandSwitch *over_pressure_switch_{nullptr};
  OcleanCommandSwitch *brush_pause_switch_{nullptr};
  OcleanCommandSwitch *raise_wake_switch_{nullptr};
  OcleanCommandSwitch *brush_mode_switch_{nullptr};
  OcleanSchemeSelect *scheme_select_{nullptr};
  OcleanLanguageSelect *language_select_{nullptr};

  State state_{State::IDLE};
  bool expose_dev_sensors_{false};

  // Handles resolved at SEARCH_CMPL for the characteristics this hub uses.
  uint16_t battery_handle_{0};
  uint16_t model_handle_{0};
  uint16_t hw_rev_handle_{0};
  uint16_t sw_rev_handle_{0};
  uint16_t rx_main_handle_{0};
  uint16_t rx_session_handle_{0};
  uint16_t tx_session_handle_{0};
  uint16_t tx_main_handle_{0};

  // Per-cycle completion tracking. The poll is done once battery and DIS model
  // have been read (or their reads have failed) so the link can drop early.
  bool got_battery_{false};
  bool got_model_{false};

  // millis(); the staleness check subtracts, so a wrap works out
  bool dis_cached_{false};
  uint32_t last_dis_read_ms_{0};

  struct PendingWrite {
    std::vector<uint8_t> bytes;
    const char *name;
    WriteKind kind;
  };
  std::vector<PendingWrite> pending_writes_{};

  // epoch of the newest record already emitted; anything at or below it is
  // skipped, so a session fires its event once across reboots
  uint32_t last_session_emitted_{0};
  esphome::ESPPreferenceObject session_wm_pref_;
  // The preference layer takes a blob on its stored length alone, so an older
  // layout has to be rejected by size, not by inspection: this one runs two
  // bytes longer than the {record, flag} blob it replaced. Magic and version are
  // the second line. Change both the size and the version on any layout change
  // here or in SessionRecord.
  static constexpr uint16_t PERSISTED_SESSION_MAGIC = 0x0C1E;
  static constexpr uint8_t PERSISTED_SESSION_VERSION = 1;
  // partial marks an inline fragment, which carries no zones or score
  struct PersistedSession {
    uint16_t magic;
    uint8_t version;
    uint8_t partial;
    SessionRecord record;
  };
  static_assert(sizeof(PersistedSession) > sizeof(SessionRecord) + 2,
                "PersistedSession must not share a size with the {record, flag} "
                "layout it replaced, or the length check would accept it");
  esphome::ESPPreferenceObject session_last_pref_;
  // gates the inline fragment: a full record must never downgrade to a partial
  uint32_t newest_record_epoch_{0};
  // oldest-first, drained one per loop iteration
  std::vector<SessionRecord> pending_session_publish_{};
  std::vector<OcleanSessionTrigger *> session_triggers_{};

  // reset each query round
  uint32_t notify_count_this_round_{0};
  bool notify_flood_warned_{false};

  // One-shot boot-stagger latch. Latched so a millis() wrap (which resets the
  // stagger window) cannot re-defer an already-running hub.
  bool boot_stagger_done_{false};

  // Session-capture dev hook.
  bool capture_armed_{false};
  bool capture_active_{false};
  // Reassembles the *B# record stream that arrives on the session notify
  // characteristic during a capture window.
  SessionAssembler session_asm_{};
  // Reassembles the two-frame settings response on the main notify char.
  SettingsAssembler settings_asm_{};

  std::string model_string_{};
  // cached so the DIS-cache path can republish without re-reading
  std::string hw_rev_string_{};
  std::string sw_rev_string_{};

  // TYPE1 until the model string says otherwise, so the first poll behaves as
  // the validated brushes do rather than as an unknown device
  const OcleanProfile *profile_{&PROFILE_TYPE1};

#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif
  // only reached when effective_tz_index_() cannot derive the node offset; the
  // wire value is 1-based into the device table (15 = UTC+1, 16 = UTC+2)
  uint8_t tz_index_{16};

  bool auto_sync_time_{false};
  uint32_t sync_drift_threshold_s_{120};

  bool ble_user_enabled_{true};

  // docked_last_ is STATUS byte2 0x01 or 0x03 and gates both cadence and hold;
  // charging_last_ is 0x01 only, for the HA charging sensor. last_poll_ms_ is
  // valid only while poll_pending_ is false: millis() can legitimately read 0
  // after the ~49.7 day wrap, so it cannot double as the never-polled sentinel.
  bool adaptive_poll_{false};
  uint32_t charging_interval_ms_{600000};
  uint32_t battery_interval_ms_{14400000};
  bool charging_last_{false};
  bool docked_last_{false};
  uint32_t last_poll_ms_{0};
  bool poll_pending_{true};

  // while holding_, state_ stays POLLING: the link is up and queries run on it,
  // so every state_==POLLING gate elsewhere stays correct
  bool hold_while_docked_{true};
  bool holding_{false};
  // Set when the current query round parses a STATUS reply; a held round that
  // stays silent must end in a disconnect, not another hold.
  bool round_status_seen_{false};

  int hub_index_{0};
  int total_hubs_{1};
};

}  // namespace esphome::oclean

#endif  // USE_ESP32
