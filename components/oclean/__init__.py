import logging

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.components import ble_client, time
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ICON,
    CONF_ID,
    CONF_NAME,
    CONF_TIME_ID,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE

_LOGGER = logging.getLogger(__name__)

DOMAIN = "oclean"

CODEOWNERS = ["@dzikus"]
DEPENDENCIES = ["ble_client"]
# The read-out platforms carry every reading the component exists for, so the hub
# always needs them. Control platforms are pulled in only by declaring them, so a
# read-only install does not pay for switch/select/number/button.
AUTO_LOAD = [
    "sensor",
    "binary_sensor",
    "text_sensor",
]
MULTI_CONF = True

CONF_OCLEAN_ID = "oclean_id"
CONF_EXPOSE_DEV_SENSORS = "expose_dev_sensors"
CONF_READ_ONLY = "read_only"

# Only reached when the node offset cannot be derived; the hub normally picks
# the index from its own DST-aware offset. Wire value is 1-based into the
# device's 33-entry GMT table.
CONF_TZINDEX = "tzindex"
DEFAULT_TZINDEX = 16

CONF_AUTO_SYNC_TIME = "auto_sync_time"
CONF_SYNC_DRIFT_THRESHOLD = "sync_drift_threshold"
DEFAULT_SYNC_DRIFT_THRESHOLD = "120s"

CONF_UPDATE_INTERVAL = "update_interval"

# Equal to update_interval means fixed-interval polling.
CONF_CHARGING_INTERVAL = "charging_interval"

CONF_HOLD_CONNECTION_WHILE_DOCKED = "hold_connection_while_docked"

CONF_NAME_PREFIX = "name_prefix"

# Picks the entity set at build time. The protocol profile still comes from the
# model id the brush reports, so a mismatch only costs entities, not data.
CONF_MODEL = "model"
MODEL_X_PRO_ELITE = "x_pro_elite"
MODEL_X_ULTRA_20 = "x_ultra_20"
DEFAULT_MODEL = MODEL_X_PRO_ELITE

# The port point_cloud_at_node writes into the X Ultra 20 cloud host while
# cloud_receiver is off. With it on, the receiver shares the web server, so the
# button takes that server's port instead.
CONF_CLOUD_RECEIVER_PORT = "cloud_receiver_port"
DEFAULT_CLOUD_RECEIVER_PORT = 8099

# In-node http receiver for the brush's cloud session uploads. It reuses the
# shared ESPHome web server (web_server_base), so a web_server must be
# configured. Off unless set true; the C++ is not compiled in otherwise
# (USE_OCLEAN_CLOUD_RECEIVER).
CONF_CLOUD_RECEIVER = "cloud_receiver"
# A record dated implausibly far in the future comes from a brush whose clock has
# not been corrected yet. On (default) the receiver acks it so the brush erases it
# from its store instead of re-uploading it every connect; off keeps it there for
# inspection. Never published either way.
CONF_CLOUD_DROP_FUTURE = "cloud_drop_future"

# Home Assistant weather entity the cloud receiver answers the X Ultra 20's
# WeatherKit request from (USE_OCLEAN_WEATHER). State and temperature come by
# state subscription; the daily forecast by the weather.get_forecasts action,
# which Home Assistant performs only for a device allowed to perform actions.
CONF_WEATHER = "weather"

# "MM-DD" the apply_birthday button writes as the X Ultra 20 birthday greeting
# date, with gender and age in the same frame. Yaml options baked into the
# firmware, not entities, so none of it reaches the Home Assistant recorder; use
# !secret. Gender codes and the 3-18 age range are what the app sends.
CONF_BIRTHDAY = "birthday"
CONF_GENDER = "gender"
CONF_AGE = "age"
GENDERS = {"unknown": 0, "male": 1, "female": 2}
DEFAULT_GENDER = "unknown"
DEFAULT_AGE = 18
_gender = cv.enum(GENDERS, lower=True)
_age = cv.int_range(min=3, max=18)
_DAYS_IN_MONTH = (31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)

