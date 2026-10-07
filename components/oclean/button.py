import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_ID,
    CONF_TIME_ID,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import CORE

from . import (
    CONF_OCLEAN_ID,
    CONF_WIFI_PROVISIONING,
    DOMAIN,
    OCLEAN_COMPONENT_SCHEMA,
    OcleanHub,
    final_hub_conf,
    hub_builds,
    inject_entity_defaults,
    oclean_ns,
)


def _hub_has_time(hub_id):
    # The sync-clock button writes the brush clock from the hub's time source.
    # Without a time: source it can only warn at runtime, so skip creating it.
    target = str(hub_id)
    for hub_conf in CORE.config.get(DOMAIN, []):
        if str(hub_conf.get(CONF_ID)) == target:
            return CONF_TIME_ID in hub_conf
    return False


DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

OcleanCaptureButton = oclean_ns.class_(
    "OcleanCaptureButton", button.Button, cg.Parented.template(OcleanHub)
)
OcleanResetHeadButton = oclean_ns.class_(
    "OcleanResetHeadButton", button.Button, cg.Parented.template(OcleanHub)
)
OcleanSyncTimeButton = oclean_ns.class_(
    "OcleanSyncTimeButton", button.Button, cg.Parented.template(OcleanHub)
)
OcleanPollNowButton = oclean_ns.class_(
    "OcleanPollNowButton", button.Button, cg.Parented.template(OcleanHub)
)
OcleanApplyCloudHostButton = oclean_ns.class_(
    "OcleanApplyCloudHostButton", button.Button, cg.Parented.template(OcleanHub)
)
OcleanClearCloudHostButton = oclean_ns.class_(
    "OcleanClearCloudHostButton", button.Button, cg.Parented.template(OcleanHub)
)
OcleanPointCloudHereButton = oclean_ns.class_(
    "OcleanPointCloudHereButton", button.Button, cg.Parented.template(OcleanHub)
)
OcleanProvisionWifiButton = oclean_ns.class_(
    "OcleanProvisionWifiButton", button.Button, cg.Parented.template(OcleanHub)
)

# Dev-gated. Requests a buffered-session download and holds the link open so
# the record stream can be captured into the log.
CONF_CAPTURE_SESSIONS = "capture_sessions"
DEFAULT_CAPTURE_NAME = "Capture sessions"

# X Pro Elite only. Resets the brush-head usage counter (irreversible).
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

# X Ultra 20 only, opt-in: write the staged cloud-host text, or clear it back to
# the firmware fallback. Both write to the brush.
CONF_APPLY_CLOUD_HOST = "apply_cloud_host"
DEFAULT_APPLY_CLOUD_HOST_NAME = "Apply cloud host"
CONF_CLEAR_CLOUD_HOST = "clear_cloud_host"
DEFAULT_CLEAR_CLOUD_HOST_NAME = "Clear cloud host"
CONF_POINT_CLOUD_AT_NODE = "point_cloud_at_node"
DEFAULT_POINT_CLOUD_AT_NODE_NAME = "Point cloud at this node"
CONF_PROVISION_WIFI = "provision_wifi"
DEFAULT_PROVISION_WIFI_NAME = "Provision Wi-Fi"


_DEFAULT_NAMES = [
    (CONF_CAPTURE_SESSIONS, DEFAULT_CAPTURE_NAME),
    (CONF_RESET_HEAD, DEFAULT_RESET_HEAD_NAME),
    (CONF_SYNC_TIME, DEFAULT_SYNC_TIME_NAME),
    (CONF_POLL_NOW, DEFAULT_POLL_NOW_NAME),
    (CONF_APPLY_CLOUD_HOST, DEFAULT_APPLY_CLOUD_HOST_NAME),
    (CONF_CLEAR_CLOUD_HOST, DEFAULT_CLEAR_CLOUD_HOST_NAME),
    (CONF_POINT_CLOUD_AT_NODE, DEFAULT_POINT_CLOUD_AT_NODE_NAME),
    (CONF_PROVISION_WIFI, DEFAULT_PROVISION_WIFI_NAME),
]

_OPT_IN = frozenset(
    {
        CONF_APPLY_CLOUD_HOST,
        CONF_CLEAR_CLOUD_HOST,
        CONF_POINT_CLOUD_AT_NODE,
        CONF_PROVISION_WIFI,
    }
)


def _inject_defaults(config):
    return inject_entity_defaults(
        config,
        _DEFAULT_NAMES,
        hidden=frozenset({CONF_POLL_NOW}),
        opt_in=_OPT_IN,
        platform="button",
    )


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
                icon="mdi:refresh",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_APPLY_CLOUD_HOST): button.button_schema(
                OcleanApplyCloudHostButton,
                icon="mdi:cloud-upload-outline",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_CLEAR_CLOUD_HOST): button.button_schema(
                OcleanClearCloudHostButton,
                icon="mdi:cloud-off-outline",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_POINT_CLOUD_AT_NODE): button.button_schema(
                OcleanPointCloudHereButton,
                icon="mdi:cloud-sync-outline",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_PROVISION_WIFI): button.button_schema(
                OcleanProvisionWifiButton,
                icon="mdi:wifi-plus",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
        }
    ),
)


def _provision_needs_wifi_provisioning(config):
    # The provisioning code, button class included, is compiled only with
    # wifi_provisioning: true on the hub.
    if CONF_PROVISION_WIFI not in config:
        return config
    if not final_hub_conf(config[CONF_OCLEAN_ID]).get(CONF_WIFI_PROVISIONING):
        raise cv.Invalid(
            f"'{CONF_PROVISION_WIFI}' needs '{CONF_WIFI_PROVISIONING}: true' on "
            f"hub '{config[CONF_OCLEAN_ID]}'",
            path=[CONF_PROVISION_WIFI],
        )
    return config


FINAL_VALIDATE_SCHEMA = _provision_needs_wifi_provisioning


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])

    sub = config.get(CONF_CAPTURE_SESSIONS)
    if sub is not None and hub_builds(
        config[CONF_OCLEAN_ID], "button", CONF_CAPTURE_SESSIONS
    ):
        btn = await button.new_button(sub)
        await cg.register_parented(btn, hub)
        cg.add(hub.set_capture_button(btn))

    sub = config.get(CONF_RESET_HEAD)
    if sub is not None:
        btn = await button.new_button(sub)
        await cg.register_parented(btn, hub)

    # Sync-clock writes the brush clock on press only; skip it when the hub has
    # no time source, since it could only warn at runtime.
    sub = config.get(CONF_SYNC_TIME)
    if sub is not None and _hub_has_time(config[CONF_OCLEAN_ID]):
        btn = await button.new_button(sub)
        await cg.register_parented(btn, hub)

    sub = config.get(CONF_POLL_NOW)
    if sub is not None:
        btn = await button.new_button(sub)
        await cg.register_parented(btn, hub)

    for key in (CONF_APPLY_CLOUD_HOST, CONF_CLEAR_CLOUD_HOST, CONF_POINT_CLOUD_AT_NODE):
        sub = config.get(key)
        if sub is not None:
            btn = await button.new_button(sub)
            await cg.register_parented(btn, hub)

    sub = config.get(CONF_PROVISION_WIFI)
    if sub is not None:
        btn = await button.new_button(sub)
        await cg.register_parented(btn, hub)
