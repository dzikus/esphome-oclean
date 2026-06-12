import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_DISABLED_BY_DEFAULT,
    DEVICE_CLASS_TIMESTAMP,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import (
    CONF_OCLEAN_ID,
    OCLEAN_COMPONENT_SCHEMA,
)

# Static device identity values and the poll freshness stamp: things a user
# rarely needs on the dashboard; created but disabled by default in Home
# Assistant, same as the connected sensor.
HIDDEN_TEXT_SENSOR_KEYS = frozenset(
    {"model", "hw_revision", "sw_version", "mac_address", "last_seen", "timezone"}
)

DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

# (yaml_key, setter, icon, entity_category|None, default_name, device_class|None)
# model: diagnostic, hidden by default; the raw model id that drives runtime
# profile selection.
# hw_revision / sw_version: diagnostic, read once from the device-information
# service and cached, hidden by default.
# last_session_time: always exposed, the timestamp of the newest brushing
# session pulled from the device buffer.
# last_seen ("Last seen"): stamped with the wall clock each time a poll
# reaches the brush, so the timestamp device class renders it as "x ago" in Home
# Assistant. The freshness signal for the slow connect-poll-disconnect cadence:
# a stale value here means the brush has not been reachable, even though the
# data entities keep their last-read state. Tracks polling only; never indicates
# a clock write to the brush.
TEXT_SENSORS = [
    (
        "model",
        "set_model_text_sensor",
        "mdi:toothbrush-paste",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Model",
        None,
    ),
    (
        "hw_revision",
        "set_hw_revision_text_sensor",
        "mdi:chip",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Hardware revision",
        None,
    ),
    (
        "sw_version",
        "set_sw_version_text_sensor",
        "mdi:package-up",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Software version",
        None,
    ),
    (
        "last_session_time",
        "set_session_time_text_sensor",
        "mdi:clock-outline",
        None,
        "Last session",
        None,
    ),
    (
        "last_session_mode",
        "set_session_mode_text_sensor",
        "mdi:toothbrush",
        None,
        "Last session mode",
        None,
    ),
    (
        "last_seen",
        "set_last_seen_text_sensor",
        "mdi:update",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Last seen",
        DEVICE_CLASS_TIMESTAMP,
    ),
    (
        "device_clock",
        "set_device_clock_text_sensor",
        "mdi:clock-check-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Device clock",
        None,
    ),
    (
        "timezone",
        "set_timezone_text_sensor",
        "mdi:map-clock",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Timezone",
        None,
    ),
    (
        "mac_address",
        "set_mac_text_sensor",
        "mdi:bluetooth",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "MAC address",
        None,
    ),
]


def _inject_defaults(config):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    platform_dev = config.get(CONF_DEVICE_ID)
    for key, _setter, _icon, _ec, default_name, _dc in TEXT_SENSORS:
        sub = config.get(key)
        if sub is None:
            # Auto-create every row so the entities appear without listing
            # them in the yaml, same as the sensor and binary_sensor platforms.
            sub = {}
        elif isinstance(sub, dict):
            sub = dict(sub)
        if not isinstance(sub, dict):
            config[key] = sub
            continue
        sub.setdefault("name", default_name)
        if platform_dev is not None and CONF_DEVICE_ID not in sub:
            sub[CONF_DEVICE_ID] = platform_dev
        if key in HIDDEN_TEXT_SENSOR_KEYS:
            sub.setdefault(CONF_DISABLED_BY_DEFAULT, True)
        config[key] = sub
    return config


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OCLEAN_COMPONENT_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            **{
                cv.Optional(key): text_sensor.text_sensor_schema(
                    icon=icon,
                    **({"entity_category": ec} if ec is not None else {}),
                    **({"device_class": dc} if dc is not None else {}),
                )
                for key, _setter, icon, ec, _default_name, dc in TEXT_SENSORS
            },
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])
    platform_device_id = config.get(CONF_DEVICE_ID)
    for key, setter, _icon, _ec, _default_name, _dc in TEXT_SENSORS:
        sub = config[key]
        if platform_device_id is not None and CONF_DEVICE_ID not in sub:
            sub = {**sub, CONF_DEVICE_ID: platform_device_id}
        ts = await text_sensor.new_text_sensor(sub)
        cg.add(getattr(hub, setter)(ts))
