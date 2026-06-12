import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_TIME_ID,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import CORE

from . import (
    CONF_OCLEAN_ID,
    OCLEAN_COMPONENT_SCHEMA,
    hub_expose_dev,
    oclean_ns,
)


def _hub_has_time(hub_id):
    # The sync-clock button writes the brush clock from the hub's time source.
    # Without a time: source it can only warn at runtime, so skip creating it.
    target = str(hub_id)
    for hub_conf in CORE.config.get("oclean", []):
        if str(hub_conf.get(CONF_ID)) == target:
            return CONF_TIME_ID in hub_conf
    return False


DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

OcleanCaptureButton = oclean_ns.class_(
    "OcleanCaptureButton", button.Button, cg.Parented
)
OcleanResetHeadButton = oclean_ns.class_(
    "OcleanResetHeadButton", button.Button, cg.Parented
)
OcleanSyncTimeButton = oclean_ns.class_(
    "OcleanSyncTimeButton", button.Button, cg.Parented
)
OcleanPollNowButton = oclean_ns.class_(
    "OcleanPollNowButton", button.Button, cg.Parented
)

# Dev-gated. Requests a buffered-session download and holds the link open so
# the record stream can be captured into the log.
CONF_CAPTURE_SESSIONS = "capture_sessions"
DEFAULT_CAPTURE_NAME = "Capture sessions"

# Always exposed. Resets the brush-head usage counter (irreversible).
CONF_RESET_HEAD = "reset_head"
DEFAULT_RESET_HEAD_NAME = "Reset brush head"

# Always exposed. Writes the brush real-time clock from the hub time source.
# Button-only: the write happens only on press, never on boot or poll.
CONF_SYNC_TIME = "sync_time"
DEFAULT_SYNC_TIME_NAME = "Sync clock"

# Always created but disabled by default in Home Assistant. Forces an
# immediate full poll cycle (read-only on the brush).
CONF_POLL_NOW = "poll_now"
DEFAULT_POLL_NOW_NAME = "Poll now"


def _inject_defaults(config):
    # Copy before mutating: the validator may run against a shared dict.
    config = dict(config)
    platform_dev = config.get(CONF_DEVICE_ID)
    for key, default_name in (
        (CONF_CAPTURE_SESSIONS, DEFAULT_CAPTURE_NAME),
        (CONF_RESET_HEAD, DEFAULT_RESET_HEAD_NAME),
        (CONF_SYNC_TIME, DEFAULT_SYNC_TIME_NAME),
        (CONF_POLL_NOW, DEFAULT_POLL_NOW_NAME),
    ):
        sub = config.get(key)
        if sub is None:
            # Auto-create so the entities appear without listing them in the
            # yaml; dev-gated rows are still skipped at codegen time.
            sub = {}
        elif isinstance(sub, dict):
            sub = dict(sub)
        if isinstance(sub, dict):
            sub.setdefault("name", default_name)
            if platform_dev is not None and CONF_DEVICE_ID not in sub:
                sub[CONF_DEVICE_ID] = platform_dev
            if key == CONF_POLL_NOW:
                sub.setdefault(CONF_DISABLED_BY_DEFAULT, True)
        config[key] = sub
    return config


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OCLEAN_COMPONENT_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            cv.Optional(CONF_CAPTURE_SESSIONS): button.button_schema(
                OcleanCaptureButton,
                icon="mdi:download",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_RESET_HEAD): button.button_schema(
                OcleanResetHeadButton,
                icon="mdi:restart",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_SYNC_TIME): button.button_schema(
                OcleanSyncTimeButton,
                icon="mdi:clock-check",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_POLL_NOW): button.button_schema(
                OcleanPollNowButton,
                icon="mdi:bluetooth-connect",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])
    expose_dev = hub_expose_dev(config[CONF_OCLEAN_ID])
    platform_device_id = config.get(CONF_DEVICE_ID)

    def _with_dev(sub):
        if platform_device_id is not None and CONF_DEVICE_ID not in sub:
            return {**sub, CONF_DEVICE_ID: platform_device_id}
        return sub

    # Capture is a dev hook, gated behind expose_dev_sensors.
    sub = config.get(CONF_CAPTURE_SESSIONS)
    if sub is not None and expose_dev:
        btn = await button.new_button(_with_dev(sub))
        await cg.register_parented(btn, hub)
        cg.add(hub.set_capture_button(btn))

    # Reset-head is a real config action, always created.
    sub = config[CONF_RESET_HEAD]
    btn = await button.new_button(_with_dev(sub))
    await cg.register_parented(btn, hub)

    # Sync-clock writes the brush clock on press only; skip it when the hub has
    # no time source, since it could only warn at runtime.
    sub = config.get(CONF_SYNC_TIME)
    if sub is not None and _hub_has_time(config[CONF_OCLEAN_ID]):
        btn = await button.new_button(_with_dev(sub))
        await cg.register_parented(btn, hub)

    # Poll-now is read-only on the brush, so it is always created; it is
    # hidden by default in Home Assistant instead of dev-gated.
    sub = config.get(CONF_POLL_NOW)
    if sub is not None:
        btn = await button.new_button(_with_dev(sub))
        await cg.register_parented(btn, hub)
