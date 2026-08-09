import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, time
from esphome.const import CONF_ID, CONF_NAME, CONF_TIME_ID
from esphome.core import CORE

CODEOWNERS = ["@dzikus"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = [
    "sensor",
    "binary_sensor",
    "text_sensor",
    "button",
    "switch",
    "number",
    "select",
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

# Fallback for platform to_code resolving expose_dev_sensors by oclean_id;
# hub_expose_dev reads CORE.config first.
HUB_CONFIGS = {}

# hub_index / total_hubs come from this
_ALL_HUBS = []

_RUN_CONFIG_ID = None


def _reset_run_state():
    # The dashboard keeps this module loaded across compiles, so hub_index and
    # total_hubs would climb run to run. CORE.config is rebuilt per run, so its
    # identity marks the boundary.
    global _RUN_CONFIG_ID
    current = id(CORE.config)
    if current != _RUN_CONFIG_ID:
        _RUN_CONFIG_ID = current
        HUB_CONFIGS.clear()
        _ALL_HUBS.clear()


def _hub_conf(hub_id):
    # Must not depend on to_code order: under MULTI_CONF a platform's to_code can
    # run before the hub fills HUB_CONFIGS, which silently dropped dev entities.
    # CORE.config is complete before any to_code runs.
    target = str(hub_id)
    for hub_conf in CORE.config.get("oclean", []):
        if str(hub_conf.get(CONF_ID)) == target:
            return hub_conf
    return HUB_CONFIGS.get(target, {})


def hub_expose_dev(hub_id):
    return bool(_hub_conf(hub_id).get(CONF_EXPOSE_DEV_SENSORS, False))


def hub_name_prefix(hub_id):
    # An entity name is its identity on mqtt (state topic, discovery topic,
    # unique_id) and hashes into the api key, neither of which carries a device.
    explicit = _hub_conf(hub_id).get(CONF_NAME_PREFIX)
    if explicit is not None:
        return explicit.strip()
    # A lone brush cannot collide with itself, and prefixing it would rename
    # every entity of every existing single-brush install.
    if len(CORE.config.get("oclean", [])) < 2:
        return ""
    return _prefix_from_hub_id(str(hub_id))


def _prefix_from_hub_id(hub_id):
    text = hub_id
    if text.startswith("oclean_"):
        text = text[len("oclean_") :]
    text = text.replace("_", " ").replace("-", " ")
    # Only the leading letter of each word is touched, so an id like oclean_XPro
    # keeps its own capitalisation.
    return " ".join(w[:1].upper() + w[1:] for w in text.split())


def apply_name_prefix(sub_config, default_name, prefix):
    # Matching the injected default is what proves the name was not hand-written.
    if not prefix or sub_config.get(CONF_NAME) != default_name:
        return sub_config
    return {**sub_config, CONF_NAME: f"{prefix} {default_name}"}


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
            cv.Optional(CONF_NAME_PREFIX): cv.All(cv.string_strict, cv.Length(max=48)),
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
        }
    ).extend(ble_client.BLE_CLIENT_SCHEMA),
    _validate_auto_sync_time,
    _validate_adaptive_poll,
    cv.require_esphome_version(2026, 1, 0),
)


async def to_code(config):
    _reset_run_state()
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    # Adaptive polling is always on (charging_interval has a default). The timer
    # ticks at the fast charging cadence; update() gates off-dock ticks until the
    # slow battery cadence (update_interval) elapses. When the two intervals are
    # equal this behaves as fixed-interval polling.
    charging_interval = config[CONF_CHARGING_INTERVAL]
    cg.add(var.set_adaptive_poll(True))
    cg.add(var.set_charging_interval(int(charging_interval.total_milliseconds)))
    cg.add(
        var.set_battery_interval(int(config[CONF_UPDATE_INTERVAL].total_milliseconds))
    )
    # The PollingComponent tick must run at the fast (charging) cadence; update()
    # gates the slow off-dock cadence itself. register_component above already set
    # the tick from CONF_UPDATE_INTERVAL, so override it here. cg.add statements
    # emit in call order, so this deterministically runs after that setter.
    cg.add(var.set_update_interval(charging_interval))
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
    # Optional local-time source for the sync-clock button.
    if (time_id := config.get(CONF_TIME_ID)) is not None:
        time_var = await cg.get_variable(time_id)
        cg.add(var.set_time(time_var))
    # Track every hub to broadcast the final count back to all of them.
    # The value baked into generated code is the final total. Useful once more
    # than one brush is configured on the same node.
    cg.add(var.set_hub_index(len(_ALL_HUBS)))
    _ALL_HUBS.append(var)
    total = len(_ALL_HUBS)
    for hub in _ALL_HUBS:
        cg.add(hub.set_total_hubs(total))
    HUB_CONFIGS[str(config[CONF_ID])] = config
