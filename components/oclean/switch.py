import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_DEVICE_ID, ENTITY_CATEGORY_CONFIG
from esphome.core import CORE

from . import (
    CONF_OCLEAN_ID,
    OCLEAN_COMPONENT_SCHEMA,
    apply_entity_prefix,
    hub_expose_dev,
    hub_name_prefix,
    inject_entity_defaults,
    oclean_ns,
)

_LOGGER = logging.getLogger(__name__)

# Toggles with no observable effect on the owned brushes: created only on hubs
# with expose_dev_sensors so they do not crowd the dashboard as live controls.
DEV_SWITCH_KEYS = frozenset({"area_reminder", "brush_pause", "brush_mode"})

# expose_dev only resolves at to_code, but explicit-vs-auto is only visible in
# the raw validator input, so it has to be recorded here for to_code to warn
# rather than silently drop a switch the user asked for by name.
_EXPLICIT_DEV_SWITCHES = {}
_DEV_RUN_TOKEN = None


def _record_explicit_dev(config):
    global _DEV_RUN_TOKEN
    token = id(CORE.config)
    if token != _DEV_RUN_TOKEN:
        _DEV_RUN_TOKEN = token
        _EXPLICIT_DEV_SWITCHES.clear()
    hub_id = config.get(CONF_OCLEAN_ID)
    hub_key = str(hub_id) if hub_id is not None else "__default__"
    present = {k for k in DEV_SWITCH_KEYS if k in config}
    if present:
        _EXPLICIT_DEV_SWITCHES.setdefault(hub_key, set()).update(present)


DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

OcleanCommandSwitch = oclean_ns.class_(
    "OcleanCommandSwitch", switch.Switch, cg.Parented
)

# Master BLE enable switch: local hub behavior, no opcode written to the brush.
# Component-derived so its setup() applies the restored state after boot.
OcleanBleSwitch = oclean_ns.class_(
    "OcleanBleSwitch", switch.Switch, cg.Parented, cg.Component
)

CONF_BLUETOOTH = "bluetooth"
DEFAULT_BLUETOOTH_NAME = "Bluetooth"

# Hub setters that register a back-pointer so the settings readback can correct
# each switch to the brush's real state.
HUB_SETTERS = {
    "area_reminder": "set_area_reminder_switch",
    "over_pressure": "set_over_pressure_switch",
    "brush_pause": "set_brush_pause_switch",
    "raise_wake": "set_raise_wake_switch",
    "brush_mode": "set_brush_mode_switch",
}

# Default on/off bytes for a config toggle: on 0x01, off 0x00.
ON_DEFAULT = 0x01
OFF_DEFAULT = 0x00

# (yaml_key, opcode_b0, opcode_b1, icon, default_name, log_label, off_value)
# Restore stays disabled on all of them: the brush holds the real state, and a
# boot-time rewrite would be an unsolicited mutation.
SWITCHES = [
    (
        "area_reminder",
        0x02,
        0x0D,
        "mdi:map-marker-check",
        "Area reminder",
        "area-reminder",
        OFF_DEFAULT,
    ),
    (
        "over_pressure",
        0x02,
        0x12,
        "mdi:gauge-low",
        "Over-pressure alert",
        "over-pressure",
        OFF_DEFAULT,
    ),
    (
        "brush_pause",
        0x02,
        0x22,
        "mdi:pause-circle-outline",
        "Brush pause",
        "brush-pause",
        OFF_DEFAULT,
    ),
    (
        "raise_wake",
        0x02,
        0x23,
        "mdi:motion-sensor",
        "Raise to wake",
        "raise-wake",
        OFF_DEFAULT,
    ),
    (
        "brush_mode",
        0x02,
        0x09,
        "mdi:toothbrush-paste",
        "Brush mode",
        "brush-mode",
        0xEC,
    ),
]


_DEFAULT_NAMES = [
    (key, name) for key, _b0, _b1, _icon, name, _label, _off in SWITCHES
] + [(CONF_BLUETOOTH, DEFAULT_BLUETOOTH_NAME)]


def _inject_defaults(config):
    # Note explicit dev switches before auto-create hides which were user-listed.
    _record_explicit_dev(config)
    return inject_entity_defaults(config, _DEFAULT_NAMES)


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OCLEAN_COMPONENT_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            **{
                cv.Optional(key): switch.switch_schema(
                    OcleanCommandSwitch,
                    icon=icon,
                    entity_category=ENTITY_CATEGORY_CONFIG,
                    default_restore_mode="DISABLED",
                )
                for key, _b0, _b1, icon, _default_name, _label, _off in SWITCHES
            },
            # Master BLE enable. RESTORE_DEFAULT_ON so a reboot never silently
            # leaves the brush unreachable when HA has no persisted OFF.
            cv.Optional(CONF_BLUETOOTH): switch.switch_schema(
                OcleanBleSwitch,
                icon="mdi:bluetooth",
                entity_category=ENTITY_CATEGORY_CONFIG,
                default_restore_mode="RESTORE_DEFAULT_ON",
            ),
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])
    config = apply_entity_prefix(
        config, _DEFAULT_NAMES, hub_name_prefix(config[CONF_OCLEAN_ID])
    )
    expose_dev = hub_expose_dev(config[CONF_OCLEAN_ID])
    explicit_dev = _EXPLICIT_DEV_SWITCHES.get(str(config[CONF_OCLEAN_ID]), set())
    for key, b0, b1, _icon, _default_name, label, off_value in SWITCHES:
        if key not in config:
            continue
        if key in DEV_SWITCH_KEYS and not expose_dev:
            # Only auto-created dev rows are dropped quietly; a user who listed
            # one explicitly gets told why it is missing.
            if key in explicit_dev:
                _LOGGER.warning(
                    "oclean: switch '%s' is dev-only and not created; set "
                    "expose_dev_sensors: true on hub '%s' to use it",
                    key,
                    config[CONF_OCLEAN_ID],
                )
            continue
        sw = await switch.new_switch(config[key])
        await cg.register_parented(sw, hub)
        cg.add(sw.set_opcode(b0, b1))
        cg.add(sw.set_label(label))
        # Only override the on/off bytes when the off sentinel is non-default.
        if off_value != OFF_DEFAULT:
            cg.add(sw.set_values(ON_DEFAULT, off_value))
        # Every command switch has a readback setter; index directly so a future
        # switch added without one fails loud instead of silently missing readback.
        cg.add(getattr(hub, HUB_SETTERS[key])(sw))

    bt = config.get(CONF_BLUETOOTH)
    if bt is not None:
        sw = await switch.new_switch(bt)
        await cg.register_parented(sw, hub)
        # Component-derived: needs a setup() slot to apply the restored state.
        await cg.register_component(sw, bt)