# BluFi Wi-Fi provisioning is off unless this is set true, and the C++ for it is
# not compiled in otherwise (USE_OCLEAN_BLUFI).
CONF_WIFI_PROVISIONING = "wifi_provisioning"
# Wi-Fi credentials the provision button sends to the brush over BluFi. Kept in
# yaml (baked into the firmware), not as entities, so a password never reaches
# the Home Assistant recorder. Left unset they fall back to the node's own wifi:.
CONF_WIFI_SSID = "wifi_ssid"
CONF_WIFI_PASSWORD = "wifi_password"


def resolve_blufi_wifi(hub_ssid, hub_password, wifi_config):
    # pure so it can be tested without a full config
    if hub_ssid is not None:
        return str(hub_ssid), str(hub_password or "")
    for network in (wifi_config or {}).get("networks") or []:
        ssid = network.get("ssid")
        if ssid:
            return str(ssid), str(network.get("password") or "")
    return "", ""


# session record order, bytes 19-22
QUADRANT_POSITIONS = ("upper_left", "lower_left", "upper_right", "lower_right")
_QUADRANT_KEYS = frozenset(f"quadrant_{position}" for position in QUADRANT_POSITIONS)
_X_PRO_ELITE_FLAG_KEYS = frozenset(
    {"volume_enabled", "calendar_enabled", "splash_prevent", "fill_brush"}
)
_X_ULTRA_20_FLAG_KEYS = frozenset(
    {
        "auto_update",
        "network",
        "voice_teaching",
        "wifi_configured",
        "area_guidance",
        "demo_mode",
    }
)
_HEAD_COUNTER_KEYS = frozenset({"head_used_time", "head_used_days", "head_used_times"})
# X Ultra 20 only: cloud host text and the write buttons.
_X_ULTRA_20_TEXT_KEYS = frozenset({"cloud_host"})
_X_ULTRA_20_WRITE_BUTTON_KEYS = frozenset(
    {
        "apply_cloud_host",
        "clear_cloud_host",
        "point_cloud_at_node",
        "provision_wifi",
        "apply_birthday",
        "clear_birthday",
    }
)

# Highest gear a program step may use; the firmware's motor tables end there.
GEAR_MAX = {MODEL_X_PRO_ELITE: 41, MODEL_X_ULTRA_20: 54}

# Per model and platform. "unavailable": rows the model has no data or opcode
# for, never built. "dev": rows with no observable effect on that brush, built
# only with expose_dev_sensors. Every other row is built on every model.
MODEL_ENTITY_SETS = {
    # Settings bytes 3, 4, 8-10 and 13 hold no setting in the firmware: constant
    # zero, copies of bytes 0 and 1, a flag nothing writes, and a pause flag the
    # next session clears. The brush also rejects the auto-mode write.
    MODEL_X_PRO_ELITE: {
        "unavailable": {
            "sensor": frozenset(
                {"device_mode", "mode_number", "running_state", "volume_index"}
            ),
            "binary_sensor": _X_ULTRA_20_FLAG_KEYS
            | _X_PRO_ELITE_FLAG_KEYS
            | {"auto_mode"},
            "switch": frozenset(
                {
                    "auto_mode",
                    "festival_reminder",
                    "voice_prompts",
                    "voice_fast_brushing",
                    "voice_pressure",
                    "voice_teaching",
                    "demo_mode",
                }
            ),
            "text": _X_ULTRA_20_TEXT_KEYS,
            "button": _X_ULTRA_20_WRITE_BUTTON_KEYS,
        },
        "dev": {
            "switch": frozenset({"area_reminder", "brush_pause", "brush_mode"}),
            "button": frozenset({"capture_sessions"}),
        },
    },
    # Settings bytes 0, 1, 3, 8-10 and 13 hold other fields on this brush, its
    # app family has no 0222 / 0209 setter, and the 12-zone map and quadrants of
    # its record are not decoded. Nothing in firmware 0.0.1.6 writes byte 1 or
    # counts head use, so the network flag and the head counters stay zero.
    # Voice teaching and the retail mode are switches here. The eight zones come
    # from the full record only, which BLE never hands over (cloud_receiver).
    MODEL_X_ULTRA_20: {
        "unavailable": {
            "sensor": frozenset({"device_theme", "volume_index"})
            | _QUADRANT_KEYS
            | _HEAD_COUNTER_KEYS,
            "binary_sensor": _X_PRO_ELITE_FLAG_KEYS
            | {"auto_mode", "network", "voice_teaching", "demo_mode"},
            "switch": frozenset({"brush_pause", "brush_mode"}),
            "number": frozenset({"head_max_minutes"}),
            "button": frozenset({"reset_head"}),
        },
        "dev": {
            "button": frozenset({"capture_sessions"}),
        },
    },
}

