import logging

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.components import ble_client, time
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_DISABLED_BY_DEFAULT,
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
    }
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


def hub_expose_dev(hub_id):
    return bool(_hub_conf(hub_id).get(CONF_EXPOSE_DEV_SENSORS, False))


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


def entity_name_prefix(hub_id):
    # mqtt topics and unique_id are built from the name alone; the api key is a
    # hash of it. No device in either, so two brushes with the same default names
    # collide on mqtt and in any client that keys by name.
    hubs = _raw_hubs()
    conf = None
    if hub_id is None:
        # A platform block without oclean_id only resolves against a single hub.
        if len(hubs) == 1:
            conf = hubs[0]
    else:
        target = str(hub_id)
        for hub in hubs:
            if str(hub.get(CONF_ID)) == target:
                conf = hub
                break
    if conf is None:
        return ""
    explicit = conf.get(CONF_NAME_PREFIX)
    if explicit is not None:
        return str(explicit).strip()
    # A lone brush cannot collide with itself, and prefixing it would rename
    # every entity of every existing single-brush install.
    if len(hubs) < 2:
        return ""
    hub_key = conf.get(CONF_ID)
    return _prefix_from_hub_id(str(hub_key)) if hub_key is not None else ""


def _prefix_from_hub_id(hub_id):
    text = hub_id
    if text.startswith("oclean_"):
        text = text[len("oclean_") :]
    text = text.replace("_", " ").replace("-", " ")
    # Only the leading letter of each word is touched, so an id like oclean_XPro
    # keeps its own capitalisation.
    return " ".join(w[:1].upper() + w[1:] for w in text.split())


def inject_entity_defaults(config, rows, hidden=frozenset(), opt_in=frozenset()):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    platform_device = config.get(CONF_DEVICE_ID)
    # Prefixed in validation, not in to_code. The duplicate-name check runs off
    # the entity schema, and the resolved config has to show the names the
    # generated code registers.
    prefix = entity_name_prefix(config.get(CONF_OCLEAN_ID))
    for key, default_name in rows:
        want = config.get(key, ...)
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
        sub.setdefault(
            CONF_NAME, f"{prefix} {default_name}" if prefix else default_name
        )
        if platform_device is not None and CONF_DEVICE_ID not in sub:
            sub[CONF_DEVICE_ID] = platform_device
        if key in hidden:
            sub.setdefault(CONF_DISABLED_BY_DEFAULT, True)
        config[key] = sub
    return config


def _validate_hub_ids_are_explicit(config):
    # An auto-generated id does not exist yet in the raw config, so a hub without
    # id: cannot be matched to the platform block referencing it and would keep
    # the bare names. That collides on mqtt and in any client keyed by name, and
    # surfaces as a wall of duplicate-entity errors. One line beats fifty.
    hubs = _raw_hubs()
    if len(hubs) < 2 or all(CONF_ID in hub for hub in hubs):
        return config
    raise cv.Invalid(
        "with more than one oclean hub, every hub needs an explicit id: so its "
        "entities can be named apart (id: brush_a -> 'Brush A Battery'). Set "
        "name_prefix on a hub to choose the wording, or name_prefix: '' to keep "
        "the bare names."
    )


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
            cv.Optional(CONF_EXPOSE_DEV_SENSORS, default=False): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL, default="3600s"): _min_interval_validator(
                "update_interval"
            ),
            cv.Optional(
                CONF_CHARGING_INTERVAL, default="600s"
            ): _min_interval_validator(CONF_CHARGING_INTERVAL),
            cv.Optional(CONF_HOLD_CONNECTION_WHILE_DOCKED, default=True): cv.boolean,
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
    _validate_hub_ids_are_explicit,
    cv.require_esphome_version(2026, 1, 0),
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


def _final_validate(config):
    _one_hub_per_ble_client(config)
    _warn_if_session_events_unavailable(config)
    return config


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
    cg.add(var.set_expose_dev_sensors(config[CONF_EXPOSE_DEV_SENSORS]))
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
