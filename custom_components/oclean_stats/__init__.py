"""Oclean session long-term statistics bridge.

Listens for the Home Assistant event that the ESPHome Oclean component fires
once per brushing session (event type "esphome.oclean_session") and writes the
session metrics into Home Assistant long-term statistics, keyed by the real
session timestamp carried in the event. Without this, Home Assistant stamps any
incoming entity state with the wall-clock "now", so a backfilled ring of past
sessions would all pile onto the import moment instead of graphing at their real
times. External statistics let us inject rows at an arbitrary past hour.

Read-only with respect to the brush: this never sends BLE, never writes the
device, and adds no entities. It only writes recorder statistics rows.

Statistic ids use a colon (external-statistics rule), one per (brush, metric),
where the slug comes from the configured brush map:
  oclean:<slug>_score, oclean:<slug>_duration, oclean:<slug>_coverage

Configuration (configuration.yaml) maps each brush MAC to a slug:
  oclean_stats:
    brushes:
      "AA:BB:CC:DD:EE:FF": alice
      "AA:BB:CC:DD:EE:00": bob

Idempotency: each session is bucketed to the top of its hour. Re-importing the
same hour overwrites that bucket, so the first-boot whole-ring backfill (which
re-sends sessions) is safe to receive repeatedly. Caveat: two sessions inside
the same clock hour collapse into one hourly bucket (see README).
"""

from __future__ import annotations

import logging
from datetime import UTC, datetime

import voluptuous as vol

# The recorder statistics API moved/renamed pieces over time. Import defensively
# so this one file works across the supported Home Assistant range without edits.
from homeassistant.components.recorder.models import (
    StatisticData,
    StatisticMetaData,
)
from homeassistant.components.recorder.statistics import (
    async_add_external_statistics,
)
from homeassistant.core import Event, HomeAssistant
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.typing import ConfigType

try:
    # StatisticMeanType landed in 2025.x and is the non-deprecated way to flag a
    # mean statistic. has_mean (bool) is deprecated and slated for removal.
    from homeassistant.components.recorder.models import StatisticMeanType

    _HAS_MEAN_TYPE = True
except ImportError:  # older cores
    StatisticMeanType = None  # type: ignore[assignment]
    _HAS_MEAN_TYPE = False

_LOGGER = logging.getLogger(__name__)

DOMAIN = "oclean_stats"

# External statistics namespace. Must be a colon-form prefix, not a real entity
# domain, and must not collide with an existing recorder entity statistic_id.
STAT_SOURCE = "oclean"

# Event fired by the ESPHome Oclean component, one per session.
EVENT_TYPE = "esphome.oclean_session"

# Reject a timestamp implausibly far ahead of now (a bad brush clock or a spoofed
# bus event): it would otherwise plant a permanent junk bucket in the statistics.
FUTURE_MARGIN_S = 86400

# configuration.yaml { MAC: slug }; MAC as the component reports it (upper,
# colons), slug in [a-z0-9_].
CONF_BRUSHES = "brushes"

CONFIG_SCHEMA = vol.Schema(
    {
        DOMAIN: vol.Schema(
            {
                vol.Optional(CONF_BRUSHES, default=dict): {cv.string: cv.string},
            }
        )
    },
    extra=vol.ALLOW_EXTRA,
)

# Metrics pulled from the event data map. Each becomes its own statistic id.
# (event_key, id_suffix, friendly_metric, unit_of_measurement, min, max)
# The event bus is spoofable (any ESPHome device or authenticated API client can
# fire esphome.* events with an arbitrary "device" field), so each value is
# clamped to its plausible range before it reaches recorder statistics. The
# firmware clamps too; this keeps a forged event from bypassing that. Duration
# bound mirrors the firmware SESSION_MAX_DURATION_S (two hours).
METRICS: tuple[tuple[str, str, str, str | None, int, int], ...] = (
    ("score", "score", "Score", None, 0, 100),
    ("duration", "duration", "Duration", "s", 0, 7200),
    ("coverage", "coverage", "Coverage", "%", 0, 100),
)


def _hour_bucket(epoch_s: int) -> datetime:
    """Truncate a unix-epoch second to the top of its UTC hour.

    The recorder requires the statistic start to sit on a period boundary
    (minute/second/microsecond zero). It is tz-aware UTC; Home Assistant
    converts to local time for display.
    """
    dt = datetime.fromtimestamp(epoch_s, tz=UTC)
    return dt.replace(minute=0, second=0, microsecond=0)