MODEL_ENTITY_DEFAULTS = {
    MODEL_X_ULTRA_20: {
        "switch": {
            "area_reminder": ("Voice on zone change", "mdi:swap-horizontal"),
        },
        "select": {
            "device_language": ("Language", None),
        },
    },
}

UNIT_DAY = "d"

MIN_UPDATE_INTERVAL_MS = 60000


def _min_interval_validator(field_name):
    def validate(value):
        period = cv.positive_time_period_milliseconds(value)
        if period.total_milliseconds < MIN_UPDATE_INTERVAL_MS:
            raise cv.Invalid(
                f"{field_name} must be at least {MIN_UPDATE_INTERVAL_MS // 1000}s"
            )
        return period

    return validate


# Hidden rather than skipped: raw indices and link state that would crowd a
# dashboard, but stay one click away for debugging.
HIDDEN_SENSOR_KEYS = frozenset(
    {
        "device_theme",
        "volume_index",
        "head_used_time",
        "clock_drift",
        "mode_number",
        "running_state",
    }
    | _QUADRANT_KEYS
)

HIDDEN_BINARY_SENSOR_KEYS = frozenset(
    {
        "connected",
        "auto_mode",
    }
)


def run_data():
    # CORE.data is rebuilt for every compile run; module globals are not, and the
    # dashboard keeps this module loaded across runs.
    return CORE.data.setdefault(DOMAIN, {})


def _hub_configs():
    # Fallback for platform to_code resolving expose_dev_sensors by oclean_id;
    # _hub_conf reads CORE.config first.
    return run_data().setdefault("hub_configs", {})


def all_hubs():
    # hub_index / total_hubs come from this
    return run_data().setdefault("all_hubs", [])


def _hub_conf(hub_id):
    # Must not depend on to_code order: under MULTI_CONF a platform's to_code can
    # run before the hub records itself, which silently dropped dev entities.
    # CORE.config is complete before any to_code runs.
    target = str(hub_id)
    for hub_conf in CORE.config.get(DOMAIN, []):
        if str(hub_conf.get(CONF_ID)) == target:
            return hub_conf
    return _hub_configs().get(target, {})


def register_hub_conf(config):
    _hub_configs()[str(config[CONF_ID])] = config


def final_hub_conf(hub_id):
    # For FINAL_VALIDATE_SCHEMA: CORE.config is only set once validation is over.
    target = str(hub_id)
    for hub_conf in fv.full_config.get().get(DOMAIN, []):
        if str(hub_conf.get(CONF_ID)) == target:
            return hub_conf
    return {}


def hub_expose_dev(hub_id):
    return bool(_hub_conf(hub_id).get(CONF_EXPOSE_DEV_SENSORS, False))


def hub_model(hub_id):
    return str(_hub_conf(hub_id).get(CONF_MODEL, DEFAULT_MODEL))


def dev_keys(model, platform):
    return MODEL_ENTITY_SETS[model]["dev"].get(platform, frozenset())


def hub_builds(hub_id, platform, key):
    if key not in dev_keys(hub_model(hub_id), platform):
        return True
    return hub_expose_dev(hub_id)


def _raw_hubs():
    # Raw, because the prefix is resolved during validation, when CORE.config is
    # still None and the hub blocks may not have been validated yet.
    raw = CORE.raw_config or {}
    hubs = raw.get(DOMAIN)
    if hubs is None:
        return []
    if isinstance(hubs, dict):
        return [hubs]
    return [hub for hub in hubs if isinstance(hub, dict)]


def _raw_hub(hub_id):
    hubs = _raw_hubs()
    if hub_id is None:
        return hubs[0] if len(hubs) == 1 else None
    target = str(hub_id)
    for hub in hubs:
        if str(hub.get(CONF_ID)) == target:
            return hub
    return None


