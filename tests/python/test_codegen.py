"""Codegen helpers from components/oclean/__init__.py, run on the host.

Needs an interpreter with esphome importable:

    python -m unittest discover -s tests/python
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "components"))

import esphome.config_validation as cv
import esphome.final_validate as fv
import oclean as oc
import oclean.number as ocnum
import oclean.select as ocsel
from esphome.core import CORE

ROWS = [("battery", "Battery"), ("score", "Score")]


class RawConfigCase(unittest.TestCase):
    """The prefix is resolved during validation, off CORE.raw_config."""

    def setUp(self):
        self.addCleanup(setattr, CORE, "raw_config", CORE.raw_config)

    def set_hubs(self, *hubs):
        CORE.raw_config = {"oclean": list(hubs)}


class EntityNamePrefix(RawConfigCase):
    def test_unset_is_empty(self):
        self.set_hubs({"id": "hub_a"}, {"id": "hub_b"})
        self.assertEqual(oc.entity_name_prefix("hub_a"), "")

    def test_reads_the_explicit_value(self):
        self.set_hubs({"id": "hub_a", "name_prefix": "Brush A"})
        self.assertEqual(oc.entity_name_prefix("hub_a"), "Brush A")

    def test_is_stripped(self):
        self.set_hubs({"id": "hub_a", "name_prefix": "  Brush A  "})
        self.assertEqual(oc.entity_name_prefix("hub_a"), "Brush A")

    def test_picks_the_matching_hub_out_of_several(self):
        self.set_hubs(
            {"id": "hub_a", "name_prefix": "A"}, {"id": "hub_b", "name_prefix": "B"}
        )
        self.assertEqual(oc.entity_name_prefix("hub_b"), "B")

    def test_a_lone_hub_resolves_without_an_id_reference(self):
        self.set_hubs({"id": "hub_a", "name_prefix": "Solo"})
        self.assertEqual(oc.entity_name_prefix(None), "Solo")

    def test_two_hubs_do_not_resolve_without_one(self):
        self.set_hubs({"id": "hub_a", "name_prefix": "A"}, {"id": "hub_b"})
        self.assertEqual(oc.entity_name_prefix(None), "")

    def test_unknown_hub_yields_no_prefix(self):
        self.set_hubs({"id": "hub_a", "name_prefix": "A"})
        self.assertEqual(oc.entity_name_prefix("nope"), "")


class InjectEntityDefaults(RawConfigCase):
    def test_prefixes_the_injected_default(self):
        self.set_hubs({"id": "hub_a", "name_prefix": "Brush A"})
        out = oc.inject_entity_defaults({"oclean_id": "hub_a"}, ROWS)
        self.assertEqual(out["battery"]["name"], "Brush A Battery")

    def test_leaves_a_name_from_yaml_alone(self):
        self.set_hubs({"id": "hub_a", "name_prefix": "Brush A"})
        config = {"oclean_id": "hub_a", "battery": {"name": "Toothbrush charge"}}
        out = oc.inject_entity_defaults(config, ROWS)
        self.assertEqual(out["battery"]["name"], "Toothbrush charge")

    def test_opt_out_keeps_the_bare_names(self):
        self.set_hubs({"id": "hub_a", "name_prefix": ""}, {"id": "hub_b"})
        out = oc.inject_entity_defaults({"oclean_id": "hub_a"}, ROWS)
        self.assertEqual(out["battery"]["name"], "Battery")

    def test_no_option_keeps_the_bare_names(self):
        self.set_hubs({"id": "hub_a"}, {"id": "hub_b"})
        out = oc.inject_entity_defaults({"oclean_id": "hub_a"}, ROWS)
        self.assertEqual(out["battery"]["name"], "Battery")

    def test_does_not_mutate_caller_config(self):
        self.set_hubs({"id": "hub_a", "name_prefix": "Brush A"})
        config = {"oclean_id": "hub_a"}
        oc.inject_entity_defaults(config, ROWS)
        self.assertNotIn("battery", config)


class NamePrefixIsReachable(RawConfigCase):
    def test_one_hub_needs_no_id(self):
        self.set_hubs({"ble_client_id": "ble_a", "name_prefix": "Solo"})
        oc._validate_name_prefix_is_reachable({})

    def test_prefix_without_an_id_is_rejected(self):
        self.set_hubs({"name_prefix": "A"}, {"id": "hub_b"})
        with self.assertRaises(cv.Invalid):
            oc._validate_name_prefix_is_reachable({})

    def test_no_prefix_without_an_id_passes(self):
        self.set_hubs({"ble_client_id": "ble_a"}, {"id": "hub_b"})
        oc._validate_name_prefix_is_reachable({})


class WarnOnSharedDefaultNames(unittest.TestCase):
    def _run(self, hubs, hub):
        token = fv.full_config.set({"oclean": hubs})
        try:
            with self.assertLogs(oc._LOGGER, level="WARNING") as caught:
                oc._warn_on_shared_default_names(hub)
                # assertLogs fails an empty block, so mark the silent case.
                oc._LOGGER.warning("sentinel")
            return [msg for msg in caught.output if "sentinel" not in msg]
        finally:
            fv.full_config.reset(token)

    def test_warns_when_a_second_hub_has_no_prefix(self):
        hubs = [{"id": "hub_a"}, {"id": "hub_b", "name_prefix": "B"}]
        self.assertEqual(len(self._run(hubs, hubs[0])), 1)

    def test_a_prefixed_hub_is_silent(self):
        hubs = [{"id": "hub_a"}, {"id": "hub_b", "name_prefix": "B"}]
        self.assertEqual(self._run(hubs, hubs[1]), [])

    def test_an_explicit_opt_out_is_silent(self):
        hubs = [{"id": "hub_a", "name_prefix": ""}, {"id": "hub_b"}]
        self.assertEqual(self._run(hubs, hubs[0]), [])

    def test_a_single_hub_is_silent(self):
        hubs = [{"id": "hub_a"}]
        self.assertEqual(self._run(hubs, hubs[0]), [])


# a row on every model, one only on the X Ultra 20, one only on the X Pro Elite
MODEL_ROWS = [
    ("battery", "Battery"),
    ("device_mode", "Device mode"),
    ("quadrant_upper_left", "Quadrant upper left"),
]


class RawHubModel(RawConfigCase):
    def test_unset_is_the_x_pro_elite(self):
        self.set_hubs({"id": "hub_a"})
        self.assertEqual(oc.raw_hub_model("hub_a"), "x_pro_elite")

    def test_reads_the_explicit_value_in_any_case(self):
        self.set_hubs({"id": "hub_a", "model": "X_Ultra_20"})
        self.assertEqual(oc.raw_hub_model("hub_a"), "x_ultra_20")

    def test_an_invalid_value_falls_back_to_the_default(self):
        # The hub schema reports it; the platform blocks must not pile on.
        self.set_hubs({"id": "hub_a", "model": "x_pro"})
        self.assertEqual(oc.raw_hub_model("hub_a"), "x_pro_elite")


class InjectPerModel(RawConfigCase):
    def _inject(self, config):
        return oc.inject_entity_defaults(config, MODEL_ROWS, platform="sensor")

    def test_x_ultra_20_builds_its_rows_and_not_the_elite_ones(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        out = self._inject({"oclean_id": "hub_a"})
        self.assertIn("battery", out)
        self.assertIn("device_mode", out)
        self.assertNotIn("quadrant_upper_left", out)

    def test_the_default_builds_the_elite_rows_and_not_the_ultra_ones(self):
        self.set_hubs({"id": "hub_a"})
        out = self._inject({"oclean_id": "hub_a"})
        self.assertIn("battery", out)
        self.assertIn("quadrant_upper_left", out)
        self.assertNotIn("device_mode", out)

    def test_an_explicit_row_the_model_lacks_is_rejected(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        for want in ({"name": "Zone"}, True, None):
            with self.assertRaises(cv.Invalid):
                self._inject({"oclean_id": "hub_a", "quadrant_upper_left": want})

    def test_false_on_a_row_the_model_lacks_is_accepted(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        out = self._inject({"oclean_id": "hub_a", "quadrant_upper_left": False})
        self.assertNotIn("quadrant_upper_left", out)

    def test_without_a_platform_nothing_is_filtered(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        out = oc.inject_entity_defaults({"oclean_id": "hub_a"}, MODEL_ROWS)
        self.assertIn("quadrant_upper_left", out)

    def test_flags_without_a_setting_are_built_on_no_model(self):
        rows = [
            ("fill_brush", "Fill brush"),
            ("auto_update", "Auto update"),
            ("charging", "Charging"),
        ]
        for model in oc.MODELS:
            self.set_hubs({"id": "hub_a", "model": model})
            out = oc.inject_entity_defaults(
                {"oclean_id": "hub_a"}, rows, platform="binary_sensor"
            )
            self.assertNotIn("fill_brush", out, model)
            self.assertNotIn("auto_update", out, model)
            self.assertIn("charging", out, model)
            # no model to switch to, so the error does not suggest one
            with self.assertRaises(cv.Invalid) as caught:
                oc.inject_entity_defaults(
                    {"oclean_id": "hub_a", "fill_brush": True},
                    rows,
                    platform="binary_sensor",
                )
            self.assertIn("No supported model", str(caught.exception))

    def test_a_row_another_model_has_suggests_the_model_option(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        with self.assertRaises(cv.Invalid) as caught:
            self._inject({"oclean_id": "hub_a", "quadrant_upper_left": True})
        self.assertIn("set model:", str(caught.exception))

    def test_buttons_are_x_pro_elite_only(self):
        rows = [
            ("capture_sessions", "Capture sessions"),
            ("reset_head", "Reset brush head"),
            ("sync_time", "Sync clock"),
            ("poll_now", "Poll now"),
        ]
        for model, built in (("x_pro_elite", True), ("x_ultra_20", False)):
            self.set_hubs({"id": "hub_a", "model": model})
            out = oc.inject_entity_defaults(
                {"oclean_id": "hub_a"}, rows, platform="button"
            )
            for key, _name in rows:
                self.assertEqual(key in out, built, f"{model} {key}")

    def test_over_pressure_is_an_x_pro_elite_switch_only(self):
        rows = [
            ("over_pressure", "Over-pressure alert"),
            ("raise_wake", "Raise to wake"),
        ]
        for model, built in (("x_pro_elite", True), ("x_ultra_20", False)):
            self.set_hubs({"id": "hub_a", "model": model})
            out = oc.inject_entity_defaults(
                {"oclean_id": "hub_a"}, rows, platform="switch"
            )
            self.assertEqual("over_pressure" in out, built, model)
            self.assertIn("raise_wake", out, model)


class HeadMaxKey(RawConfigCase):
    def test_the_old_days_key_points_at_the_minutes_one(self):
        self.set_hubs({"id": "hub_a"})
        for value in (90, False, {"name": "Head days"}):
            with self.assertRaises(cv.Invalid) as caught:
                ocnum.CONFIG_SCHEMA({"oclean_id": "hub_a", "head_max_days": value})
            self.assertIn("head_max_minutes", str(caught.exception))

    def test_the_limit_takes_any_u16_minute_count(self):
        keys = {key for key, _n in ocnum._DEFAULT_NAMES}
        self.assertIn("head_max_minutes", keys)
        self.assertNotIn("head_max_days", keys)
        self.assertEqual((ocnum.HEAD_MAX_MIN, ocnum.HEAD_MAX_MAX), (1, 0xFFFF))


def _scheme_config(gear, name="Hard"):
    return {
        "oclean_id": "hub_a",
        "brush_scheme": {
            "custom_modes": [
                {"name": name, "program": [{"gear": gear, "duration": 30}]},
            ]
        },
    }


class SchemeModesPerModel(RawConfigCase):
    def test_x_ultra_20_takes_gears_up_to_54(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        ocsel._validate_modes_for_model(_scheme_config(54))
        with self.assertRaises(cv.Invalid):
            ocsel._validate_modes_for_model(_scheme_config(55))

    def test_x_pro_elite_stops_at_41(self):
        self.set_hubs({"id": "hub_a"})
        ocsel._validate_modes_for_model(_scheme_config(41))
        with self.assertRaises(cv.Invalid) as caught:
            ocsel._validate_modes_for_model(_scheme_config(50))
        self.assertIn("41", str(caught.exception))

    def test_a_label_may_not_shadow_a_fixed_option_of_its_model(self):
        # "Travel" is an X Pro Elite preset of 2 min; on the X Ultra 20 it is free
        travel = {
            "oclean_id": "hub_a",
            "brush_scheme": {
                "custom_modes": [
                    {
                        "name": "Travel",
                        "program": [
                            {"gear": 17, "duration": 30},
                            {"gear": 17, "duration": 30},
                            {"gear": 35, "duration": 30},
                            {"gear": 35, "duration": 30},
                        ],
                    }
                ]
            },
        }
        self.set_hubs({"id": "hub_a"})
        with self.assertRaises(cv.Invalid):
            ocsel._validate_modes_for_model(travel)
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        ocsel._validate_modes_for_model(travel)


SWITCH_ROWS = [
    ("area_reminder", "Area reminder"),
    ("raise_wake", "Raise to wake"),
]


class InjectModelDefaults(RawConfigCase):
    def _inject(self, config):
        return oc.inject_entity_defaults(config, SWITCH_ROWS, platform="switch")

    def test_x_ultra_20_names_the_zone_cue_after_what_it_does(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20", "name_prefix": "X20"})
        out = self._inject({"oclean_id": "hub_a"})
        self.assertEqual(out["area_reminder"]["name"], "X20 Voice on zone change")
        self.assertEqual(out["area_reminder"]["icon"], "mdi:swap-horizontal")
        self.assertEqual(out["raise_wake"]["name"], "X20 Raise to wake")
        self.assertNotIn("icon", out["raise_wake"])

    def test_x_pro_elite_keeps_the_shared_default(self):
        self.set_hubs({"id": "hub_a"})
        out = self._inject({"oclean_id": "hub_a"})
        self.assertEqual(out["area_reminder"]["name"], "Area reminder")
        self.assertNotIn("icon", out["area_reminder"])

    def test_name_and_icon_from_yaml_win(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        mine = {"name": "Zone voice", "icon": "mdi:bell"}
        out = self._inject({"oclean_id": "hub_a", "area_reminder": mine})
        self.assertEqual(out["area_reminder"], mine)


class HubBuilds(unittest.TestCase):
    """The dev gate, resolved off the validated config in to_code."""

    def setUp(self):
        self.addCleanup(setattr, CORE, "config", CORE.config)

    def set_hub(self, **conf):
        CORE.config = {"oclean": [{"id": "hub_a", **conf}]}

    def test_area_reminder_is_dev_on_the_x_pro_elite(self):
        self.set_hub(model="x_pro_elite", expose_dev_sensors=False)
        self.assertFalse(oc.hub_builds("hub_a", "switch", "area_reminder"))

    def test_area_reminder_is_a_plain_control_on_the_x_ultra_20(self):
        self.set_hub(model="x_ultra_20", expose_dev_sensors=False)
        self.assertTrue(oc.hub_builds("hub_a", "switch", "area_reminder"))

    def test_expose_dev_builds_the_dev_rows(self):
        self.set_hub(model="x_pro_elite", expose_dev_sensors=True)
        self.assertTrue(oc.hub_builds("hub_a", "switch", "area_reminder"))

    def test_capture_is_dev_on_the_x_pro_elite(self):
        self.set_hub(model="x_pro_elite", expose_dev_sensors=False)
        self.assertFalse(oc.hub_builds("hub_a", "button", "capture_sessions"))


NEEDS_ROWS = [
    ("last_session_score", "Score"),
    ("gesture_zone_1", "Zone 1"),
    ("battery", "Battery"),
]


class InjectNeeds(RawConfigCase):
    """Rows of a model fed only through a hub option."""

    def _inject(self, config, rows=None, platform="sensor"):
        rows = NEEDS_ROWS if rows is None else rows
        return oc.inject_entity_defaults(config, rows, platform=platform)

    def test_x_ultra_20_without_the_receiver_skips_score_and_zones(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        out = self._inject({"oclean_id": "hub_a"})
        self.assertNotIn("last_session_score", out)
        self.assertNotIn("gesture_zone_1", out)
        self.assertIn("battery", out)

    def test_x_ultra_20_with_the_receiver_builds_them(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20", "cloud_receiver": True})
        out = self._inject({"oclean_id": "hub_a"})
        self.assertIn("last_session_score", out)
        self.assertIn("gesture_zone_1", out)

    def test_a_receiver_switched_off_counts_as_unset(self):
        for off in (False, "false", "off"):
            self.set_hubs({"id": "hub_a", "model": "x_ultra_20", "cloud_receiver": off})
            self.assertNotIn("last_session_score", self._inject({"oclean_id": "hub_a"}))

    def test_an_explicit_row_without_its_option_names_the_option(self):
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20"})
        with self.assertRaises(cv.Invalid) as caught:
            self._inject({"oclean_id": "hub_a", "last_session_score": True})
        self.assertIn("cloud_receiver", str(caught.exception))

    def test_the_x_pro_elite_reads_score_and_zones_over_ble(self):
        self.set_hubs({"id": "hub_a"})
        out = self._inject({"oclean_id": "hub_a"})
        self.assertIn("last_session_score", out)
        self.assertIn("gesture_zone_1", out)

    def test_birthday_written_needs_a_birthday_on_the_hub(self):
        rows = [("birthday_written", "Birthday written")]
        self.set_hubs(
            {"id": "hub_a", "model": "x_ultra_20", "gender": "unknown", "age": 18}
        )
        out = self._inject({"oclean_id": "hub_a"}, rows, "binary_sensor")
        self.assertNotIn("birthday_written", out)
        self.set_hubs({"id": "hub_a", "model": "x_ultra_20", "birthday": "03-07"})
        out = self._inject({"oclean_id": "hub_a"}, rows, "binary_sensor")
        self.assertIn("birthday_written", out)


if __name__ == "__main__":
    unittest.main()
