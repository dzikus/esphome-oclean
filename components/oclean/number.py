import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_DEVICE_ID, ENTITY_CATEGORY_CONFIG

from . import (
    CONF_OCLEAN_ID,
    OCLEAN_COMPONENT_SCHEMA,
    apply_name_prefix,
    hub_name_prefix,
    oclean_ns,
)

DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

OcleanHeadDaysNumber = oclean_ns.class_(
    "OcleanHeadDaysNumber", number.Number, cg.Parented
)
OcleanCustomParamNumber = oclean_ns.class_(
    "OcleanCustomParamNumber", number.Number, cg.Component, cg.Parented
)

CONF_HEAD_MAX_DAYS = "head_max_days"
DEFAULT_HEAD_MAX_DAYS_NAME = "Head replacement days"

# Brush-head reminder period, in days. The command carries the value as a
# two-byte big-endian payload. Range covers the usual replacement windows.
HEAD_DAYS_MIN = 1
HEAD_DAYS_MAX = 365
HEAD_DAYS_STEP = 1

# Parameters of the runtime custom brushing program: per-step gear and
# duration. Values live on the node (flash-persisted) and only reach the brush
# when the Custom scheme is written. The program always has exactly four
# steps, so it fits a single write frame and keeps the brush's four-quadrant
# guidance.
# (yaml_key, default_name, kind, index, unit|None, icon, min, max, step, initial)
CUSTOM_PARAMS = [
    *[
        (
            f"custom_step{i + 1}_gear",
            f"Custom step {i + 1} gear",
            0,
            i,
            None,
            "mdi:speedometer",
            1,
            41,
            1,
            8,
        )
        for i in range(4)
    ],
    *[
        (
            f"custom_step{i + 1}_duration",
            f"Custom step {i + 1} duration",
            1,
            i,
            "s",
            "mdi:timer-outline",
            5,
            120,
            5,
            30,
        )
        for i in range(4)
    ],
]


def _inject_defaults(config):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    platform_dev = config.get(CONF_DEVICE_ID)
    keys = [(CONF_HEAD_MAX_DAYS, DEFAULT_HEAD_MAX_DAYS_NAME)]
    keys += [(row[0], row[1]) for row in CUSTOM_PARAMS]
    for key, default_name in keys:
        sub = config.get(key)
        if sub is None:
            # Auto-create so the entities appear without listing them in the
            # yaml.
            sub = {}
        elif isinstance(sub, dict):
            sub = dict(sub)
        if isinstance(sub, dict):
            sub.setdefault("name", default_name)
            if platform_dev is not None and CONF_DEVICE_ID not in sub:
                sub[CONF_DEVICE_ID] = platform_dev
        config[key] = sub
    return config


def _custom_param_schema(unit, icon):
    kwargs = {"icon": icon, "entity_category": ENTITY_CATEGORY_CONFIG}
    if unit is not None:
        kwargs["unit_of_measurement"] = unit
    return number.number_schema(OcleanCustomParamNumber, **kwargs).extend(
        cv.COMPONENT_SCHEMA
    )


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OCLEAN_COMPONENT_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            cv.Optional(CONF_HEAD_MAX_DAYS): number.number_schema(
                OcleanHeadDaysNumber,
                unit_of_measurement="d",
                icon="mdi:toothbrush",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ).extend(
                {
                    # Box input, not a slider: dragging a slider would fire a
                    # write on every step. A typed value commits one write.
                    cv.Optional("mode", default="BOX"): cv.enum(
                        number.NUMBER_MODES, upper=True
                    ),
                }
            ),
            **{
                cv.Optional(key): _custom_param_schema(unit, icon)
                for key, _name, _kind, _idx, unit, icon, *_ in CUSTOM_PARAMS
            },
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])
    platform_device_id = config.get(CONF_DEVICE_ID)

    prefix = hub_name_prefix(config[CONF_OCLEAN_ID])

    def _with_device(sub):
        if platform_device_id is not None and CONF_DEVICE_ID not in sub:
            return {**sub, CONF_DEVICE_ID: platform_device_id}
        return sub

    sub = config.get(CONF_HEAD_MAX_DAYS)
    if sub is not None:
        sub = apply_name_prefix(sub, DEFAULT_HEAD_MAX_DAYS_NAME, prefix)
        num = await number.new_number(
            _with_device(sub),
            min_value=HEAD_DAYS_MIN,
            max_value=HEAD_DAYS_MAX,
            step=HEAD_DAYS_STEP,
        )
        await cg.register_parented(num, hub)
        cg.add(hub.set_head_max_number(num))

    for (
        key,
        default_name,
        kind,
        idx,
        _unit,
        _icon,
        vmin,
        vmax,
        vstep,
        initial,
    ) in CUSTOM_PARAMS:
        sub = apply_name_prefix(config[key], default_name, prefix)
        num = await number.new_number(
            _with_device(sub), min_value=vmin, max_value=vmax, step=vstep
        )
        await cg.register_component(num, sub)
        await cg.register_parented(num, hub)
        cg.add(num.set_param(kind, idx))
        cg.add(num.set_initial(float(initial)))
