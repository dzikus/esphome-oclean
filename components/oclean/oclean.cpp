#include "oclean.h"
// each of these pulls in its platform, which is only in the build when the
// yaml declares it
#ifdef USE_BUTTON
#include "oclean_button.h"
#endif
#ifdef USE_SWITCH
#include "oclean_switch.h"
#endif
#ifdef USE_SELECT
#include "oclean_select.h"
#endif
#ifdef USE_NUMBER
#include "oclean_number.h"
#endif

#include <algorithm>
#include <cmath>

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace oclean {

static const char *const TAG = "oclean";

static const auto OCLEAN_SERVICE = espbt::ESPBTUUID::from_raw(OCLEAN_SERVICE_UUID);

// Cap on queued writes. Bounds the worst case where HA floods send_command (a
// runaway automation or a debounce bug) while the brush is asleep and the queue
// cannot drain. Above this the newest write is dropped with a warning.
static const size_t MAX_PENDING_WRITES = 16;

// DIS strings are device-controlled input. A hostile or buggy peripheral may
// report a multi-kilobyte length, so clamp before allocating the std::string.
static const uint16_t MAX_DIS_STRING_LEN = 64;

// Max notifies per round before the peer counts as flooding and the link drops.
// A full 32-record ring plus status/settings/battery stays under 200; the rest
// is headroom.
static const uint32_t NOTIFY_FLOOD_CAP = 256;

void OcleanHub::setup() {
  this->set_state_(State::IDLE);
  // Dedup watermark, keyed off the brush MAC so two hubs never share a slot.
  // Unset loads as 0, which emits the whole ring once.
  uint32_t hub_hash = this->pref_salt();
  this->session_wm_pref_ = esphome::global_preferences->make_preference<uint32_t>(hub_hash ^ 0x05E5510Eu);
  this->session_wm_pref_.load(&this->last_session_emitted_);
  this->session_last_pref_ = esphome::global_preferences->make_preference<PersistedSession>(hub_hash ^ 0x5E55D47Au);
  PersistedSession last;
  if (this->session_last_pref_.load(&last)) {
    if (last.magic == PERSISTED_SESSION_MAGIC && last.version == PERSISTED_SESSION_VERSION) {
      this->newest_record_epoch_ = session_record_epoch(last.record);
      this->publish_session_record_(last.record, last.partial != 0);
    } else {
      // right length, wrong content; the next ring download refills the entities
      ESP_LOGW(TAG, "[%s] stored session is not v%u (magic 0x%04X, version %u): discarded",
               this->parent_->address_str(), (unsigned)PERSISTED_SESSION_VERSION, (unsigned)last.magic,
               (unsigned)last.version);
    }
  }
  // static value, so once here instead of on every poll
  this->publish_(this->mac_text_sensor_, std::string(this->parent_->address_str()));
  // BLEClient sets up later (AFTER_BLUETOOTH) and leaves itself enabled, so a
  // plain set_enabled(false) here is overridden and the client auto-connects at
  // boot, outside any poll cycle.
  this->defer([this]() {
    if (this->state_ == State::IDLE && this->parent_ != nullptr) {
      this->parent_->set_enabled(false);
    }
  });
}

void OcleanHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Oclean Hub:");
  ESP_LOGCONFIG(TAG, "  MAC: %s", this->parent_->address_str());
  ESP_LOGCONFIG(TAG, "  Hub: %d of %d", this->hub_index_, this->total_hubs_);
  // the tick runs at the docked cadence and update() gates the off-dock one, so
  // get_update_interval() alone would misreport an off-dock brush
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms docked / %u ms off dock", (unsigned)this->charging_interval_ms_,
                (unsigned)this->battery_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Hold link while docked: %s", YESNO(this->hold_while_docked_));
  ESP_LOGCONFIG(TAG, "  Expose dev sensors: %s", YESNO(this->expose_dev_sensors_));
  // Active protocol profile: the default until the first poll reads the DIS
  // model string and selects the per-device profile.
  ESP_LOGCONFIG(TAG, "  Profile: %s (confidence %u)", this->profile_->name, (unsigned)this->profile_->confidence);
}

void OcleanHub::publish_(sensor::Sensor *s, float value) {
  if (s != nullptr)
    s->publish_state(value);
}

void OcleanHub::publish_(binary_sensor::BinarySensor *s, bool value) {
  if (s != nullptr)
    s->publish_state(value);
}

void OcleanHub::publish_(text_sensor::TextSensor *s, const std::string &value) {
  if (s != nullptr)
    s->publish_state(value);
}

const char *OcleanHub::state_name_(State s) const {
  switch (s) {
    case State::IDLE:
      return "IDLE";
    case State::CONNECTING:
      return "CONNECTING";
    case State::DISCOVERING:
      return "DISCOVERING";
    case State::POLLING:
      return "POLLING";
    case State::DISCONNECTING:
      return "DISCONNECTING";
  }
  return "?";
}

void OcleanHub::set_state_(State s) {
  if (this->state_ == s)
    return;
  ESP_LOGD(TAG, "[%s] state %s -> %s", this->parent_->address_str(), this->state_name_(this->state_),
           this->state_name_(s));
  this->state_ = s;
}

void OcleanHub::set_ble_user_enabled(bool en) {
  if (en == this->ble_user_enabled_)
    return;
  this->ble_user_enabled_ = en;
  if (!en) {
    // Drop queued config writes: a disabled hub must not accumulate mutations
    // that would fire unannounced on a later connect.
    this->pending_writes_.clear();
    this->capture_armed_ = false;
    if (this->state_ != State::IDLE) {
      // Tear down the in-flight cycle immediately. The brush buffers sessions
      // internally, so an aborted download loses nothing. disconnect_ also
      // clears held mode, so a held link is dropped here too.
      if (this->holding_)
        ESP_LOGI(TAG, "[%s] leaving held mode (bt off)", this->parent_->address_str());
      this->disconnect_();
    } else if (this->parent_ != nullptr) {
      // Idle path still covers the boot race where BLEClient setup leaves
      // itself enabled.
      this->parent_->set_enabled(false);
    }
    ESP_LOGI(TAG, "[%s] BLE user-disabled, halting all activity", this->parent_->address_str());
  } else {
    // Resume immediately: a user flipping the switch back on expects the hub
    // to act now, not at the next timer tick. Marking a poll pending
    // lifts the adaptive off-dock gate for this one cycle. If a teardown from
    // the OFF flip is still in flight, update() skips and the next tick polls.
    this->poll_pending_ = true;
    ESP_LOGI(TAG, "[%s] BLE user-enabled, polling now", this->parent_->address_str());
    this->update();
  }
}

void OcleanHub::update() {
  PollTickState st{};
  st.now_ms = millis();
  st.last_poll_ms = this->last_poll_ms_;
  st.boot_stagger_ms = uint32_t(this->hub_index_) * BOOT_STAGGER_MS;
  st.charging_interval_ms = this->charging_interval_ms_;
  st.battery_interval_ms = this->battery_interval_ms_;
  st.ble_enabled = this->ble_user_enabled_;
  st.link_busy = this->node_state == espbt::ClientState::ESTABLISHED || this->parent_->enabled;
  st.poll_pending = this->poll_pending_;
  st.adaptive = this->adaptive_poll_;
  st.docked = this->docked_last_;
  st.boot_stagger_done = this->boot_stagger_done_;

  PollDecision d = plan_poll_tick(st);
  switch (d.action) {
    case PollAction::SKIP_BLE_OFF:
      // master BLE switch off; also covers the adaptive charging-interval ticks
      ESP_LOGV(TAG, "[%s] poll skipped (BLE user-disabled)", this->parent_->address_str());
      return;
    case PollAction::SKIP_LINK_BUSY:
      ESP_LOGW(TAG, "[%s] poll cycle still active (state=%s), skipping tick", this->parent_->address_str(),
               this->state_name_(this->state_));
      return;
    case PollAction::DEFER_BOOT_STAGGER:
      // latch it. A millis() wrap reads as "still in the boot window" and would
      // re-defer a hub that has been running for weeks
      this->boot_stagger_done_ = true;
      ESP_LOGI(TAG, "[%s] first poll deferred %ums (boot stagger)", this->parent_->address_str(), (unsigned)d.defer_ms);
      this->set_timeout("boot_stagger", d.defer_ms, [this]() { this->update(); });
      return;
    case PollAction::SKIP_NOT_DUE:
      ESP_LOGV(TAG, "[%s] poll skipped (off-dock, battery interval not due)", this->parent_->address_str());
      return;
    case PollAction::POLL:
      break;
  }

  ESP_LOGD(TAG, "[%s] starting poll cycle", this->parent_->address_str());
  this->begin_connect_cycle_(true);
}