def _parse_int(raw: str | None) -> int | None:
    """Parse an int that the component may send as '-' for absent score."""
    if raw is None:
        return None
    raw = raw.strip()
    if raw in ("", "-"):
        return None
    try:
        return int(raw)
    except ValueError:
        try:
            # tolerate a stray float string
            return round(float(raw))
        except ValueError:
            return None


def _metadata(stat_id: str, name: str, unit: str | None) -> StatisticMetaData:
    """Build metadata for a gauge-like (mean) external statistic.

    A brushing score/coverage/duration is an instantaneous gauge, not an
    accumulating counter, so it is a mean statistic: has_sum False, mean flagged.
    unit_class is None because none of these map to a Home Assistant unit
    converter (score and coverage are dimensionless-ish; duration in seconds has
    no cross-unit conversion need here). Passing unit_class explicitly silences
    the 2025.11 deprecation.
    """
    meta: StatisticMetaData = {
        "source": STAT_SOURCE,
        "statistic_id": stat_id,
        "name": name,
        "unit_of_measurement": unit,
        "has_sum": False,
        "unit_class": None,
    }
    if _HAS_MEAN_TYPE:
        meta["mean_type"] = StatisticMeanType.ARITHMETIC
    else:
        # Deprecated path for cores predating StatisticMeanType.
        meta["has_mean"] = True
    return meta


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    """Register the event listener. No config entry, no platforms, no entities."""
    conf = config.get(DOMAIN) or {}
    mac_to_slug = {
        mac.strip().upper(): slug
        for mac, slug in (conf.get(CONF_BRUSHES) or {}).items()
    }

    async def _handle_session(event: Event) -> None:
        # A malformed or hostile bus event must never take the listener down, so
        # any failure past here is logged and swallowed.
        try:
            data = event.data or {}
            mac = str(data.get("device") or "")
            slug = mac_to_slug.get(mac.strip().upper()) if mac else None
            if slug is None:
                _LOGGER.warning(
                    "oclean_stats: session event from unmapped brush MAC %r; add "
                    "it to the brushes map in configuration.yaml to record stats",
                    mac,
                )
                return

            ts_raw = data.get("ts")
            epoch = _parse_int(ts_raw)
            now_ts = datetime.now(UTC).timestamp()
            if epoch is None or epoch <= 0 or epoch > now_ts + FUTURE_MARGIN_S:
                _LOGGER.debug("oclean_stats: skipping event with bad ts %r", ts_raw)
                return

            start = _hour_bucket(epoch)

            for event_key, suffix, metric_name, unit, vmin, vmax in METRICS:
                value = _parse_int(data.get(event_key))
                if value is None:
                    continue
                value = max(vmin, min(vmax, value))

                stat_id = f"{STAT_SOURCE}:{slug}_{suffix}"
                fvalue = float(value)
                meta = _metadata(stat_id, f"Oclean {slug} {metric_name}", unit)

                # One hourly bucket. min/max/mean are equal for a single reading;
                # if two sessions share an hour, the later import overwrites this
                # bucket (it does not merge), so only the last session in that hour
                # survives. state mirrors the value for convenience.
                stat_row: StatisticData = {
                    "start": start,
                    "mean": fvalue,
                    "min": fvalue,
                    "max": fvalue,
                    "state": fvalue,
                }

                # async_add_external_statistics is the public, supported entry
                # point for non-entity sources. It validates the colon statistic_id
                # and enqueues the DB write on the recorder thread. Importing the
                # same (statistic_id, start) overwrites that row -> idempotent.
                async_add_external_statistics(hass, meta, [stat_row])

            _LOGGER.debug(
                "oclean_stats: recorded session for %s at %s (epoch %s)",
                slug,
                start.isoformat(),
                epoch,
            )
        except Exception:
            _LOGGER.exception("oclean_stats: failed to handle session event")

    hass.bus.async_listen(EVENT_TYPE, _handle_session)
    _LOGGER.info(
        "oclean_stats: listening for %s, mapping %d brush MAC(s)",
        EVENT_TYPE,
        len(mac_to_slug),
    )
    return True
