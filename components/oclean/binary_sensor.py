import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_DISABLED_BY_DEFAULT,
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import (
    CONF_OCLEAN_ID,
    HIDDEN_BINARY_SENSOR_KEYS,
    OCLEAN_COMPONENT_SCHEMA,
    apply_name_prefix,
    hub_expose_dev,
    hub_name_prefix,
)

# Settings readbacks with no observable effect on the owned brushes: created
# only on hubs with expose_dev_sensors.
DEV_BINARY_SENSOR_KEYS = frozenset(
    {"volume_enabled", "calendar_enabled", "splash_prevent", "fill_brush"}
)

DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

# (yaml_key, setter, device_class|None, icon, entity_category, default_name)
# charging is published from the STATUS (0303) response byte 2: 0x01 means on the
# dock / charging, 0x02 means off the dock. The rest are config toggles read back
# from the settings buffer (030201) on each poll and have no device class. Every
# row is auto-created so the entities appear without listing them in the yaml.
# fill_brush and auto_mode are read-only: the brush returns a one-byte error stub
# for their write opcodes (0224 / 0225) and the setting never changes, so they are
# surfaced as state, not controls.
BINARY_SENSORS = [
    (
        "charging",
        "set_charging_binary_sensor",
        DEVICE_CLASS_BATTERY_CHARGING,
        "mdi:power-plug",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Charging",
    ),
    # docked is the wider on-dock state from STATUS byte 2 (0x01 charging or 0x03
    # fully charged), separate from the narrower charging sensor above. A dock icon
    # with no device class renders as a clean on/off presence state.
    (
        "docked",
        "set_docked_binary_sensor",
        None,
        "mdi:dock-top",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Docked",
    ),
    # BLE link state, on between the open and disconnect events. Disabled by
    # default: the hub connects only for a few seconds per poll, so this reads
    # off almost always; it is a debug aid, not a status. The last-seen text
    # sensor is the freshness signal users want.
    (
        "connected",
        "set_connected_binary_sensor",
        DEVICE_CLASS_CONNECTIVITY,
        None,
        ENTITY_CATEGORY_DIAGNOSTIC,
        "BLE connected",
    ),
    (
        "volume_enabled",
        "set_volume_enabled_binary_sensor",
        None,
        "mdi:volume-high",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Volume enabled",
    ),
    (
        "calendar_enabled",
        "set_calendar_enabled_binary_sensor",
        None,
        "mdi:calendar-check",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Calendar enabled",
    ),
    (
        "splash_prevent",
        "set_splash_prevent_binary_sensor",
        None,
        "mdi:water-off",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Splash prevention",
    ),
    (
        "fill_brush",
        "set_fill_brush_binary_sensor",
        None,
        "mdi:brush",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Fill brush",
    ),
    (
        "auto_mode",
        "set_auto_mode_binary_sensor",
        None,
        "mdi:autorenew",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Auto mode",
    ),
]


def _binary_sensor_schema(device_class, icon, entity_category):
    kwargs = {}
    if device_class is not None:
        kwargs["device_class"] = device_class
    if icon is not None:
        kwargs["icon"] = icon
    if entity_category is not None:
        kwargs["entity_category"] = entity_category
    return binary_sensor.binary_sensor_schema(**kwargs)


def _inject_defaults(config):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    platform_dev = config.get(CONF_DEVICE_ID)
    for key, _setter, _dc, _icon, _ec, default_name in BINARY_SENSORS:
        sub = config.get(key)
        if sub is None:
            sub = {}
        elif isinstance(sub, dict):
            sub = dict(sub)
        if not isinstance(sub, dict):
            config[key] = sub
            continue
        sub.setdefault("name", default_name)
        if platform_dev is not None and CONF_DEVICE_ID not in sub:
            sub[CONF_DEVICE_ID] = platform_dev
        if key in HIDDEN_BINARY_SENSOR_KEYS:
            sub.setdefault(CONF_DISABLED_BY_DEFAULT, True)
        config[key] = sub
    return config


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OCLEAN_COMPONENT_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            **{
                cv.Optional(key): _binary_sensor_schema(dc, icon, ec)
                for key, _setter, dc, icon, ec, _default_name in BINARY_SENSORS
            },
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])
    platform_device_id = config.get(CONF_DEVICE_ID)
    expose_dev = hub_expose_dev(config[CONF_OCLEAN_ID])
    prefix = hub_name_prefix(config[CONF_OCLEAN_ID])
    for key, setter, _dc, _icon, _ec, default_name in BINARY_SENSORS:
        if key in DEV_BINARY_SENSOR_KEYS and not expose_dev:
            continue
        sub = apply_name_prefix(config[key], default_name, prefix)
        if platform_device_id is not None and CONF_DEVICE_ID not in sub:
            sub = {**sub, CONF_DEVICE_ID: platform_device_id}
        bs = await binary_sensor.new_binary_sensor(sub)
        cg.add(getattr(hub, setter)(bs))