void OcleanHub::arm_cycle_(bool stamp_cadence) {
  if (stamp_cadence) {
    this->poll_pending_ = false;
    this->last_poll_ms_ = millis();
  }
  this->got_battery_ = false;
  this->got_model_ = false;
  this->start_watchdog_();
}

void OcleanHub::begin_connect_cycle_(bool stamp_cadence) {
  // handles belong to the connection that resolved them; SEARCH_CMPL re-resolves
  this->battery_handle_ = 0;
  this->model_handle_ = 0;
  this->hw_rev_handle_ = 0;
  this->sw_rev_handle_ = 0;
  this->rx_main_handle_ = 0;
  this->rx_session_handle_ = 0;
  this->tx_session_handle_ = 0;
  this->tx_main_handle_ = 0;
  this->set_state_(State::CONNECTING);
  this->parent_->set_enabled(true);
  this->arm_cycle_(stamp_cadence);
}

void OcleanHub::start_watchdog_() {
  // Force a disconnect if the whole cycle does not complete in time.
  this->set_timeout("poll_watchdog", WHOLE_POLL_TIMEOUT_MS, [this]() {
    if (this->state_ != State::IDLE) {
      if (this->state_ == State::CONNECTING) {
        // The brush was never reached, so the cycle read nothing. Marking a
        // poll pending lets the next tick retry right away instead of
        // waiting out the full off-dock interval.
        this->poll_pending_ = true;
      }
      ESP_LOGW(TAG, "[%s] poll watchdog fired, forcing disconnect", this->parent_->address_str());
      // out of range or asleep otherwise reads in HA like "nothing changed"
      this->status_set_warning("poll timed out");
      this->disconnect_();
    }
  });
}

void OcleanHub::disconnect_() {
  this->cancel_timeout("poll_watchdog");
  this->cancel_timeout("capture_hold");
  this->cancel_timeout("enrichment_wait");
  // Leaving held mode on every disconnect path: cancel the re-query timer and
  // clear the flag so a torn-down link never leaves a zombie held timer running.
  this->clear_hold_();
  this->holding_ = false;
  this->capture_active_ = false;
  this->set_state_(State::DISCONNECTING);
  // Dropping the link conserves both the ESP32 BLE slot and the brush battery.
  this->parent_->set_enabled(false);
}

// ESPHome's BLEClient dispatches GATTC events on the main loop task, not on the
// Bluedroid BT task: the BT callback enqueues each event and the registered
// handlers run synchronously from BLEClient::loop(). update(), the entity
// control() paths and this handler therefore all run on the same task, so the
// member state touched here needs no locking.
void OcleanHub::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                    esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status != ESP_GATT_OK) {
        // Failed open: the link never reached the brush. poll_pending_ retries on
        // the next tick rather than waiting out the full off-dock interval.
        ESP_LOGW(TAG, "[%s] connection open failed, status=%d", this->parent_->address_str(), param->open.status);
        this->status_set_warning("connection failed");
        this->publish_(this->connected_binary_sensor_, false);
        if (this->state_ == State::CONNECTING)
          this->poll_pending_ = true;
        this->disconnect_();
        break;
      }
      ESP_LOGI(TAG, "[%s] connection opened", this->parent_->address_str());
      this->publish_(this->connected_binary_sensor_, true);
      if (!this->ble_user_enabled_) {
        // A stray connect raced the user-disable (e.g. boot auto-connect before
        // the restored switch state applied). Drop it instead of adopting.
        ESP_LOGW(TAG, "[%s] dropping connection: BLE user-disabled", this->parent_->address_str());
        this->disconnect_();
        break;
      }
      if (this->state_ == State::IDLE) {
        // Connection initiated outside update(), e.g. a boot auto-connect
        // racing the deferred disable. Adopt it as a regular poll cycle so
        // it still reads and then disconnects instead of idling forever.
        // Stamp the cycle so the adaptive gate does not fire an extra poll
        // right after this adopted cycle completes.
        this->arm_cycle_(true);
      }
      this->set_state_(State::DISCOVERING);
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGI(TAG, "[%s] disconnected", this->parent_->address_str());
      this->publish_(this->connected_binary_sensor_, false);
      this->node_state = espbt::ClientState::IDLE;
      // Restore the discovery connection type. The notify-registration path
      // flips it to the cache type to dodge a faulting descriptor lookup; the
      // next cycle needs the discovery type so services are searched and
      // handles are re-resolved.
      this->parent_->set_connection_type(espbt::ConnectionType::V1);
      this->cancel_timeout("poll_watchdog");
      this->cancel_timeout("capture_hold");
      this->cancel_timeout("enrichment_wait");
      // A remote disconnect while holding (e.g. brush lifted off the dock and
      // dropped the link) must clear the held timer and flag so the next cycle
      // returns to normal adaptive cadence.
      if (this->holding_)
        ESP_LOGI(TAG, "[%s] leaving held mode (remote disconnect)", this->parent_->address_str());
      this->clear_hold_();
      this->holding_ = false;
      this->capture_active_ = false;
      this->set_state_(State::IDLE);
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      ESP_LOGD(TAG, "[%s] service discovery complete", this->parent_->address_str());
      // A BLEClientNode must mark itself established; nothing else does it,
      // and the settle callback below bails out on a non-established state.
      this->node_state = espbt::ClientState::ESTABLISHED;
      // Resolve handles and subscribe to notifies synchronously here. The
      // characteristic table is only valid during this event; a deferred
      // lookup from the settle callback returns nullptr for everything.
      this->resolve_handles_();
      // Settle before issuing reads; some firmware drops early requests. By
      // now every handle is cached, so the callback only reads and never looks
      // characteristics up again.
      this->set_timeout("settle", POST_CONNECT_SETTLE_MS, [this]() {
        if (this->node_state != espbt::ClientState::ESTABLISHED)
          return;
        this->set_state_(State::POLLING);
        // The link reached the brush: stamp the freshness sensor now so a stale
        // value flags an unreachable brush even if no reading changed. Also drops
        // a warning left by an earlier failed cycle.
        this->publish_last_seen_();
        this->status_clear_warning();
        // Device info is static, so read it at most once a day and serve it
        // from cache on the polls in between to keep each link short.
        bool dis_fresh = this->dis_cached_ && (millis() - this->last_dis_read_ms_) < DIS_CACHE_MS;
        if (dis_fresh) {
          this->got_model_ = true;
          // Re-assert the profile from the cached model string so a cached boot
          // routes commands by the right profile without re-reading DIS.
          this->profile_ = profile_for_model(this->model_string_.c_str(), this->model_string_.size());
          if (this->model_text_sensor_ != nullptr && !this->model_string_.empty())
            this->model_text_sensor_->publish_state(this->model_string_);
          if (this->hw_rev_text_sensor_ != nullptr && !this->hw_rev_string_.empty())
            this->hw_rev_text_sensor_->publish_state(this->hw_rev_string_);
          if (this->sw_rev_text_sensor_ != nullptr && !this->sw_rev_string_.empty())
            this->sw_rev_text_sensor_->publish_state(this->sw_rev_string_);
          ESP_LOGD(TAG, "[%s] device info served from cache (%s, profile %s)", this->parent_->address_str(),
                   this->model_string_.c_str(), this->profile_->name);
        } else {
          this->read_handle_(this->model_handle_, "dis-model");
          this->read_handle_(this->hw_rev_handle_, "dis-hw-rev");
          this->read_handle_(this->sw_rev_handle_, "dis-sw-rev");
        }
        this->read_handle_(this->battery_handle_, "battery");
        if (this->battery_handle_ == 0)
          this->got_battery_ = true;
        // Every poll does the full query (status, settings, session download)
        // so the session and status entities stay populated, not just after a
        // manual capture. The query holds the link for the responses, then
        // disconnects. capture_armed_ extends the hold and adds the probe.
        this->query_device_(this->capture_armed_);
        this->capture_armed_ = false;
      });
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[%s] register_for_notify failed handle=0x%04X status=%d", this->parent_->address_str(),
                 param->reg_for_notify.handle, param->reg_for_notify.status);
      }
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[%s] read handle=0x%04X failed status=%d", this->parent_->address_str(), param->read.handle,
                 param->read.status);
        break;
      }
      uint16_t h = param->read.handle;
      if (h == this->battery_handle_) {
        this->handle_battery_(param->read.value, param->read.value_len);
      } else if (h == this->model_handle_) {
        this->handle_dis_read_(DIS_MODEL_UUID16, param->read.value, param->read.value_len);
      } else if (h == this->hw_rev_handle_) {
        this->handle_dis_read_(DIS_HW_REV_UUID16, param->read.value, param->read.value_len);
      } else if (h == this->sw_rev_handle_) {
        this->handle_dis_read_(DIS_SW_REV_UUID16, param->read.value, param->read.value_len);
      } else {
        ESP_LOGD(TAG, "[%s] read handle=0x%04X len=%u data=%s", this->parent_->address_str(), h,
                 (unsigned)param->read.value_len, format_hex_pretty(param->read.value, param->read.value_len).c_str());
      }
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      // Writes are time-staggered, not ACK-paced; the completion event is
      // inspected only so a failed write (e.g. congestion) is visible.
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[%s] write failed on handle 0x%04X, status=%d", this->parent_->address_str(),
                 param->write.handle, param->write.status);
      }
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (++this->notify_count_this_round_ > NOTIFY_FLOOD_CAP) {
        // Over the cap: flooding peer. Drop the link once, ignore the rest.
        if (!this->notify_flood_warned_) {
          this->notify_flood_warned_ = true;
          ESP_LOGW(TAG, "[%s] notify flood (> %u this round), dropping link", this->parent_->address_str(),
                   (unsigned)NOTIFY_FLOOD_CAP);
          this->disconnect_();
        }
        break;
      }
      const uint8_t *buf = param->notify.value;
      const size_t len = param->notify.value_len;
      // Hex-dump every notify: the raw feed for protocol analysis.
      ESP_LOGD(TAG, "[%s] notify h=0x%04X len=%u data=%s", this->parent_->address_str(), param->notify.handle,
               (unsigned)len, format_hex_pretty(buf, len).c_str());
      if (param->notify.handle == this->battery_handle_) {
        this->handle_battery_(buf, len);
      } else if (param->notify.handle == this->rx_session_handle_) {
        this->handle_session_notify_(buf, len);
      } else if (param->notify.handle == this->rx_main_handle_) {
        this->handle_main_notify_(buf, len);
      }
      break;
    }
    default:
      break;
  }
}

