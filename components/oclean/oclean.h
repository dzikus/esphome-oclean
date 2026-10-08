#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
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

#ifdef USE_OCLEAN_WEATHER
#include "esphome/core/string_ref.h"
namespace esphome::api {
class ActionResponse;
}  // namespace esphome::api
#endif

namespace esphome::oclean {

namespace espbt = esphome::esp32_ble_tracker;

// one per newly downloaded session, for automations that do not want to go
// through the Home Assistant event
class OcleanSessionTrigger : public Trigger<const SessionRecord &> {};

class OcleanCaptureButton;
class OcleanCommandSwitch;
class OcleanVoiceSwitch;
class OcleanSchemeSelect;
class OcleanLanguageSelect;
class OcleanHeadMaxNumber;
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
#ifdef USE_OCLEAN_CLOUD_RECEIVER
  void loop() override;
#endif
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void add_on_session_trigger(OcleanSessionTrigger *t) { this->session_triggers_.push_back(t); }

  void set_hub_index(int i) { this->hub_index_ = i; }
  void set_total_hubs(int n) { this->total_hubs_ = n; }
  void set_model(BrushModel model) {
    this->model_ = model;
    // DIS replaces it on the first poll; until then the write gate already
    // matches the brush the yaml names
    if (model == BrushModel::X_ULTRA_20)
      this->profile_ = &PROFILE_TYPE_V20;
  }
  void set_expose_dev_sensors(bool en) { this->expose_dev_sensors_ = en; }
  void set_read_only(bool en) { this->read_only_ = en; }

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
  void set_battery_voltage_sensor(sensor::Sensor *s) { this->battery_voltage_sensor_ = s; }
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
  void set_quadrant_sensor(int i, sensor::Sensor *s) {
    if (i >= 0 && i < (int)SESSION_QUADRANTS_COUNT)
      this->quadrant_sensors_[i] = s;
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
  // X Ultra 20 only; other models never publish them
  void set_auto_update_binary_sensor(binary_sensor::BinarySensor *s) { this->auto_update_binary_sensor_ = s; }
  void set_network_binary_sensor(binary_sensor::BinarySensor *s) { this->network_binary_sensor_ = s; }
  void set_voice_teaching_binary_sensor(binary_sensor::BinarySensor *s) { this->voice_teaching_binary_sensor_ = s; }
  void set_wifi_configured_binary_sensor(binary_sensor::BinarySensor *s) { this->wifi_configured_binary_sensor_ = s; }
  void set_area_guidance_binary_sensor(binary_sensor::BinarySensor *s) { this->area_guidance_binary_sensor_ = s; }
  void set_demo_mode_binary_sensor(binary_sensor::BinarySensor *s) { this->demo_mode_binary_sensor_ = s; }
  void set_device_mode_sensor(sensor::Sensor *s) { this->device_mode_sensor_ = s; }
  void set_mode_number_sensor(sensor::Sensor *s) { this->mode_number_sensor_ = s; }
  void set_running_state_sensor(sensor::Sensor *s) { this->running_state_sensor_ = s; }

  // Settings-buffer scalar fields (raw indices and a usage counter).
  void set_device_theme_sensor(sensor::Sensor *s) { this->device_theme_sensor_ = s; }
  void set_volume_index_sensor(sensor::Sensor *s) { this->volume_index_sensor_ = s; }
  void set_head_used_time_sensor(sensor::Sensor *s) { this->head_used_time_sensor_ = s; }
  void set_clock_drift_sensor(sensor::Sensor *s) { this->clock_drift_sensor_ = s; }
  void set_timezone_text_sensor(text_sensor::TextSensor *s) { this->timezone_text_sensor_ = s; }
  // The head time limit number doubles as a readback target for buffer 25-26.
  void set_head_max_number(OcleanHeadMaxNumber *n) { this->head_max_number_ = n; }

  // back-pointers, so the settings readback can correct each optimistic publish
  void set_area_reminder_switch(OcleanCommandSwitch *s) { this->area_reminder_switch_ = s; }
  void set_over_pressure_switch(OcleanCommandSwitch *s) { this->over_pressure_switch_ = s; }
  void set_brush_pause_switch(OcleanCommandSwitch *s) { this->brush_pause_switch_ = s; }
  void set_raise_wake_switch(OcleanCommandSwitch *s) { this->raise_wake_switch_ = s; }
  void set_brush_mode_switch(OcleanCommandSwitch *s) { this->brush_mode_switch_ = s; }
  void set_auto_mode_switch(OcleanCommandSwitch *s) { this->auto_mode_switch_ = s; }
  void set_festival_reminder_switch(OcleanCommandSwitch *s) { this->festival_reminder_switch_ = s; }
  void set_voice_teaching_switch(OcleanCommandSwitch *s) { this->voice_teaching_switch_ = s; }
  void set_demo_mode_switch(OcleanCommandSwitch *s) { this->demo_mode_switch_ = s; }
  void set_voice_prompt_switch(uint8_t index, OcleanVoiceSwitch *s) {
    if (index < VOICE_PROMPT_COUNT)
      this->voice_prompt_switches_[index] = s;
  }
  void set_scheme_select(OcleanSchemeSelect *s) { this->scheme_select_ = s; }
  void set_language_select(OcleanLanguageSelect *s) { this->language_select_ = s; }
  void set_cloud_host_text_sensor(text_sensor::TextSensor *s) { this->cloud_host_text_sensor_ = s; }
  // baked from yaml (secrets), never entities, so they stay out of the recorder
  void set_birthday(uint8_t month, uint8_t day) {
    this->birthday_month_ = month;
    this->birthday_day_ = day;
  }
  void set_user_profile(uint8_t gender, uint8_t age) {
    this->user_gender_ = gender;
    this->user_age_ = age;
  }
#ifdef USE_OCLEAN_BLUFI
  // Baked provisioning credentials (hub yaml or the node's own wifi:), not
  // entities, so a Wi-Fi password never reaches the Home Assistant recorder.
  void set_blufi_ssid(const std::string &s) { this->blufi_ssid_ = s; }
  void set_blufi_password(const std::string &s) { this->blufi_password_ = s; }
#endif

#ifdef USE_OCLEAN_CLOUD_RECEIVER
  void set_cloud_receiver_enabled(bool en) { this->cloud_receiver_enabled_ = en; }
  bool cloud_receiver_enabled() const { return this->cloud_receiver_enabled_; }
  uint64_t brush_address() const { return this->parent_ != nullptr ? this->parent_->get_address() : 0; }
  // http server task -> main loop; captured gates the record-erasing ack
  void enqueue_cloud_record(const SessionRecord &rec, uint32_t epoch);
  bool cloud_record_captured(uint32_t epoch);
  void set_cloud_drop_future_enabled(bool en) { this->cloud_drop_future_enabled_ = en; }
  bool cloud_drop_future_enabled() const { return this->cloud_drop_future_enabled_; }
  // the node-clock half of the ingest's future check (the brush reading is
  // main-loop state), so the receiver can ack and erase such a record at once
  bool cloud_session_implausible(uint32_t epoch);
  // node local time as "YYYYMMDDHHMMSS" for the brush's currentTime (firmware
  // parses the digits, not an epoch), empty when the node clock is unset
  std::string cloud_current_time();
  // web server task: the Host header of a request from this brush, which is
  // the cloud host it has stored (0233 has no read-back over BLE)
  void note_cloud_host(const std::string &host);
#ifdef USE_OCLEAN_WEATHER
  // Home Assistant weather entity behind the brush's WeatherKit answer: its
  // state and temperature by subscription, the daily forecast by action
  void set_weather_entity(const std::string &entity_id) { this->weather_entity_ = entity_id; }
  bool weather_enabled() const { return !this->weather_entity_.empty(); }
  // web server task side: the reply body from the latest snapshot
  std::string cloud_weather_reply();
#endif
#endif

  // resend reprograms the brush, debounced, but only while custom is selected
  void set_custom_scheme_param(uint8_t kind, uint8_t index, uint8_t value, bool resend);

  // arms a flag when the link is down, so the command still fires once
  // discovery completes
  void trigger_session_capture();

  // bypasses the adaptive off-dock gate
  void trigger_immediate_poll();

  // bytes are the whole command, opcode included; they flush at the start of the
  // next query window. name labels the log line and must outlive the queue entry
  // (a literal). False means nothing was queued (BLE off or queue full), so the
  // caller must skip its optimistic publish.
  bool send_command(std::vector<uint8_t> bytes, const char *name);

  // Warns and writes nothing until the local clock is synced. A mutation, so it
  // runs on a button press only, never on boot or a poll.
  void sync_clock();

  // The frame carries all three flags, so the other two come from the last
  // readback; refused until one has arrived.
  bool set_voice_prompt(uint8_t index, bool on);

  // False, with a warning, for an id past what this brush's firmware has: the
  // brush would switch to English instead.
  bool language_available(uint8_t id);

 protected:
  // per-cycle flags plus the watchdog, for a link that is already up.
  // stamp_cadence = count this as a real poll; write and capture cycles do not
  void arm_cycle_(bool stamp_cadence);

  // link down to up, then arm the cycle
  void begin_connect_cycle_(bool stamp_cadence);

  // query window open or being set up: a record stream may be in flight, and a
  // second round would reset the assemblers mid-transfer
  bool query_round_open_() const { return this->capture_active_ || this->round_setup_pending_(); }
  bool round_setup_pending_() const { return this->awaiting_model_ || this->round_starting_; }

  // samples the local time, so it runs at the moment of the write
  bool build_clock_command_(std::vector<uint8_t> *out);

  // node offset, DST-aware; falls back to tz_index_ when it has no table entry
  uint8_t effective_tz_index_();

  // no-op unless the drift exceeds the threshold
  void maybe_auto_sync_clock_(const DeviceSettings &ds);

  // The clock is set at the end of a round, after the session download: a ring
  // read after the set was recorded before it, in the old time base. Needs this
  // round's clock reading, since the shift it measures moves the watermark.
  // False when nothing was written.
  bool write_due_clock_();

  // the watermark and the newest-record gate follow a set only once a settings
  // read shows the brush took it
  void confirm_clock_shift_(const DeviceSettings &ds);

  SessionClocks session_clocks_();

  // brings the link up, or runs a round on a held one, for queued work
  void kick_link_();

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
  uint16_t lookup_cccd_(uint16_t char_handle);
  bool write_cccd_(uint16_t cccd_handle);
  void dump_gatt_map_();
  // runs once the profile is known: the connection type it picks has to be set
  // before the first register-for-notify
  void begin_queries_();
  void model_read_finished_();
  void start_query_round_();
  bool read_handle_(uint16_t handle, const char *name);
  bool register_notify_handle_(uint16_t handle, const char *name);
  void log_refusal_(const char *name, const uint8_t *bytes, size_t len);
  uint16_t tx_handle_(WriteTarget target) const {
    return target == WriteTarget::TX_SESSION ? this->tx_session_handle_ : this->tx_main_handle_;
  }
  bool write_raw_(uint16_t handle, const uint8_t *bytes, size_t len, const char *name);

#ifdef USE_OCLEAN_BLUFI
  // BluFi provisioning runs on its own cycle, not the Oclean query round: it
  // registers the 0xFF02 notify, then staggers the control and data frames.
  void start_blufi_provision_();
  // writes to the BluFi characteristic, bypassing the Oclean command gate (the
  // frames are not Oclean opcodes); still refused on a read-only hub. redact
  // keeps the Wi-Fi password frame out of the log.
  bool write_blufi_frame_(const std::vector<uint8_t> &frame, const char *name, bool redact = false);
  void handle_blufi_notify_(const uint8_t *data, size_t len);
  // true, and the attempt recorded, when the yaml Wi-Fi is not confirmed and has
  // not been tried since boot: a wrong password must not reprovision every cycle
  bool wifi_sync_due_();
#endif
  // Staggered: Write With Response allows one outstanding write. Returns the ms
  // offset at which the read queries may start without colliding.
  uint32_t flush_pending_writes_();

  // === Values kept on the brush from the yaml (0211, 0233, BluFi) ===
  // None can be read back over BLE, so the hub stores a fingerprint of what the
  // brush confirmed (NVS, never an entity) and writes only on a mismatch.
  uint32_t value_fingerprint_(SyncSlot slot, const uint8_t *payload, size_t len) const;
  void store_synced_(SyncSlot slot, uint32_t fp, const char *what);
  // queues the 0211 and 0233 writes the brush has not confirmed; once per round
  void queue_value_sync_();
  bool handle_value_sync_ack_(const uint8_t *data, size_t len);
  // http://<this node's IPv4>:<web server port>, empty until an address is up
  static std::string node_cloud_url_();

  void handle_dis_read_(uint16_t uuid16, const uint8_t *data, size_t len);
  void handle_battery_(const uint8_t *data, size_t len);
  void handle_session_notify_(const uint8_t *data, size_t len);
  void handle_fixed_session_notify_(const uint8_t *data, size_t len);
  void handle_variable_session_notify_(const uint8_t *data, size_t len);
  void ingest_session_records_(const std::vector<SessionRecord> &records, const uint8_t *newest_raw,
                               size_t newest_raw_len);
#ifdef USE_OCLEAN_CLOUD_RECEIVER
  // one cloud record through the BLE ingest path, minus the BLE-only tail
  void ingest_cloud_record_(const SessionRecord &rec);
  void cloud_remember_(uint32_t epoch);
#ifdef USE_OCLEAN_WEATHER
  void weather_on_state_(StringRef state);
  void weather_on_temperature_(StringRef value);
  void weather_tick_();
  void weather_request_forecast_();
  void weather_on_forecast_(const api::ActionResponse &response);
  int64_t node_utc_offset_s_();
#endif
#endif
  // an inline record of a profile whose inline heads a stored session
  void ingest_inline_session_(const SessionRecord &inl);
  // What a record carries. Fields it lacks go unknown rather than keep an older
  // session's values under a newer timestamp.
  enum class SessionDetail : uint8_t {
    FULL,
    NO_SCORE,  // no score, no zones
    HEAD,      // no score, zones or brushed time
  };
  void publish_session_record_(const SessionRecord &r, SessionDetail detail);
  // One record per loop iteration: same-entity publishes inside one iteration
  // coalesce, so a backfilled ring would collapse to a single recorder row.
  void schedule_next_session_publish_();
  // ts is the ordering/dedup epoch, not the timestamp sent to Home Assistant
  void emit_session_event_(const SessionRecord &r, uint32_t ts);
  void handle_main_notify_(const uint8_t *data, size_t len);
  // an ER reply to one of this round's writes queues it once more for the next
  // round; false for any other frame
  bool handle_write_refusal_(const uint8_t *data, size_t len);
  // the brush's OK to a 0202, which marks an inline-record clear as done; false
  // for any other frame
  bool handle_inline_clear_ack_(const uint8_t *data, size_t len);
  // the record packets a count=0 header is followed by while a session runs;
  // true when the packet was one of them
  bool skip_after_inline_(const uint8_t *data, size_t len);
  void publish_v20_start_settings_(const uint8_t *buf);
  // the one-byte answers to the X Ultra 20's extra reads; false for other frames
  bool handle_status_reply_(const uint8_t *data, size_t len);
  // notify on the session channel after the stream completed: enrichment push
  void handle_enrichment_notify_(const uint8_t *data, size_t len);

  void maybe_finish_poll_();
  void disconnect_();
  void start_watchdog_();

  // Both window-end paths land here: a due clock set and its readback run on
  // the link first, then finish_or_hold_.
  void end_query_window_(const char *reason);
  // Ends a query window: holds the link only while docked with BLE enabled,
  // otherwise disconnects.
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
  sensor::Sensor *battery_voltage_sensor_{nullptr};
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
  sensor::Sensor *quadrant_sensors_[SESSION_QUADRANTS_COUNT]{};
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
  binary_sensor::BinarySensor *auto_update_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *network_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *voice_teaching_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *wifi_configured_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *area_guidance_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *demo_mode_binary_sensor_{nullptr};
  sensor::Sensor *device_mode_sensor_{nullptr};
  sensor::Sensor *mode_number_sensor_{nullptr};
  sensor::Sensor *running_state_sensor_{nullptr};
  OcleanCommandSwitch *auto_mode_switch_{nullptr};
  OcleanCommandSwitch *festival_reminder_switch_{nullptr};
  OcleanCommandSwitch *voice_teaching_switch_{nullptr};
  OcleanCommandSwitch *demo_mode_switch_{nullptr};
  OcleanVoiceSwitch *voice_prompt_switches_[VOICE_PROMPT_COUNT]{};
  std::array<bool, VOICE_PROMPT_COUNT> voice_prompts_{};
  bool voice_prompts_known_{false};
  sensor::Sensor *device_theme_sensor_{nullptr};
  sensor::Sensor *volume_index_sensor_{nullptr};
  sensor::Sensor *head_used_time_sensor_{nullptr};
  sensor::Sensor *clock_drift_sensor_{nullptr};
  text_sensor::TextSensor *timezone_text_sensor_{nullptr};
  OcleanHeadMaxNumber *head_max_number_{nullptr};
  OcleanCommandSwitch *area_reminder_switch_{nullptr};
  OcleanCommandSwitch *over_pressure_switch_{nullptr};
  OcleanCommandSwitch *brush_pause_switch_{nullptr};
  OcleanCommandSwitch *raise_wake_switch_{nullptr};
  OcleanCommandSwitch *brush_mode_switch_{nullptr};
  OcleanSchemeSelect *scheme_select_{nullptr};
  OcleanLanguageSelect *language_select_{nullptr};
  text_sensor::TextSensor *cloud_host_text_sensor_{nullptr};
  uint8_t birthday_month_{BIRTHDAY_UNSET};
  uint8_t birthday_day_{BIRTHDAY_UNSET};
  uint8_t user_gender_{USER_GENDER_DEFAULT};
  uint8_t user_age_{USER_AGE_DEFAULT};
  struct SyncedValues {
    std::array<uint32_t, SYNC_SLOTS> fp;
  };
  SyncedValues synced_{};
  esphome::ESPPreferenceObject synced_pref_;
  // fingerprint of a write sent this round, until its ack
  std::array<uint32_t, SYNC_SLOTS> sync_sent_{};
  // a cloud host the brush acked but no request has confirmed yet: not written
  // again before a reboot or a request naming another host
  uint32_t cloud_host_written_fp_{0};

  State state_{State::IDLE};
  BrushModel model_{BrushModel::X_PRO_ELITE};
  bool expose_dev_sensors_{false};
  bool read_only_{false};

  // Handles resolved at SEARCH_CMPL for the characteristics this hub uses.
  uint16_t battery_handle_{0};
  uint16_t model_handle_{0};
  uint16_t hw_rev_handle_{0};
  uint16_t fw_rev_handle_{0};
  uint16_t sw_rev_handle_{0};
  uint16_t rx_main_handle_{0};
  uint16_t rx_session_handle_{0};
  uint16_t tx_session_handle_{0};
  uint16_t tx_main_handle_{0};
  uint16_t battery_cccd_{0};
  uint16_t rx_main_cccd_{0};
  uint16_t rx_session_cccd_{0};
#ifdef USE_OCLEAN_BLUFI
  // BluFi provisioning characteristics, X Ultra 20 only (service 0xFFFF)
  uint16_t blufi_write_handle_{0};
  uint16_t blufi_notify_handle_{0};
  uint16_t blufi_notify_cccd_{0};
#endif

  // Per-cycle completion tracking. The poll is done once battery and DIS model
  // have been read (or their reads have failed) so the link can drop early.
  bool got_battery_{false};
  bool got_model_{false};

  // millis(); the staleness check subtracts, so a wrap works out
  bool dis_cached_{false};
  uint32_t last_dis_read_ms_{0};

  bool awaiting_model_{false};
  bool round_starting_{false};
  uint8_t cccd_writes_pending_{0};

  struct PendingWrite {
    std::vector<uint8_t> bytes;
    const char *name;
    uint8_t retries{0};
    // requeued or dropped after a refusal: not sent if still due, not matched
    // by another refusal
    bool settled{false};
  };
  std::vector<PendingWrite> pending_writes_{};
  // The writes flushed into the current round, kept so an ER reply can requeue
  // them. The generation lets a stale write timer from an older round bail out.
  std::vector<PendingWrite> round_writes_{};
  uint32_t round_write_gen_{0};

  // epoch of the newest record already emitted; anything at or below it is
  // skipped, so a session fires its event once across reboots
  uint32_t last_session_emitted_{0};
  esphome::ESPPreferenceObject session_wm_pref_;
  // The preference layer takes a blob on its stored length alone, so an older
  // layout has to be rejected by size, not by inspection: this one runs two
  // bytes longer than the {record, flag} blob it replaced. Magic and version are
  // the second line. Change both the size and the version on any layout change
  // here or in SessionRecord. Version 1 is the same size: it carried five
  // unmapped record bytes where the time zone and the quadrants sit now, and is
  // loaded with those marked unknown.
  static constexpr uint16_t PERSISTED_SESSION_MAGIC = 0x0C1E;
  static constexpr uint8_t PERSISTED_SESSION_VERSION = 2;
  static constexpr uint8_t PERSISTED_SESSION_VERSION_V1 = 1;
  // partial is the SessionDetail of an inline fragment, 0 for a full record
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
  // Epoch of the inline record a 0202 went out for, and of the one the brush
  // acked. RAM only: after a reboot the clear is sent once more.
  uint32_t inline_clear_sent_epoch_{0};
  uint32_t inline_cleared_epoch_{0};
  SessionRecord shown_session_{};
  bool shown_session_valid_{false};
  // oldest-first, drained one per loop iteration
  std::vector<SessionRecord> pending_session_publish_{};
  std::vector<OcleanSessionTrigger *> session_triggers_{};

#ifdef USE_OCLEAN_CLOUD_RECEIVER
  // web server task fills cloud_inbound_, the main loop drains it; cloud_captured_
  // (RAM only) gates the record-erasing ack
  bool cloud_receiver_enabled_{false};
  bool cloud_drop_future_enabled_{true};
  Mutex cloud_mutex_;
  std::vector<SessionRecord> cloud_inbound_;
  std::vector<uint32_t> cloud_captured_;
  // Host header of the brush's latest request, handed to the main loop
  std::string cloud_host_seen_;
  bool cloud_host_fresh_{false};
  // the main loop's side: what the read-only entity last showed
  std::string cloud_host_shown_;
  void process_cloud_host_(const std::string &host);
#ifdef USE_OCLEAN_WEATHER
  std::string weather_entity_;
  // main loop writes, the web server task copies, both under cloud_mutex_
  WeatherSnapshot weather_{};
  std::string weather_unmapped_;  // last condition warned about
  uint32_t weather_next_ask_ms_{0};
  bool weather_ha_connected_{false};
  bool weather_waiting_{false};
  // the API server keeps a response callback until an answer consumes it and
  // has no way to drop one, so one stays registered and every request reuses it
  bool weather_callback_armed_{false};
  bool weather_refusal_warned_{false};
#endif
#endif

  // reset each query round
  uint32_t notify_count_this_round_{0};
  bool notify_flood_warned_{false};
  bool inline_seen_this_round_{false};
  bool after_inline_logged_{false};

  // One-shot boot-stagger latch. Latched so a millis() wrap (which resets the
  // stagger window) cannot re-defer an already-running hub.
  bool boot_stagger_done_{false};

  // Session-capture dev hook.
  bool capture_armed_{false};
  bool capture_active_{false};

#ifdef USE_OCLEAN_BLUFI
  std::string blufi_ssid_{};
  std::string blufi_password_{};
  uint8_t blufi_seq_{0};
  // fingerprint of the credentials tried this boot, and of the run awaiting
  // the brush's connected report
  uint32_t blufi_tried_fp_{0};
  uint32_t blufi_pending_fp_{0};
#endif
  // Reassembles the *B# record stream that arrives on the session notify
  // characteristic during a capture window.
  SessionAssembler session_asm_{};
  VarSessionAssembler session_v20_asm_{};
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
  bool clock_sync_due_{false};
  // this round's brush clock and the millis() it arrived at; 0 = not read
  int64_t round_brush_clock_{0};
  uint32_t round_brush_clock_ms_{0};
  int64_t clock_shift_pending_s_{0};

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
  bool cycle_stamps_cadence_{false};

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