def entity_name_prefix(hub_id):
    # mqtt topics and unique_id are built from the name alone; the api key is a
    # hash of it. No device in either, so two brushes with the same default names
    # collide on mqtt and in any client that keys by name.
    conf = _raw_hub(hub_id)
    if conf is None:
        return ""
    return str(conf.get(CONF_NAME_PREFIX, "")).strip()


def raw_hub_model(hub_id):
    # An invalid value fails the hub schema; falling back here keeps the
    # platform blocks from piling their own errors on top of that one.
    conf = _raw_hub(hub_id) or {}
    model = str(conf.get(CONF_MODEL, DEFAULT_MODEL)).strip().lower()
    return model if model in MODEL_ENTITY_SETS else DEFAULT_MODEL


def inject_entity_defaults(
    config, rows, hidden=frozenset(), opt_in=frozenset(), platform=None
):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    platform_device = config.get(CONF_DEVICE_ID)
    hub_id = config.get(CONF_OCLEAN_ID)
    # Prefixed in validation, not in to_code. The duplicate-name check runs off
    # the entity schema, and the resolved config has to show the names the
    # generated code registers.
    prefix = entity_name_prefix(hub_id)
    model = raw_hub_model(hub_id)
    unavailable = MODEL_ENTITY_SETS[model]["unavailable"].get(platform, frozenset())
    overrides = MODEL_ENTITY_DEFAULTS.get(model, {}).get(platform, {})
    for key, default_name in rows:
        want = config.get(key, ...)
        if key in unavailable:
            # An error, not a warning: the entity would be missing from a node
            # that compiled cleanly, which reads as a bug on the brush side.
            if want is not ... and want is not False:
                if all(
                    key in sets["unavailable"].get(platform, frozenset())
                    for sets in MODEL_ENTITY_SETS.values()
                ):
                    hint = "No supported model has it."
                else:
                    hint = "Or set model: on the hub to the brush you have."
                raise cv.Invalid(
                    f"'{key}' does not exist on model: {model}. Remove it from "
                    f"this {platform} block. {hint}",
                    path=[key],
                )
            config.pop(key, None)
            continue
        if want is False or (want is ... and key in opt_in):
            config.pop(key, None)
            continue
        # is, not ==: a stray 'battery: 1' equals True and must not read as one.
        sub = {} if (want is ... or want is None or want is True) else want
        if not isinstance(sub, dict):
            raise cv.Invalid(
                f"'{key}' takes true, false, or the options for one entity. "
                f"To rename it write 'name: {sub}' under it.",
                path=[key],
            )
        sub = dict(sub)
        name, icon = overrides.get(key, (default_name, None))
        sub.setdefault(CONF_NAME, f"{prefix} {name}" if prefix else name)
        if icon is not None:
            sub.setdefault(CONF_ICON, icon)
        if platform_device is not None and CONF_DEVICE_ID not in sub:
            sub[CONF_DEVICE_ID] = platform_device
        if key in hidden:
            sub.setdefault(CONF_DISABLED_BY_DEFAULT, True)
        config[key] = sub
    return config


def _validate_name_prefix_is_reachable(config):
    # An auto-generated id is not in the raw config, so the prefix could not
    # be matched to the platform blocks and would be dropped silently.
    hubs = _raw_hubs()
    if len(hubs) < 2:
        return config
    if any(CONF_NAME_PREFIX in hub and CONF_ID not in hub for hub in hubs):
        raise cv.Invalid(
            "a hub with name_prefix also needs an explicit id:, so the prefix "
            "can be matched to the platform blocks that name it in oclean_id."
        )
    return config


def _validate_name_prefix(value):
    # It becomes part of every default entity name, and the entity-name validator
    # rejects a slash there for web-server URL compatibility. Catch it here, where
    # the message can name the option.
    if "/" in value:
        raise cv.Invalid("name_prefix must not contain '/'")
    return value


def _validate_auto_sync_time(config):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    # Defaulted here rather than in the schema so an explicit true without a time
    # source still fails loud instead of silently never syncing.
    if CONF_AUTO_SYNC_TIME not in config:
        config[CONF_AUTO_SYNC_TIME] = CONF_TIME_ID in config
    elif config[CONF_AUTO_SYNC_TIME] and CONF_TIME_ID not in config:
        raise cv.Invalid(
            f"{CONF_AUTO_SYNC_TIME} requires {CONF_TIME_ID} (a time: source) to be set"
        )
    return config