void OcleanHub::resolve_handles_() {
  // parsed once, not on every discovery
  static const auto rx_main = espbt::ESPBTUUID::from_raw(READ_NOTIFY_CHAR_UUID);
  static const auto rx_session = espbt::ESPBTUUID::from_raw(RECEIVE_BRUSH_UUID);
  static const auto tx_session = espbt::ESPBTUUID::from_raw(SEND_BRUSH_CMD_UUID);
  static const auto tx_main = espbt::ESPBTUUID::from_raw(WRITE_CHAR_UUID);

  auto resolve16 = [this](uint16_t svc, uint16_t chr) -> uint16_t {
    auto *c = this->parent_->get_characteristic(svc, chr);
    return c == nullptr ? 0 : c->handle;
  };
  auto resolve_full = [this](const espbt::ESPBTUUID &chr) -> uint16_t {
    auto *c = this->parent_->get_characteristic(OCLEAN_SERVICE, chr);
    return c == nullptr ? 0 : c->handle;
  };

  this->battery_handle_ = resolve16(BATTERY_SERVICE_UUID16, BATTERY_CHAR_UUID16);
  this->model_handle_ = resolve16(DIS_SERVICE_UUID16, DIS_MODEL_UUID16);
  this->hw_rev_handle_ = resolve16(DIS_SERVICE_UUID16, DIS_HW_REV_UUID16);
  this->sw_rev_handle_ = resolve16(DIS_SERVICE_UUID16, DIS_SW_REV_UUID16);
  this->rx_main_handle_ = resolve_full(rx_main);
  this->rx_session_handle_ = resolve_full(rx_session);
  this->tx_session_handle_ = resolve_full(tx_session);
  this->tx_main_handle_ = resolve_full(tx_main);

  ESP_LOGD(TAG,
           "[%s] handles bat=0x%04X model=0x%04X hw=0x%04X sw=0x%04X "
           "rx-main=0x%04X rx-session=0x%04X tx-session=0x%04X tx-main=0x%04X",
           this->parent_->address_str(), this->battery_handle_, this->model_handle_, this->hw_rev_handle_,
           this->sw_rev_handle_, this->rx_main_handle_, this->rx_session_handle_, this->tx_session_handle_,
           this->tx_main_handle_);

  // A missing DIS model must not stall the cycle waiting on a read event.
  if (this->model_handle_ == 0) {
    this->got_model_ = true;
  }

  // The base client, on each register-for-notify completion, looks up the
  // characteristic's config descriptor to auto-write the notify enable bit.
  // On this device that descriptor list is malformed and the lookup can
  // dereference a bad pointer and fault the task. Switching to the cache
  // connection type makes the base client skip that lookup and assume the
  // client owns the descriptor. The brush streams notifies without a
  // descriptor write anyway, so nothing is lost. Reset on disconnect.
  this->parent_->set_connection_type(espbt::ConnectionType::V3_WITH_CACHE);

  // Subscribe to every known notify characteristic. Missing characteristics
  // are logged and skipped, never fatal.
  this->register_notify_handle_(this->battery_handle_, "battery");
  this->register_notify_handle_(this->rx_main_handle_, "rx-main");
  this->register_notify_handle_(this->rx_session_handle_, "rx-session");
}

void OcleanHub::read_handle_(uint16_t handle, const char *name) {
  if (handle == 0) {
    ESP_LOGD(TAG, "[%s] no handle for %s, skipping read", this->parent_->address_str(), name);
    return;
  }
  auto status = esp_ble_gattc_read_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), handle,
                                        ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "[%s] read %s failed status=%d", this->parent_->address_str(), name, status);
  }
}

void OcleanHub::register_notify_handle_(uint16_t handle, const char *name) {
  if (handle == 0) {
    ESP_LOGD(TAG, "[%s] notify char %s not found, skipping", this->parent_->address_str(), name);
    return;
  }
  auto status =
      esp_ble_gattc_register_for_notify(this->parent_->get_gattc_if(), this->parent_->get_remote_bda(), handle);
  if (status != ESP_OK) {
    ESP_LOGD(TAG, "[%s] register_for_notify %s failed status=%d", this->parent_->address_str(), name, status);
  }
}

bool OcleanHub::write_raw_(uint16_t handle, const uint8_t *bytes, size_t len, const char *name) {
  if (handle == 0) {
    ESP_LOGW(TAG, "[%s] write char for %s not found", this->parent_->address_str(), name);
    return false;
  }
  ESP_LOGI(TAG, "[%s] write %s -> %s", this->parent_->address_str(), name, format_hex_pretty(bytes, len).c_str());
  // Write With Response is mandatory; the device drops Write No Response.
  auto status = esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), handle, len,
                                         const_cast<uint8_t *>(bytes), ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "[%s] write %s failed status=%d", this->parent_->address_str(), name, status);
    return false;
  }
  return true;
}

