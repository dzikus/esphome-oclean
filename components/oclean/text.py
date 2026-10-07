import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text
from esphome.const import CONF_DEVICE_ID, ENTITY_CATEGORY_CONFIG

from . import (
    CONF_OCLEAN_ID,
    OCLEAN_COMPONENT_SCHEMA,
    OcleanHub,
    hub_builds,
    inject_entity_defaults,
    oclean_ns,
)

DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

OcleanStoredText = oclean_ns.class_(
    "OcleanStoredText", text.Text, cg.Component, cg.Parented.template(OcleanHub)
)

CONF_CLOUD_HOST = "cloud_host"
# firmware stores at most 59 bytes
CLOUD_HOST_MAX_LEN = 59

# (yaml_key, default_name, icon, mode, max_length, hub_setter)
TEXTS = [
    (
        CONF_CLOUD_HOST,
        "Cloud host",
        "mdi:cloud-cog-outline",
        "TEXT",
        CLOUD_HOST_MAX_LEN,
        "set_cloud_host_text",
    ),
]

# Each writes to the brush, so none is created unless the user lists it.
_OPT_IN = frozenset(key for key, *_ in TEXTS)
_DEFAULT_NAMES = [(key, name) for key, name, *_ in TEXTS]


def _inject_defaults(config):
    return inject_entity_defaults(
        config, _DEFAULT_NAMES, opt_in=_OPT_IN, platform="text"
    )


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OCLEAN_COMPONENT_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            **{
                cv.Optional(key): text.text_schema(
                    OcleanStoredText,
                    icon=icon,
                    entity_category=ENTITY_CATEGORY_CONFIG,
                    mode=mode,
                ).extend(cv.COMPONENT_SCHEMA)
                for key, _name, icon, mode, _max, _setter in TEXTS
            },
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])
    for key, _name, _icon, _mode, max_len, setter in TEXTS:
        sub = config.get(key)
        if sub is None:
            continue
        if not hub_builds(config[CONF_OCLEAN_ID], "text", key):
            continue
        var = await text.new_text(sub, min_length=0, max_length=max_len)
        await cg.register_component(var, sub)
        await cg.register_parented(var, hub)
        cg.add(getattr(hub, setter)(var))