def _validate_blufi_wifi(config):
    if CONF_WIFI_PASSWORD in config and CONF_WIFI_SSID not in config:
        raise cv.Invalid(
            f"{CONF_WIFI_PASSWORD} needs {CONF_WIFI_SSID}", path=[CONF_WIFI_PASSWORD]
        )
    if not config.get(CONF_WIFI_PROVISIONING):
        for key in (CONF_WIFI_SSID, CONF_WIFI_PASSWORD):
            if key in config:
                raise cv.Invalid(
                    f"{key} needs {CONF_WIFI_PROVISIONING}: true", path=[key]
                )
    return config


def parse_month_day(value):
    # "MM-DD", one or two digits each, to a day that exists in a leap year
    month, sep, day = str(value).partition("-")
    if (
        not sep
        or not (1 <= len(month) <= 2 and month.isdigit())
        or not (1 <= len(day) <= 2 and day.isdigit())
    ):
        raise cv.Invalid(f"{CONF_BIRTHDAY} takes MM-DD, e.g. 03-07")
    m, d = int(month), int(day)
    if not 1 <= m <= 12 or not 1 <= d <= _DAYS_IN_MONTH[m - 1]:
        raise cv.Invalid(f"{CONF_BIRTHDAY} is not a calendar day")
    return m, d


def _month_day(value):
    m, d = parse_month_day(value)
    return f"{m:02d}-{d:02d}"


def _validate_birthday(config):
    for key in (CONF_BIRTHDAY, CONF_GENDER, CONF_AGE):
        if key in config and config[CONF_MODEL] != MODEL_X_ULTRA_20:
            raise cv.Invalid(
                f"{key} goes to the birthday greeting, which only the "
                f"{MODEL_X_ULTRA_20} shows",
                path=[key],
            )
    return config


def _weather_entity(value):
    value = cv.entity_id(value)
    if not value.startswith("weather."):
        raise cv.Invalid(f"{CONF_WEATHER} takes a weather.* entity, not '{value}'")
    return value


def _validate_weather(config):
    if CONF_WEATHER not in config:
        return config
    if not config.get(CONF_CLOUD_RECEIVER):
        raise cv.Invalid(
            f"{CONF_WEATHER} is answered by the cloud receiver; set "
            f"{CONF_CLOUD_RECEIVER}: true",
            path=[CONF_WEATHER],
        )
    if CONF_TIME_ID not in config:
        raise cv.Invalid(
            f"{CONF_WEATHER} needs {CONF_TIME_ID}: the answer picks today's or "
            f"tomorrow's forecast by the node's local date",
            path=[CONF_WEATHER],
        )
    return config


def _validate_adaptive_poll(config):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    # The docked cadence is the fast one, so it must not exceed update_interval.
    # Lowering only update_interval is a reasonable thing to do, so clamp instead
    # of erroring: equal intervals degenerate to fixed-interval polling.
    ci = config.get(CONF_CHARGING_INTERVAL)
    if (
        ci is not None
        and ci.total_milliseconds > config[CONF_UPDATE_INTERVAL].total_milliseconds
    ):
        config[CONF_CHARGING_INTERVAL] = config[CONF_UPDATE_INTERVAL]
    return config


oclean_ns = cg.esphome_ns.namespace("oclean")
OcleanHub = oclean_ns.class_("OcleanHub", ble_client.BLEClientNode, cg.PollingComponent)

BrushModel = oclean_ns.enum("BrushModel", is_class=True)
MODELS = {
    MODEL_X_PRO_ELITE: BrushModel.X_PRO_ELITE,
    MODEL_X_ULTRA_20: BrushModel.X_ULTRA_20,
}

SessionRecord = oclean_ns.struct("SessionRecord")
SessionRecordConstRef = SessionRecord.operator("const").operator("ref")
OcleanSessionTrigger = oclean_ns.class_(
    "OcleanSessionTrigger", automation.Trigger.template(SessionRecordConstRef)
)