bool OcleanHub::send_command(std::vector<uint8_t> bytes, const char *name, WriteKind kind) {
  if (!this->ble_user_enabled_) {
    // Drop, do not queue: a write queued while disabled would fire unannounced
    // whenever BLE comes back, and the optimistic entity state would lie until
    // then.
    ESP_LOGW(TAG, "[%s] command %s ignored: BLE user-disabled", this->parent_->address_str(), name);
    return false;
  }
  if (this->pending_writes_.size() >= MAX_PENDING_WRITES) {
    // Queue is full (brush asleep and not draining). Drop the newest write
    // instead of growing the queue without bound. Leave the BLE link alone.
    ESP_LOGW(TAG, "[%s] command %s dropped: write queue full (%u pending)", this->parent_->address_str(), name,
             (unsigned)this->pending_writes_.size());
    return false;
  }
  ESP_LOGI(TAG, "[%s] queued command %s (%u bytes)", this->parent_->address_str(), name, (unsigned)bytes.size());
  this->pending_writes_.push_back(PendingWrite{std::move(bytes), name, kind});
  // Bring the link up if it is down; the writes flush in the query window once
  // discovery completes. If a cycle is already in flight they flush on its
  // query window (or the next one). Mirrors the capture trigger.
  bool connected = this->node_state == espbt::ClientState::ESTABLISHED;
  if (this->holding_ && this->state_ == State::POLLING) {
    // Held link is up and idle between re-queries. Run a re-query round now so
    // the write flushes promptly instead of waiting out the charging interval.
    ESP_LOGI(TAG, "[%s] flushing queued command on held link", this->parent_->address_str());
    this->held_requery_();
    return true;
  }
  if (!connected && !this->parent_->enabled) {
    ESP_LOGI(TAG, "[%s] starting poll cycle to flush queued command", this->parent_->address_str());
    // not a scheduled poll, cadence untouched
    this->begin_connect_cycle_(false);
  }
  return true;
}

void OcleanHub::sync_clock() {
#ifdef USE_TIME
  if (this->time_ == nullptr) {
    ESP_LOGW(TAG, "[%s] sync-clock pressed but no time source configured", this->parent_->address_str());
    return;
  }
  if (!this->time_->now().is_valid()) {
    ESP_LOGW(TAG, "[%s] sync-clock pressed but local time is not synced yet", this->parent_->address_str());
    return;
  }
  this->queue_set_clock_("manual");
#else
  ESP_LOGW(TAG, "[%s] sync-clock pressed but the firmware was built without time support",
           this->parent_->address_str());
#endif
}

#ifdef USE_TIME
// Civil fields of an ESPTime as a Unix epoch, same basis as civil_to_epoch.
static int64_t epoch_of(const ESPTime &t) {
  return civil_to_epoch(t.year, t.month, t.day_of_month, t.hour, t.minute, t.second);
}
#endif

uint8_t OcleanHub::effective_tz_index_() {
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    ESPTime local = this->time_->now();
    ESPTime utc = this->time_->utcnow();
    if (local.is_valid() && utc.is_valid()) {
      uint8_t idx = tz_index_for_offset_seconds((int32_t)(epoch_of(local) - epoch_of(utc)));
      if (idx != 0)
        return idx;
    }
  }
#endif
  return this->tz_index_;
}

int64_t OcleanHub::local_now_epoch_() {
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    ESPTime now = this->time_->now();
    if (now.is_valid())
      return epoch_of(now);
  }
#endif
  return 0;
}

bool OcleanHub::build_clock_command_(std::vector<uint8_t> *out) {
#ifdef USE_TIME
  if (this->time_ == nullptr)
    return false;
  ESPTime now = this->time_->now();
  if (!now.is_valid())
    return false;
  // ESPTime day_of_week is 1=Sunday..7=Saturday; the device wants 0=Sunday..6.
  uint8_t weekday = (now.day_of_week >= 1) ? (uint8_t)(now.day_of_week - 1) : 0;
  *out = build_set_clock_command(now.year, now.month, now.day_of_month, now.hour, now.minute, now.second, weekday,
                                 this->effective_tz_index_());
  return true;
#else
  (void)out;
  return false;
#endif
}

void OcleanHub::queue_set_clock_(const char *reason) {
#ifdef USE_TIME
  std::vector<uint8_t> cmd;
  if (!this->build_clock_command_(&cmd))
    return;
  ESP_LOGI(TAG, "[%s] sync-clock (%s) queued, time is sampled again at write", this->parent_->address_str(), reason);
  this->send_command(std::move(cmd), "sync-clock", WriteKind::CLOCK);
#else
  (void)reason;
#endif
}

void OcleanHub::maybe_auto_sync_clock_(const DeviceSettings &ds) {
#ifdef USE_TIME
  if (!this->auto_sync_time_ || this->time_ == nullptr || !ds.clock_valid)
    return;
  ESPTime now = this->time_->now();
  if (!now.is_valid())
    return;
  // Do not pile up: if a clock write is already queued, wait for it to flush and
  // be read back before deciding again.
  for (const auto &w : this->pending_writes_)
    if (w.kind == WriteKind::CLOCK)
      return;
  int64_t brush_epoch = civil_to_epoch(ds.year, ds.month, ds.day, ds.hour, ds.minute, ds.second);
  int64_t local_epoch = epoch_of(now);
  if (!should_resync_clock(brush_epoch, local_epoch, this->sync_drift_threshold_s_))
    return;
  ESP_LOGI(TAG, "[%s] clock drift %lld s exceeds %u s, queuing auto sync", this->parent_->address_str(),
           (long long)(brush_epoch - local_epoch), (unsigned)this->sync_drift_threshold_s_);
  this->queue_set_clock_("auto");
#else
  (void)ds;
#endif
}

void OcleanHub::publish_clock_drift_(const DeviceSettings &ds) {
#ifdef USE_TIME
  if (this->clock_drift_sensor_ == nullptr || this->time_ == nullptr || !ds.clock_valid)
    return;
  ESPTime now = this->time_->now();
  if (!now.is_valid())
    return;
  // Same local-time basis as the auto-sync check. Signed: positive means the
  // brush clock runs ahead of real time, negative means it lags behind.
  int64_t brush_epoch = civil_to_epoch(ds.year, ds.month, ds.day, ds.hour, ds.minute, ds.second);
  int64_t local_epoch = epoch_of(now);
  this->clock_drift_sensor_->publish_state((float)clamp_clock_drift(brush_epoch - local_epoch));
#else
  (void)ds;
#endif
}

void OcleanHub::publish_last_seen_() {
#ifdef USE_TIME
  if (this->last_seen_text_sensor_ == nullptr || this->time_ == nullptr)
    return;
  ESPTime now = this->time_->utcnow();
  if (!now.is_valid())
    return;
  // RFC3339 in UTC, e.g. 2026-06-06T12:34:56Z. The timestamp device class on the
  // text sensor turns this into a relative "x ago" in Home Assistant.
  char buf[24];
  now.strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ");
  this->last_seen_text_sensor_->publish_state(buf);
#endif
}

uint32_t OcleanHub::flush_pending_writes_() {
  if (this->pending_writes_.empty())
    return 0;
  // Config writes go to the characteristic named by the active profile (the
  // main tx char on every active profile).
  uint16_t write_handle = (this->profile_->config_write_target == WriteTarget::TX_SESSION) ? this->tx_session_handle_
                                                                                           : this->tx_main_handle_;
  uint32_t offset = 0;
  for (auto &pw : this->pending_writes_) {
    // Move into the scheduled callback so the bytes outlive the queue clear
    // below. Nameless timeout: these are fire-once and must not cancel one
    // another, so they cannot share a name.
    std::vector<uint8_t> bytes = std::move(pw.bytes);
    const char *name = pw.name;
    WriteKind kind = pw.kind;
    this->set_timeout(offset, [this, write_handle, kind, name, bytes = std::move(bytes)]() mutable {
      // The watchdog can disconnect before this fires. Drop the write unless
      // the link is still in the query phase, so it never lands on a dead or
      // freshly reopened connection.
      if (this->state_ != State::POLLING) {
        ESP_LOGD(TAG, "[%s] dropping queued write %s, no longer polling", this->parent_->address_str(), name);
        return;
      }
      if (kind == WriteKind::CLOCK) {
        // a queued write can wait out a whole poll interval, so resample here
        std::vector<uint8_t> fresh;
        if (this->build_clock_command_(&fresh))
          bytes = std::move(fresh);
      }
      this->write_raw_(write_handle, bytes.data(), bytes.size(), name);
    });
    offset += PENDING_WRITE_STAGGER_MS;
  }
  ESP_LOGI(TAG, "[%s] flushing %u queued command(s)", this->parent_->address_str(),
           (unsigned)this->pending_writes_.size());
  this->pending_writes_.clear();
  return offset;
}

