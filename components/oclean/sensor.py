import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_DISABLED_BY_DEFAULT,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_DURATION,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_PERCENT,
    UNIT_SECOND,
)

from . import (
    CONF_OCLEAN_ID,
    HIDDEN_SENSOR_KEYS,
    OCLEAN_COMPONENT_SCHEMA,
    apply_name_prefix,
    hub_expose_dev,
    hub_name_prefix,
)

# Raw settings indices with no use on the owned brushes: created only on hubs
# with expose_dev_sensors.
DEV_SENSOR_KEYS = frozenset({"volume_index"})

DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

# (yaml_key, setter_cpp_method, unit, decimals, device_class|None, state_class|None,
#  icon|None, entity_category|None, default_name)
# One row per sensor entity; the to_code loop wires each via its setter.
SENSORS = [
    (
        "battery",
        "set_battery_sensor",
        UNIT_PERCENT,
        0,
        DEVICE_CLASS_BATTERY,
        STATE_CLASS_MEASUREMENT,
        None,
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Battery",
    ),
    (
        "last_session_score",
        "set_session_score_sensor",
        "",
        0,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:tooth-outline",
        None,
        "Score",
    ),
    (
        "last_session_duration",
        "set_session_duration_sensor",
        UNIT_SECOND,
        0,
        DEVICE_CLASS_DURATION,
        STATE_CLASS_MEASUREMENT,
        None,
        None,
        "Duration",
    ),
    (
        "last_session_valid_duration",
        "set_session_valid_duration_sensor",
        UNIT_SECOND,
        0,
        DEVICE_CLASS_DURATION,
        STATE_CLASS_MEASUREMENT,
        None,
        None,
        "Valid duration",
    ),
    # The session scheme id is exposed as the decoded last_session_mode text
    # sensor (mode name), not a raw number.
    (
        "last_session_coverage",
        "set_session_coverage_sensor",
        UNIT_PERCENT,
        0,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:tooth-outline",
        None,
        "Coverage",
    ),
    # Brush-head usage counters, read back from the settings buffer. Cumulative
    # since the last head reset (020F), so total-increasing handles the reset.
    (
        "head_used_days",
        "set_head_used_days_sensor",
        "d",
        0,
        None,
        STATE_CLASS_TOTAL_INCREASING,
        "mdi:toothbrush",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Brush head used days",
    ),
    (
        "head_used_times",
        "set_head_used_times_sensor",
        "",
        0,
        None,
        STATE_CLASS_TOTAL_INCREASING,
        "mdi:counter",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Brush head sessions",
    ),
    # categorical raw indices, hence no state class
    (
        "device_theme",
        "set_device_theme_sensor",
        "",
        0,
        None,
        None,
        "mdi:palette",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Device theme",
    ),
    (
        "volume_index",
        "set_volume_index_sensor",
        "",
        0,
        None,
        None,
        "mdi:volume-high",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Volume index",
    ),
    # unitless on purpose: the unit is unconfirmed, and total-increasing still
    # gives long-term statistics with the head reset absorbed as a counter reset
    (
        "head_used_time",
        "set_head_used_time_sensor",
        "",
        0,
        None,
        STATE_CLASS_TOTAL_INCREASING,
        "mdi:timer-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Brush head used time",
    ),
    # positive = brush ahead. No device class: duration disallows negatives.
    (
        "clock_drift",
        "set_clock_drift_sensor",
        UNIT_SECOND,
        0,
        None,
        STATE_CLASS_MEASUREMENT,
        "mdi:clock-alert-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Clock drift",
    ),
    # gesture array, one sensor per region: left 1-4 then right 5-8, each side
    # ordered upper-outer / upper-inner / lower-outer / lower-inner
    *[
        (
            f"gesture_zone_{i + 1}",
            # No generic setter: to_code wires these via the indexed
            # set_gesture_zone_sensor branch. Sentinel None makes a future
            # copy-paste onto the generic path fail loudly.
            None,
            "",
            0,
            None,
            STATE_CLASS_MEASUREMENT,
            "mdi:gesture-tap",
            None,
            f"Zone {i + 1}",
        )
        for i in range(8)
    ],
]


def _sensor_schema(unit, decimals, device_class, state_class, icon, entity_category):
    kwargs = {"unit_of_measurement": unit, "accuracy_decimals": decimals}
    if device_class is not None:
        kwargs["device_class"] = device_class
    if state_class is not None:
        kwargs["state_class"] = state_class
    if icon is not None:
        kwargs["icon"] = icon
    if entity_category is not None:
        kwargs["entity_category"] = entity_category
    return sensor.sensor_schema(**kwargs)


def _inject_defaults(config):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    platform_dev = config.get(CONF_DEVICE_ID)
    for key, *_, default_name in SENSORS:
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
        if key in HIDDEN_SENSOR_KEYS:
            sub.setdefault(CONF_DISABLED_BY_DEFAULT, True)
        config[key] = sub
    return config


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OCLEAN_COMPONENT_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            **{
                cv.Optional(key): _sensor_schema(unit, dec, dc, sc, icon, ec)
                for (
                    key,
                    _setter,
                    unit,
                    dec,
                    dc,
                    sc,
                    icon,
                    ec,
                    _default_name,
                ) in SENSORS
            },
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])
    platform_device_id = config.get(CONF_DEVICE_ID)
    expose_dev = hub_expose_dev(config[CONF_OCLEAN_ID])
    prefix = hub_name_prefix(config[CONF_OCLEAN_ID])
    for key, setter, *_, default_name in SENSORS:
        if key in DEV_SENSOR_KEYS and not expose_dev:
            continue
        sub_config = apply_name_prefix(config[key], default_name, prefix)
        if platform_device_id is not None and CONF_DEVICE_ID not in sub_config:
            sub_config = {**sub_config, CONF_DEVICE_ID: platform_device_id}
        sens = await sensor.new_sensor(sub_config)
        if key.startswith("gesture_zone_"):
            index = int(key.rsplit("_", 1)[1]) - 1
            cg.add(hub.set_gesture_zone_sensor(index, sens))
        else:
            # Generic setter path requires a setter string. A None sentinel
            # means the row belongs on a dedicated branch above; fail loudly
            # rather than crashing on getattr(hub, None).
            assert setter is not None, f"sensor {key} has no generic setter"
            cg.add(getattr(hub, setter)(sens))