CONF_ON_SESSION = "on_session"

OCLEAN_COMPONENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OCLEAN_ID): cv.use_id(OcleanHub),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OcleanHub),
            cv.Optional(CONF_MODEL, default=DEFAULT_MODEL): cv.enum(MODELS, lower=True),
            cv.Optional(CONF_EXPOSE_DEV_SENSORS, default=False): cv.boolean,
            cv.Optional(CONF_READ_ONLY, default=False): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL, default="3600s"): _min_interval_validator(
                "update_interval"
            ),
            cv.Optional(
                CONF_CHARGING_INTERVAL, default="600s"
            ): _min_interval_validator(CONF_CHARGING_INTERVAL),
            cv.Optional(CONF_HOLD_CONNECTION_WHILE_DOCKED, default=True): cv.boolean,
            cv.Optional(
                CONF_CLOUD_RECEIVER_PORT, default=DEFAULT_CLOUD_RECEIVER_PORT
            ): cv.port,
            cv.Optional(CONF_CLOUD_RECEIVER, default=False): cv.boolean,
            cv.Optional(CONF_CLOUD_DROP_FUTURE, default=True): cv.boolean,
            cv.Optional(CONF_WEATHER): _weather_entity,
            cv.Optional(CONF_BIRTHDAY): cv.sensitive(_month_day),
            cv.Optional(CONF_GENDER): cv.sensitive(_gender),
            cv.Optional(CONF_AGE): cv.sensitive(_age),
            cv.Optional(CONF_WIFI_PROVISIONING, default=False): cv.boolean,
            cv.Optional(CONF_WIFI_SSID): cv.string,
            cv.Optional(CONF_WIFI_PASSWORD): cv.sensitive(cv.string),
            cv.Optional(CONF_NAME_PREFIX): cv.All(
                cv.string_strict, cv.Length(max=48), _validate_name_prefix
            ),
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_TZINDEX, default=DEFAULT_TZINDEX): cv.int_range(
                min=1, max=33
            ),
            # No schema default: _validate_auto_sync_time fills it in based on
            # whether a time source is configured.
            cv.Optional(CONF_AUTO_SYNC_TIME): cv.boolean,
            cv.Optional(
                CONF_SYNC_DRIFT_THRESHOLD, default=DEFAULT_SYNC_DRIFT_THRESHOLD
            ): cv.positive_time_period_seconds,
            # Per-session automation hook: fires for each newly downloaded
            # session, independent of the Home Assistant event.
            cv.Optional(CONF_ON_SESSION): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OcleanSessionTrigger),
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA),
    _validate_auto_sync_time,
    _validate_adaptive_poll,
    _validate_name_prefix_is_reachable,
    _validate_blufi_wifi,
    _validate_weather,
    _validate_birthday,
    cv.require_esphome_version(2026, 2, 0),
)


# Any of these defines USE_API_HOMEASSISTANT_SERVICES on its own, which is what
# the session event needs, so a config using one gets the events with
# homeassistant_services left at its default.
_HA_SERVICE_ACTIONS = frozenset(
    {"homeassistant.service", "homeassistant.event", "homeassistant.tag_scanned"}
)


def _uses_ha_service_action(node):
    if isinstance(node, dict):
        return any(
            key in _HA_SERVICE_ACTIONS or _uses_ha_service_action(value)
            for key, value in node.items()
        )
    if isinstance(node, list):
        return any(_uses_ha_service_action(item) for item in node)
    return False


def _warn_if_session_events_unavailable(config):
    # Session events go out through fire_homeassistant_event, which the api
    # component only compiles in when the Home Assistant service path is enabled.
    # The firmware builds either way; without it the events are simply not sent.
    full = fv.full_config.get()
    api_conf = full.get("api")
    if api_conf is None or api_conf.get("homeassistant_services", False):
        return config
    # The flag is not the only way in. An automation anywhere in the config that
    # calls a homeassistant.* action turns the same path on, and warning then
    # would claim the events are gone while they are being sent.
    if _uses_ha_service_action(full):
        return config
    if run_data().get("ha_services_warned"):
        return config
    run_data()["ha_services_warned"] = True
    _LOGGER.warning(
        "oclean: 'api:' has no 'homeassistant_services: true' and nothing else in "
        "this configuration calls a homeassistant.* action, so the "
        "esphome.oclean_session events (per-session history) will not be sent. "
        "Entities and long-term statistics are unaffected."
    )
    return config