void OcleanHub::handle_battery_(const uint8_t *data, size_t len) {
  uint8_t level = 0;
  if (!parse_battery_level(data, len, &level)) {
    ESP_LOGW(TAG, "[%s] battery payload invalid (len=%u): %s", this->parent_->address_str(), (unsigned)len,
             format_hex_pretty(data, len).c_str());
    return;
  }
  ESP_LOGD(TAG, "[%s] battery %u%%", this->parent_->address_str(), level);
  this->publish_(this->battery_sensor_, level);
  this->got_battery_ = true;
  this->maybe_finish_poll_();
}

void OcleanHub::handle_dis_read_(uint16_t uuid16, const uint8_t *data, size_t len) {
  // A device may answer a DIS read with status OK but a null value pointer;
  // std::string(nullptr, 0) is undefined, so reject it like every other reader.
  if (data == nullptr)
    return;
  // len comes straight from the device (value_len), so clamp it before building
  // the string to bound the heap a hostile peripheral could otherwise pin.
  size_t n = std::min(len, (size_t)MAX_DIS_STRING_LEN);
  std::string s(reinterpret_cast<const char *>(data), n);
  // DIS strings come from an unauthenticated peripheral; keep printable ASCII only.
  s.erase(std::remove_if(s.begin(), s.end(),
                         [](char c) {
                           auto u = (unsigned char)c;
                           return u < 0x20 || u > 0x7E;
                         }),
          s.end());
  switch (uuid16) {
    case DIS_MODEL_UUID16:
      this->model_string_ = s;
      // Select the runtime protocol profile from the model string. Every known
      // model shares the TYPE1 GATT layout, so handle resolution is identical;
      // only command routing, session decode and settings framing differ
      // between profiles.
      this->profile_ = profile_for_model(this->model_string_.c_str(), this->model_string_.size());
      ESP_LOGI(TAG, "[%s] model: %s (profile %s, confidence %u)", this->parent_->address_str(), s.c_str(),
               this->profile_->name, this->profile_->confidence);
      this->publish_(this->model_text_sensor_, s);
      this->got_model_ = true;
      this->dis_cached_ = true;
      this->last_dis_read_ms_ = millis();
      this->maybe_finish_poll_();
      break;
    case DIS_HW_REV_UUID16:
      this->hw_rev_string_ = s;
      ESP_LOGI(TAG, "[%s] hw rev: %s", this->parent_->address_str(), s.c_str());
      this->publish_(this->hw_rev_text_sensor_, s);
      break;
    case DIS_SW_REV_UUID16:
      this->sw_rev_string_ = s;
      ESP_LOGI(TAG, "[%s] sw rev: %s", this->parent_->address_str(), s.c_str());
      this->publish_(this->sw_rev_text_sensor_, s);
      break;
    default:
      break;
  }
}

void OcleanHub::publish_session_record_(const SessionRecord &r, bool partial) {
  // Score is 0-100; a spoofed byte outside that (0xFF already means "no score")
  // is clamped so downstream stats never see an out-of-range reading.
  float score = NAN;
  if (!partial && r.has_score)
    score = (float)(r.score > 100 ? 100 : r.score);
  this->publish_(this->session_score_sensor_, score);
  // Durations are 16-bit fields from the peer; bound them like score so a
  // spoofed 65535 s never reaches the entities or recorder statistics.
  this->publish_(this->session_duration_sensor_, (float)clamp_session_duration(r.duration_s));
  this->publish_(this->session_valid_duration_sensor_, (float)clamp_session_duration(r.valid_duration_s));
  if (this->session_mode_text_sensor_ != nullptr) {
    // Decode the scheme id into the select's option label; an id outside the
    // preset table falls back to the bare number.
    std::string scheme_name;
#ifdef USE_SELECT
    if (this->scheme_select_ != nullptr)
      scheme_name = this->scheme_select_->name_for_pnum(r.scheme);
#endif
    if (scheme_name.empty())
      scheme_name = to_string((unsigned)r.scheme);
    this->session_mode_text_sensor_->publish_state(scheme_name);
  }
  this->publish_(this->session_coverage_sensor_, session_coverage_percent(r.valid_duration_s, r.duration_s));
  for (size_t i = 0; i < SESSION_ZONES_COUNT; i++)
    this->publish_(this->zone_sensors_[i], partial ? NAN : (float)r.zones[i]);
  if (this->session_time_text_sensor_ != nullptr) {
    // 32, not 24: date fields are unvalidated uint8, three digits each in the
    // worst case (26 bytes with the terminator)
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%04u-%02u-%02u %02u:%02u:%02u", r.year, r.month, r.day, r.hour, r.minute,
             r.second);
    this->session_time_text_sensor_->publish_state(ts_buf);
  }
}

void OcleanHub::schedule_next_session_publish_() {
  if (this->pending_session_publish_.empty())
    return;
  // Publish one queued backfill record per loop iteration. Spacing past the API
  // batch delay guarantees each record's entity states flush (landing their own
  // recorder row) before the next record overwrites them.
  this->set_timeout("session_publish", SESSION_PUBLISH_STAGGER_MS, [this]() {
    if (this->pending_session_publish_.empty())
      return;
    SessionRecord rec = this->pending_session_publish_.front();
    this->pending_session_publish_.erase(this->pending_session_publish_.begin());
    this->publish_session_record_(rec, false);
    this->schedule_next_session_publish_();
  });
}

