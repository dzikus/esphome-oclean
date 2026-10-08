import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_DEVICE_ID,
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import (
    CONF_OCLEAN_ID,
    HIDDEN_BINARY_SENSOR_KEYS,
    OCLEAN_COMPONENT_SCHEMA,
    hub_builds,
    inject_entity_defaults,
)

DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

# (yaml_key, setter, device_class|None, icon, entity_category, default_name)
# charging is published from the STATUS (0303) response byte 2: 0x01 means on the
# dock / charging, 0x02 means off the dock. Every row the hub's model has is
# auto-created so the entities appear without listing them in the yaml.
# volume_enabled, calendar_enabled, splash_prevent, fill_brush, auto_mode,
# auto_update and network hold no setting on any supported model, and
# voice_teaching and demo_mode are switches on the one model that has them
# (MODEL_ENTITY_SETS).
# Their rows stay so a yaml that names one gets an error that says so.
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
    (
        "voice_teaching",
        "set_voice_teaching_binary_sensor",
        None,
        "mdi:school-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Voice teaching",
    ),
    # On means an SSID is stored; without one the brush never starts Wi-Fi.
    (
        "wifi_configured",
        "set_wifi_configured_binary_sensor",
        None,
        "mdi:wifi-cog",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Wi-Fi provisioned",
    ),
    # One per write the hub keeps on the brush: on while the brush has confirmed
    # the value the yaml holds now (0211 birthday, gender and age; BluFi Wi-Fi;
    # 0233 cloud host).
    (
        "user_info_written",
        "set_user_info_written_binary_sensor",
        None,
        "mdi:account-check",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "User info written",
    ),
    (
        "wifi_written",
        "set_wifi_written_binary_sensor",
        None,
        "mdi:wifi-check",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Wi-Fi written",
    ),
    (
        "cloud_host_written",
        "set_cloud_host_written_binary_sensor",
        None,
        "mdi:cloud-check",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Cloud host written",
    ),
    (
        "area_guidance",
        "set_area_guidance_binary_sensor",
        None,
        "mdi:map-marker-path",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Zone guidance",
    ),
    (
        "demo_mode",
        "set_demo_mode_binary_sensor",
        None,
        "mdi:storefront-outline",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Retail display mode",
    ),
    (
        "auto_update",
        "set_auto_update_binary_sensor",
        None,
        "mdi:update",
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Auto update",
    ),
    (
        "network",
        "set_network_binary_sensor",
        DEVICE_CLASS_CONNECTIVITY,
        None,
        ENTITY_CATEGORY_DIAGNOSTIC,
        "Network",
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


_DEFAULT_NAMES = [(key, name) for key, *_row, name in BINARY_SENSORS]


def _inject_defaults(config):
    return inject_entity_defaults(
        config,
        _DEFAULT_NAMES,
        hidden=HIDDEN_BINARY_SENSOR_KEYS,
        platform="binary_sensor",
    )


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
    for key, setter, *_row in BINARY_SENSORS:
        if key not in config:
            continue
        if not hub_builds(config[CONF_OCLEAN_ID], "binary_sensor", key):
            continue
        bs = await binary_sensor.new_binary_sensor(config[key])
        cg.add(getattr(hub, setter)(bs))