def _one_hub_per_ble_client(config):
    claimed = {}
    for hub in fv.full_config.get().get(DOMAIN, []):
        ble_id = str(hub[ble_client.CONF_BLE_CLIENT_ID])
        hub_id = str(hub[CONF_ID])
        if ble_id in claimed:
            raise cv.Invalid(
                f"oclean '{hub_id}' and '{claimed[ble_id]}' both use ble_client "
                f"'{ble_id}'. Give each brush its own ble_client."
            )
        claimed[ble_id] = hub_id
    return config


def _warn_on_shared_default_names(config):
    hub_count = len(fv.full_config.get().get(DOMAIN, []))
    if hub_count > 1 and CONF_NAME_PREFIX not in config:
        _LOGGER.warning(
            "oclean hub '%s' has no name_prefix and %d hubs are configured. "
            "Their entities keep the same default names, so they share api keys "
            "and mqtt topics, and only a client that reads device_id can tell "
            "the brushes apart. Set name_prefix per hub to give each brush its "
            "own names, or name_prefix: '' to keep the current ones and silence "
            "this.",
            config[CONF_ID],
            hub_count,
        )
    return config


def _blufi_ssid_available(config, wifi_config):
    # With wifi_provisioning on, there must be an SSID to send: the hub's
    # wifi_ssid, or a wifi: network to fall back on. A node on Ethernet has
    # neither unless wifi_ssid is set, so require it here rather than ship a
    # provision button that can only warn at runtime.
    if not config.get(CONF_WIFI_PROVISIONING):
        return
    ssid, _password = resolve_blufi_wifi(
        config.get(CONF_WIFI_SSID), config.get(CONF_WIFI_PASSWORD), wifi_config
    )
    if not ssid:
        raise cv.Invalid(
            f"oclean '{config[CONF_ID]}' has {CONF_WIFI_PROVISIONING}: true but no "
            f"Wi-Fi SSID to send. Set {CONF_WIFI_SSID} on the hub; this node has "
            f"no wifi: network to fall back on.",
            path=[CONF_WIFI_PROVISIONING],
        )


def _cloud_receiver_needs_web_server(config, full):
    # The receiver reuses the shared ESPHome web server (web_server_base) rather
    # than starting its own, so a web_server must be configured on the node.
    if config.get(CONF_CLOUD_RECEIVER) and "web_server" not in full:
        raise cv.Invalid(
            f"oclean '{config[CONF_ID]}' has {CONF_CLOUD_RECEIVER}: true, which "
            f"serves the brush uploads on the node's web server. Add a "
            f"web_server: block (the receiver reuses it, not a second server).",
            path=[CONF_CLOUD_RECEIVER],
        )


def _weather_needs_api_and_one_hub(config, full):
    # The entity comes from Home Assistant over the native API, and the brush's
    # weather request carries no MAC, so a single hub can answer it.
    if CONF_WEATHER not in config:
        return
    if "api" not in full:
        raise cv.Invalid(
            f"oclean '{config[CONF_ID]}' has {CONF_WEATHER}, which reads the entity "
            f"from Home Assistant: add an api: block",
            path=[CONF_WEATHER],
        )
    for hub in full.get(DOMAIN, []):
        if str(hub[CONF_ID]) == str(config[CONF_ID]):
            return
        if CONF_WEATHER in hub:
            raise cv.Invalid(
                f"oclean '{hub[CONF_ID]}' already answers the brush weather request; "
                f"it names no brush, so only one hub can carry {CONF_WEATHER}",
                path=[CONF_WEATHER],
            )