void OcleanHub::handle_session_notify_(const uint8_t *data, size_t len) {
  // unknown record layout: the dispatcher already hex-dumped the bytes, and
  // decoding them on a guess would publish nonsense
  if (this->profile_->decode_record == nullptr)
    return;
  // past the end of the stream everything on this channel is an enrichment
  // push, not more record bytes
  if (this->session_asm_.complete()) {
    this->handle_enrichment_notify_(data, len);
    return;
  }
  if (this->session_asm_.failed())
    return;
  const int64_t now_local = this->local_now_epoch_();
  // Inline count=0 reply: no unread sessions, but the head of the newest
  // already-read record rides along. Published live and never persisted, since
  // a genuinely new session always arrives as a full ring. The epoch gate stops
  // a repeated inline frame from blanking that ring's score and zones.
  SessionRecord inl;
  if (!this->session_asm_.started() && decode_inline_0307(data, len, &inl)) {
    uint32_t ts = session_record_epoch(inl);
    ESP_LOGI(TAG,
             "[%s] inline newest session %04u-%02u-%02u %02u:%02u:%02u "
             "scheme=%u dur=%us valid=%us (no unread ring)",
             this->parent_->address_str(), inl.year, inl.month, inl.day, inl.hour, inl.minute, inl.second,
             (unsigned)inl.scheme, (unsigned)inl.duration_s, (unsigned)inl.valid_duration_s);
    if (accept_inline_record(ts, this->newest_record_epoch_, now_local)) {
      this->publish_session_record_(inl, true);
      this->newest_record_epoch_ = ts;
    }
    return;
  }
  bool done = this->session_asm_.feed(data, len);
  if (this->session_asm_.failed()) {
    ESP_LOGW(TAG, "[%s] session stream rejected (bad header)", this->parent_->address_str());
    return;
  }
  if (!done)
    return;

  uint16_t count = this->session_asm_.record_count();
  ESP_LOGD(TAG, "[%s] session stream complete: %u records", this->parent_->address_str(), (unsigned)count);
  // one decode pass; every decision below, newest included, runs on this vector
  std::vector<SessionRecord> records;
  records.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    SessionRecord rec;
    if (this->session_asm_.record(i, &rec))
      records.push_back(rec);
  }
  SessionIngestPlan plan =
      plan_session_ingest(records, this->last_session_emitted_, this->newest_record_epoch_, now_local);

  for (const auto &rec : plan.implausible) {
    ESP_LOGW(TAG, "[%s] dropping session dated %04u-%02u-%02u: implausibly future", this->parent_->address_str(),
             rec.year, rec.month, rec.day);
  }
  // events are not batched, entity state is. Hence the one-per-loop publish below.
  for (const auto &rec : plan.to_publish)
    this->emit_session_event_(rec, session_record_epoch(rec));
  if (plan.new_watermark > this->last_session_emitted_) {
    this->last_session_emitted_ = plan.new_watermark;
    this->session_wm_pref_.save(&this->last_session_emitted_);
  }
  ESP_LOGD(TAG, "[%s] session events emitted: %u (watermark ts=%u)", this->parent_->address_str(),
           (unsigned)plan.to_publish.size(), (unsigned)this->last_session_emitted_);

  if (plan.have_newest) {
    const SessionRecord &r = plan.newest;
    char score_buf[8];
    if (r.has_score) {
      snprintf(score_buf, sizeof(score_buf), "%u", (unsigned)r.score);
    } else {
      score_buf[0] = '-';
      score_buf[1] = '\0';
    }
    ESP_LOGD(TAG,
             "[%s] newest session %04u-%02u-%02u %02u:%02u:%02u "
             "scheme=%u dur=%us valid=%us score=%s",
             this->parent_->address_str(), r.year, r.month, r.day, r.hour, r.minute, r.second, (unsigned)r.scheme,
             (unsigned)r.duration_s, (unsigned)r.valid_duration_s, score_buf);

    // Persisted because the brush never resends an already-read ring: without
    // this a reboot blanks the session entities until the next brushing.
    if (plan.persist_newest) {
      PersistedSession ps{PERSISTED_SESSION_MAGIC, PERSISTED_SESSION_VERSION, 0, r};
      this->session_last_pref_.save(&ps);
      this->newest_record_epoch_ = plan.new_newest_epoch;
    }

    // bytes past the mapped offsets are still unidentified. Indexed off the
    // reassembler, not the plan, so a failed decode only mislabels this line.
    int newest_slot = this->session_asm_.newest_index();
    const uint8_t *raw = newest_slot >= 0 ? this->session_asm_.raw_record((uint16_t)newest_slot) : nullptr;
    if (raw != nullptr) {
      ESP_LOGI(TAG, "[%s] newest raw: %s", this->parent_->address_str(),
               format_hex_pretty(raw, SESSION_RECORD_SIZE).c_str());
    }
  }

  // The oldest goes out now, so the common case of one new session costs no
  // extra latency; the rest are staggered, newest last. A re-served ring with
  // nothing new is republished to keep the live state right without adding a
  // history row.
  if (!plan.to_publish.empty()) {
    this->publish_session_record_(plan.to_publish.front(), false);
    this->pending_session_publish_.assign(plan.to_publish.begin() + 1, plan.to_publish.end());
    this->schedule_next_session_publish_();
  } else if (plan.have_newest && plan.newest_plausible) {
    this->publish_session_record_(plan.newest, false);
  }

  // The record stream is in. Hold the link a short while longer for a possible
  // brush-areas enrichment push (021f), which the device sends after the stream
  // only when fresh sessions are present, then disconnect.
  this->cancel_timeout("capture_hold");
  this->set_timeout("enrichment_wait", ENRICHMENT_WAIT_MS, [this]() {
    ESP_LOGI(TAG, "[%s] enrichment window ended", this->parent_->address_str());
    this->capture_active_ = false;
    // maybe_finish_poll_ may have already torn the link down by now. Only act
    // if the link is still up, to avoid a spurious second teardown.
    if (this->state_ != State::IDLE && this->state_ != State::DISCONNECTING)
      this->finish_or_hold_("enrichment window ended");
  });
}

void OcleanHub::emit_session_event_(const SessionRecord &r, uint32_t ts) {
  // The brush clock runs on local wall time, so the record epoch (civil fields
  // read as if UTC) is local-as-UTC. Subtract the current local-to-UTC offset
  // (DST-aware, from the time source) so Home Assistant buckets the session at
  // its true UTC instant instead of one timezone width late. The dedup watermark
  // (the ts argument) stays on the unshifted record epoch, so this conversion
  // does not touch session ordering or persisted state.
  uint32_t event_ts = ts;
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    ESPTime local = this->time_->now();
    ESPTime utc = this->time_->utcnow();
    if (local.is_valid() && utc.is_valid()) {
      event_ts = (uint32_t)((int64_t)ts - (epoch_of(local) - epoch_of(utc)));
    }
  }
#endif
  std::map<std::string, std::string> data;
  data["device"] = this->parent_->address_str();
  data["ts"] = to_string(event_ts);
  data["local"] = str_sprintf("%04u-%02u-%02uT%02u:%02u:%02u", r.year, r.month, r.day, r.hour, r.minute, r.second);
  // Clamp score/coverage/durations the same way the entity publish does: an
  // out-of-range field from a spoofed record must not reach the recorder
  // statistics.
  unsigned score = r.score > 100 ? 100 : r.score;
  data["score"] = r.has_score ? to_string(score) : std::string("-");
  data["duration"] = to_string((unsigned)clamp_session_duration(r.duration_s));
  data["valid"] = to_string((unsigned)clamp_session_duration(r.valid_duration_s));
  data["scheme"] = to_string((unsigned)r.scheme);
  uint32_t coverage =
      r.duration_s > 0 ? (uint32_t)((100UL * (uint32_t)r.valid_duration_s + r.duration_s / 2) / r.duration_s) : 0;
  if (coverage > 100)
    coverage = 100;
  data["coverage"] = to_string(coverage);
  data["zones"] = str_sprintf("%u,%u,%u,%u,%u,%u,%u,%u", r.zones[0], r.zones[1], r.zones[2], r.zones[3], r.zones[4],
                              r.zones[5], r.zones[6], r.zones[7]);
  ESP_LOGI(TAG, "[%s] session event %s score=%s scheme=%s", this->parent_->address_str(), data["local"].c_str(),
           data["score"].c_str(), data["scheme"].c_str());
  // fires whether or not the HA event below is compiled in
  for (auto *trigger : this->session_triggers_)
    trigger->trigger(r);
  // without homeassistant_services the api component turns this call into a
  // static_assert, so gate it and let the firmware build without events
#ifdef USE_API_HOMEASSISTANT_SERVICES
  this->fire_homeassistant_event("esphome.oclean_session", data);
#endif
}

void OcleanHub::handle_enrichment_notify_(const uint8_t *data, size_t len) {
  // Post-stream notify on the session channel: brush-areas push (021f) or meta.
  // This channel is fully peer-controlled, so keep the raw dump at DEBUG rather
  // than amplifying it to INFO. Never observed on the owned Y3P brushes.
  ESP_LOGD(TAG, "[%s] post-stream notify (enrichment) len=%u data=%s", this->parent_->address_str(), (unsigned)len,
           format_hex_pretty(data, len).c_str());
  BrushAreasPush ba;
  if (decode_brush_areas_push(data, len, &ba)) {
    // 8 values x 3 digits + 7 separators = 31. Clamp keeps the arithmetic sound
    // if that ever stops holding.
    char b[48];
    size_t n = 0;
    for (size_t i = 0; i < BRUSH_AREAS_COUNT && n < sizeof(b) - 1; i++) {
      int w = snprintf(b + n, sizeof(b) - n, "%s%u", i ? "," : "", ba.values[i]);
      if (w <= 0)
        break;
      n = std::min(n + (size_t)w, sizeof(b) - 1);
    }
    ESP_LOGD(TAG, "[%s] brush-areas push (left=0-3 right=4-7): [%s]", this->parent_->address_str(), b);
  }
}

void OcleanHub::handle_main_notify_(const uint8_t *data, size_t len) {
  if (len >= 2 && data[0] == 0x03 && data[1] == 0x03) {
    StatusResponse st;
    if (parse_status_response(data, len, &st)) {
      bool charging = status_is_charging(st.charging_raw);
      bool docked = status_is_docked(st.charging_raw);
      ESP_LOGI(TAG, "[%s] status: battery %u%% charging=%s docked=%s (byte2=0x%02X)", this->parent_->address_str(),
               st.battery, ONOFF(charging), ONOFF(docked), st.charging_raw);
      this->round_status_seen_ = true;
      // STATUS carries the same battery byte as the battery characteristic,
      // which is only read at connect; publishing from here keeps the sensor
      // moving on a held link that never re-reads it.
      this->publish_(this->battery_sensor_, (float)st.battery);
      // Remember the dock-presence state so the next adaptive-poll tick picks the
      // fast (docked) or slow (battery) interval. The HA charging sensor below
      // tracks the narrower actively-charging state instead.
      this->charging_last_ = charging;
      this->docked_last_ = docked;
      this->publish_(this->charging_binary_sensor_, charging);
      this->publish_(this->docked_binary_sensor_, docked);
      // While holding the link for a docked brush, any STATUS that reports the
      // brush is off the dock (re-query readback or a spontaneous push the brush
      // sends on a dock-state change) ends the hold and returns to the normal
      // adaptive cadence. A fully-charged brush still on the dock stays held.
      if (this->holding_ && !docked) {
        ESP_LOGI(TAG, "[%s] leaving held mode (off dock)", this->parent_->address_str());
        this->disconnect_();
        return;
      }
    }
  } else if (len >= 2 && data[0] == 0x03 && data[1] == 0x02 &&
             this->profile_->settings_kind == SettingsKind::SETTINGS_TYPE1_34B) {
    // The settings response is a two-frame transfer. Feed both 0302 frames into
    // the reassembler and publish from the 34-byte buffer as fields arrive. Only
    // profiles that use the TYPE1 34-byte settings buffer take this path.
    this->settings_asm_.feed(data, len);
    DeviceSettings ds;
    parse_device_settings(this->settings_asm_.buffer(), &ds);
    if (this->settings_asm_.has_cont()) {
      // Continuation region: device clock, config toggles, brush-head usage.
      ESP_LOGI(TAG,
               "[%s] settings: clock %04u-%02u-%02u %02u:%02u:%02u "
               "over_pressure=%s area_reminder=%s head_days=%u head_sessions=%u "
               "tz=%u head_max=%u lang=%u",
               this->parent_->address_str(), ds.year, ds.month, ds.day, ds.hour, ds.minute, ds.second,
               ONOFF(ds.over_pressure), ONOFF(ds.area_reminder), ds.head_used_days, ds.head_used_times, ds.tz_index,
               ds.head_max, ds.device_language);
      if (this->device_clock_text_sensor_ != nullptr && ds.clock_valid) {
        // clock_valid bounds the fields to two digits, but the compiler cannot
        // see that; 32 keeps -Wformat-truncation quiet
        char clk[32];
        snprintf(clk, sizeof(clk), "%04u-%02u-%02u %02u:%02u:%02u", ds.year, ds.month, ds.day, ds.hour, ds.minute,
                 ds.second);
        this->device_clock_text_sensor_->publish_state(clk);
      }
      this->publish_clock_drift_(ds);
      // Auto-correct the brush clock if it has drifted past the threshold. The
      // brush clock here is freshest; the queued write flushes on the next poll.
      this->maybe_auto_sync_clock_(ds);
#ifdef USE_SWITCH
      if (this->over_pressure_switch_ != nullptr)
        this->over_pressure_switch_->publish_state(ds.over_pressure);
      if (this->area_reminder_switch_ != nullptr)
        this->area_reminder_switch_->publish_state(ds.area_reminder);
#endif
      this->publish_(this->head_used_days_sensor_, (float)clamp_head_counter(ds.head_used_days));
      this->publish_(this->head_used_times_sensor_, (float)clamp_head_counter(ds.head_used_times));
      this->publish_(this->timezone_text_sensor_, timezone_index_to_string(ds.tz_index));
#ifdef USE_SELECT
      if (this->language_select_ != nullptr)
        this->language_select_->publish_language(ds.device_language);
#endif
#ifdef USE_NUMBER
      // The head-replacement number entity advertises a 1-365 day range; a raw
      // readback outside it is logged and not published.
      if (this->head_max_number_ != nullptr) {
        if (ds.head_max >= 1 && ds.head_max <= 365) {
          this->head_max_number_->publish_state((float)ds.head_max);
        } else {
          ESP_LOGD(TAG, "[%s] head_max readback %u outside 1-365, not published", this->parent_->address_str(),
                   (unsigned)ds.head_max);
        }
      }
#endif
    }
    if (this->settings_asm_.has_start()) {
      // Start region: scheme pNum drives the select readback; the config toggles
      // and the raw indices correct their optimistic state.
#ifdef USE_SELECT
      if (this->scheme_select_ != nullptr)
        this->scheme_select_->publish_pnum(ds.scheme_pnum);
#endif
      ESP_LOGI(TAG,
               "[%s] settings: brush_mode=%s volume=%s calendar=%s auto=%s "
               "raise_wake=%s pause=%s fill=%s splash=%s theme=%u vol_idx=%u",
               this->parent_->address_str(), ONOFF(ds.brush_mode_on), ONOFF(ds.volume_enabled),
               ONOFF(ds.calendar_enabled), ONOFF(ds.auto_mode), ONOFF(ds.raise_wake), ONOFF(ds.brush_pause),
               ONOFF(ds.fill_brush), ONOFF(ds.splash_prevent), ds.device_theme, ds.volume_index);
      // Writable config toggles: correct their optimistic state from the buffer.
#ifdef USE_SWITCH
      if (this->brush_pause_switch_ != nullptr)
        this->brush_pause_switch_->publish_state(ds.brush_pause);
      if (this->raise_wake_switch_ != nullptr)
        this->raise_wake_switch_->publish_state(ds.raise_wake);
      if (this->brush_mode_switch_ != nullptr)
        this->brush_mode_switch_->publish_state(ds.brush_mode_on);
#endif
      // Read-only toggles stay binary sensors. fill_brush and auto_mode are here
      // too: the brush rejects their write opcodes, so they are state, not config.
      this->publish_(this->volume_enabled_binary_sensor_, ds.volume_enabled);
      this->publish_(this->calendar_enabled_binary_sensor_, ds.calendar_enabled);
      this->publish_(this->splash_prevent_binary_sensor_, ds.splash_prevent);
      this->publish_(this->fill_brush_binary_sensor_, ds.fill_brush);
      this->publish_(this->auto_mode_binary_sensor_, ds.auto_mode);
      this->publish_(this->device_theme_sensor_, (float)ds.device_theme);
      this->publish_(this->volume_index_sensor_, (float)ds.volume_index);
      this->publish_(this->head_used_time_sensor_, (float)clamp_head_counter(ds.head_used_time));
    }
  } else {
    // Any other frame on the main notify char: device-info replies, command
    // ACKs and anything not yet mapped. Debug-level raw dump keeps ACKs
    // visible when diagnosing a write.
    ESP_LOGD(TAG, "[%s] main notify (unmapped): %s", this->parent_->address_str(),
             format_hex_pretty(data, len).c_str());
  }
}

void OcleanHub::maybe_finish_poll_() {
  // While holding the link across a charging cycle, a spontaneous battery notify
  // must not tear the connection down. Held requery rounds run their own capture
  // window; the hold exits via the charging=false path, never from here.
  if (this->holding_)
    return;
  if (this->capture_active_)
    return;  // hold the link open for the capture window
  if (this->got_battery_ && this->got_model_) {
    ESP_LOGD(TAG, "[%s] poll complete, disconnecting", this->parent_->address_str());
    this->disconnect_();
  }
}

void OcleanHub::set_custom_scheme_param(uint8_t kind, uint8_t index, uint8_t value, bool resend) {
#ifndef USE_SELECT
  // the parameters only reach the brush through the select; without it the
  // number entities are inert
  (void)kind;
  (void)index;
  (void)value;
  (void)resend;
#else
  if (this->scheme_select_ == nullptr)
    return;
  this->scheme_select_->set_custom_param(kind, index, value);
  if (!resend || !this->scheme_select_->custom_active())
    return;
  // Debounce: slider edits fire one control() per step, but only the final
  // program needs to reach the brush.
  this->set_timeout("custom-scheme-resend", CUSTOM_SCHEME_DEBOUNCE_MS, [this]() {
    if (this->scheme_select_ != nullptr && this->scheme_select_->custom_active())
      this->scheme_select_->send_custom_program();
  });
#endif
}

void OcleanHub::trigger_session_capture() {
  if (!this->ble_user_enabled_) {
    ESP_LOGW(TAG, "[%s] session capture ignored: BLE user-disabled", this->parent_->address_str());
    return;
  }
  bool connected = this->node_state == espbt::ClientState::ESTABLISHED;
  if (!connected) {
    ESP_LOGI(TAG, "[%s] session capture armed, starting poll cycle", this->parent_->address_str());
    this->capture_armed_ = true;
    // not a scheduled poll, so the cadence bookkeeping stays untouched
    if (!this->parent_->enabled)
      this->begin_connect_cycle_(false);
    return;
  }
  // Connected. Only run the query directly when the cycle has settled into
  // POLLING; firing query_device_ during DISCOVERING (settle still pending)
  // would double-run it and reset the assemblers mid-stream. In that case just
  // arm capture and let the settle-driven query_device_ pick it up.
  if (this->state_ == State::POLLING) {
    if (this->query_round_open_()) {
      ESP_LOGI(TAG, "[%s] session capture ignored: query round already open", this->parent_->address_str());
      return;
    }
    this->query_device_(true);
  } else {
    this->capture_armed_ = true;
  }
}