def _final_validate(config):
    full = fv.full_config.get()
    _one_hub_per_ble_client(config)
    _warn_if_session_events_unavailable(config)
    _blufi_ssid_available(config, full.get("wifi"))
    _cloud_receiver_needs_web_server(config, full)
    _weather_needs_api_and_one_hub(config, full)
    return _warn_on_shared_default_names(config)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    # The PollingComponent tick has to run at the fast (docked) cadence; update()
    # gates the slow off-dock cadence itself. Handing the tick interval to
    # register_component keeps the framework as the single writer of it.
    charging_interval = config[CONF_CHARGING_INTERVAL]
    await cg.register_component(
        var, {**config, CONF_UPDATE_INTERVAL: charging_interval}
    )
    await ble_client.register_ble_node(var, config)
    # Adaptive polling is always on (charging_interval has a default). The timer
    # ticks at the fast charging cadence; update() gates off-dock ticks until the
    # slow battery cadence (update_interval) elapses. When the two intervals are
    # equal this behaves as fixed-interval polling.
    cg.add(var.set_adaptive_poll(True))
    cg.add(var.set_charging_interval(int(charging_interval.total_milliseconds)))
    cg.add(
        var.set_battery_interval(int(config[CONF_UPDATE_INTERVAL].total_milliseconds))
    )
    cg.add(
        var.set_hold_connection_while_docked(config[CONF_HOLD_CONNECTION_WHILE_DOCKED])
    )
    cg.add(var.set_cloud_receiver_port(config[CONF_CLOUD_RECEIVER_PORT]))
    if config[CONF_CLOUD_RECEIVER]:
        cg.add_define("USE_OCLEAN_CLOUD_RECEIVER")
        cg.add(var.set_cloud_receiver_enabled(True))
        cg.add(var.set_cloud_drop_future_enabled(config[CONF_CLOUD_DROP_FUTURE]))
    if (weather := config.get(CONF_WEATHER)) is not None:
        # the API parts a homeassistant sensor and a homeassistant.action with
        # capture_response would enable; json comes with the web server
        cg.add_define("USE_OCLEAN_WEATHER")
        cg.add_define("USE_API_HOMEASSISTANT_STATES")
        cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
        cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES")
        cg.add_define("USE_API_HOMEASSISTANT_ACTION_RESPONSES_JSON")
        cg.add(var.set_weather_entity(weather))
    if (birthday := config.get(CONF_BIRTHDAY)) is not None:
        month, day = parse_month_day(birthday)
        cg.add(var.set_birthday(month, day))
    if CONF_GENDER in config or CONF_AGE in config:
        gender = GENDERS[config.get(CONF_GENDER, DEFAULT_GENDER)]
        cg.add(var.set_user_profile(gender, config.get(CONF_AGE, DEFAULT_AGE)))
    if config[CONF_WIFI_PROVISIONING]:
        cg.add_define("USE_OCLEAN_BLUFI")
        blufi_ssid, blufi_password = resolve_blufi_wifi(
            config.get(CONF_WIFI_SSID),
            config.get(CONF_WIFI_PASSWORD),
            CORE.config.get("wifi"),
        )
        cg.add(var.set_blufi_ssid(blufi_ssid))
        cg.add(var.set_blufi_password(blufi_password))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_expose_dev_sensors(config[CONF_EXPOSE_DEV_SENSORS]))
    cg.add(var.set_read_only(config[CONF_READ_ONLY]))
    cg.add(var.set_tz_index(config[CONF_TZINDEX]))
    cg.add(var.set_auto_sync_time(config[CONF_AUTO_SYNC_TIME]))
    cg.add(
        var.set_sync_drift_threshold(
            int(config[CONF_SYNC_DRIFT_THRESHOLD].total_seconds)
        )
    )
    for conf in config.get(CONF_ON_SESSION, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_session_trigger(trigger))
        await automation.build_automation(trigger, [(SessionRecordConstRef, "x")], conf)
    # Optional local-time source for the sync-clock button.
    if (time_id := config.get(CONF_TIME_ID)) is not None:
        time_var = await cg.get_variable(time_id)
        cg.add(var.set_time(time_var))
    # Track every hub to broadcast the final count back to all of them.
    # The value baked into generated code is the final total. Useful once more
    # than one brush is configured on the same node.
    hubs = all_hubs()
    cg.add(var.set_hub_index(len(hubs)))
    hubs.append(var)
    total = len(hubs)
    for hub in hubs:
        cg.add(hub.set_total_hubs(total))
    register_hub_conf(config)