// Permanent dev / diagnostic hook behind the hidden-by-default poll-now button:
// force an immediate full poll cycle, bypassing the adaptive off-dock gate.
void OcleanHub::trigger_immediate_poll() {
  if (!this->ble_user_enabled_) {
    ESP_LOGW(TAG, "[%s] poll-now ignored: BLE user-disabled", this->parent_->address_str());
    return;
  }
  // If the link is already up and settled (e.g. held while docked), run the
  // query directly on it instead of trying to start a new cycle: update() would
  // skip while connected. Same gate as trigger_session_capture.
  if (this->state_ == State::POLLING) {
    if (this->query_round_open_()) {
      ESP_LOGI(TAG, "[%s] poll-now ignored: query round already open", this->parent_->address_str());
      return;
    }
    ESP_LOGI(TAG, "[%s] poll-now pressed, querying on live link", this->parent_->address_str());
    this->query_device_(true);
    return;
  }
  // Marking a poll pending lifts the adaptive off-dock gate; update() still
  // skips on its own if a poll cycle is already active.
  this->poll_pending_ = true;
  // Run this cycle in capture mode so the link stays open for the full capture
  // hold and any late or pushed response is logged.
  this->capture_armed_ = true;
  ESP_LOGI(TAG, "[%s] poll-now pressed, starting poll cycle (capture hold armed)", this->parent_->address_str());
  this->update();
}

void OcleanHub::query_device_(bool capture_mode) {
  ESP_LOGD(TAG, "[%s] querying status + settings, then session download", this->parent_->address_str());
  // keeps the link past the query window, for the replies and the record stream
  this->capture_active_ = true;
  this->round_status_seen_ = false;
  this->notify_count_this_round_ = 0;
  this->notify_flood_warned_ = false;
  this->session_asm_.reset();
  this->settings_asm_.reset();
  // Writes go first and the reads start past them, so the readback reflects the
  // new state rather than the old.
  uint32_t base = this->flush_pending_writes_();
  // 500 ms apart: Write With Response allows one outstanding write.
  //
  // The scheduler keys const char* timeouts by pointer identity, so a reused
  // buffer would make every query cancel the previous one and leave only the
  // last running. String literals have distinct stable addresses.
  static const char *const QUERY_TIMERS[] = {"query0", "query1", "query2", "query3",
                                             "query4", "query5", "query6", "query7"};
  const uint8_t query_timer_count = sizeof(QUERY_TIMERS) / sizeof(QUERY_TIMERS[0]);
  const OcleanProfile *prof = this->profile_;
  for (uint8_t i = 0; i < prof->query_cmd_count; i++) {
    if (i >= query_timer_count) {
      // out of distinct names: a shared one would cancel an earlier query
      ESP_LOGE(TAG,
               "[%s] profile has %u query commands, only %u timer slots; "
               "skipping the rest",
               this->parent_->address_str(), (unsigned)prof->query_cmd_count, (unsigned)query_timer_count);
      break;
    }
    const ProfileCmd &qc = prof->query_cmds[i];
    uint16_t handle = (qc.target == WriteTarget::TX_SESSION) ? this->tx_session_handle_ : this->tx_main_handle_;
    const uint8_t *bytes = qc.bytes;
    uint8_t len = qc.len;
    const char *wname = qc.name;
    const char *tname = QUERY_TIMERS[i];
    this->set_timeout(tname, base + uint32_t(i) * QUERY_STAGGER_MS, [this, handle, bytes, len, wname]() {
      // Skip the query if the watchdog tore the link down before it fired.
      if (this->state_ != State::POLLING)
        return;
      this->write_raw_(handle, bytes, len, wname);
    });
  }
  // read-only; its reply falls through to the main-notify dump
  this->set_timeout("query_info", base + uint32_t(prof->query_cmd_count) * QUERY_STAGGER_MS, [this]() {
    // Skip the query if the watchdog tore the link down before it fired.
    if (this->state_ != State::POLLING)
      return;
    static const uint8_t device_info_cmd[] = {0x02, 0x02};
    this->write_raw_(this->tx_main_handle_, device_info_cmd, sizeof(device_info_cmd), "DEVICE_INFO");
  });
  // Capture mode only extends the hold below; every read query above already
  // runs on a normal poll. The longer window catches late or pushed responses.
  uint32_t hold_ms = capture_mode ? CAPTURE_HOLD_MS : POLL_QUERY_HOLD_MS;
  this->set_timeout("capture_hold", base + hold_ms, [this]() {
    ESP_LOGI(TAG, "[%s] query window ended", this->parent_->address_str());
    this->capture_active_ = false;
    this->finish_or_hold_("query window ended");
  });
}

void OcleanHub::finish_or_hold_(const char *reason) {
  // Default end of a query window: drop the link. Hold it open instead only when
  // the option is set, BLE is user-enabled, and this round's STATUS reply says
  // the brush is docked (still charging or fully charged). A round with no
  // STATUS reply disconnects rather than re-holds on a stale dock state, so a
  // brush that goes silent on a held link cannot pin a BLE slot forever.
  // Holding off the dock would drain the brush battery, which the
  // connect-poll-disconnect cycle exists to avoid.
  if (should_hold_link(this->hold_while_docked_, this->ble_user_enabled_, this->docked_last_,
                       this->round_status_seen_)) {
    if (!this->holding_)
      ESP_LOGI(TAG, "[%s] holding connection while docked (%s)", this->parent_->address_str(), reason);
    this->enter_hold_();
    return;
  }
  // If a hold was active and the brush is no longer docked (or the option was
  // turned off), this disconnect returns it to the normal cadence.
  this->disconnect_();
}

void OcleanHub::enter_hold_() {
  this->holding_ = true;
  // The whole-poll watchdog guards the connect-poll-disconnect cycle; a held link
  // is intentionally long-lived, so disarm it for the held period. Each re-query
  // round re-arms its own watchdog.
  this->cancel_timeout("poll_watchdog");
  this->capture_active_ = false;
  // A completed poll: stamp the cadence bookkeeping exactly as a normal cycle end
  // does, so the adaptive timer treats this as a real poll.
  this->poll_pending_ = false;
  this->last_poll_ms_ = millis();
  // Re-query on the live link one charging interval out. Cancelled on every exit.
  this->clear_hold_();
  this->set_timeout("hold_requery", this->charging_interval_ms_, [this]() { this->held_requery_(); });
}

void OcleanHub::held_requery_() {
  // Held link still up? If a disconnect slipped in, bail; disconnect_ already
  // cleared the timer and flag.
  if (!this->holding_ || this->node_state != espbt::ClientState::ESTABLISHED)
    return;
  // Writes queued while a round is open flush at the start of the next one.
  if (this->query_round_open_())
    return;
  ESP_LOGI(TAG, "[%s] held re-query", this->parent_->address_str());
  // Re-arm a watchdog for this round only: if the re-query hangs, disconnect and
  // fall back to the normal cadence rather than leave a zombie held link.
  this->set_timeout("poll_watchdog", WHOLE_POLL_TIMEOUT_MS, [this]() {
    ESP_LOGW(TAG, "[%s] held re-query timed out, leaving held mode", this->parent_->address_str());
    // a brush that goes quiet on a held link is what this warning is for
    this->status_set_warning("held re-query timed out");
    this->disconnect_();
  });
  // query_device_ resets both assemblers and re-issues the read set, so it is
  // safe to run again on the same connection. It schedules a fresh query window
  // that funnels back through finish_or_hold_, re-entering the hold while docked.
  this->query_device_(false);
}

void OcleanHub::clear_hold_() {
  this->cancel_timeout("hold_requery");
}

}  // namespace oclean
}  // namespace esphome

#endif  // USE_ESP32
