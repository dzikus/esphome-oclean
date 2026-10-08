"""Cross-table invariants of the entity tables in components/oclean.

These fail silently: a hidden key matching no row leaves the entity visible, a
switch missing from HUB_SETTERS is a KeyError in codegen rather than in
validation, and a duplicate scheme label makes the second one unreachable.

    python -m unittest discover -s tests/python
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "components"))

import esphome.config_validation as cv
import oclean as oc
import oclean.binary_sensor as ocbs
import oclean.button as ocbtn
import oclean.number as ocnum
import oclean.select as ocsel
import oclean.sensor as ocsens
import oclean.switch as ocsw
import oclean.text as octext
import oclean.text_sensor as octs

PLATFORMS = {
    "sensor": ocsens,
    "binary_sensor": ocbs,
    "text_sensor": octs,
    "switch": ocsw,
    "select": ocsel,
    "number": ocnum,
    "button": ocbtn,
    "text": octext,
}


class DefaultNames(unittest.TestCase):
    def test_no_duplicate_key_within_a_platform(self):
        for name, mod in PLATFORMS.items():
            keys = [key for key, _n in mod._DEFAULT_NAMES]
            self.assertEqual(len(keys), len(set(keys)), name)

    def test_no_duplicate_default_name_within_a_platform(self):
        # Duplicates inside one platform collide even on a single-hub node.
        for name, mod in PLATFORMS.items():
            names = [n for _k, n in mod._DEFAULT_NAMES]
            self.assertEqual(len(names), len(set(names)), name)

    def test_every_default_name_is_non_empty(self):
        for name, mod in PLATFORMS.items():
            for key, default in mod._DEFAULT_NAMES:
                self.assertTrue(default and default.strip(), f"{name}.{key}")


class KeySets(unittest.TestCase):
    def _keys(self, mod):
        return {key for key, _n in mod._DEFAULT_NAMES}

    def test_hidden_sensor_keys_exist(self):
        self.assertLessEqual(oc.HIDDEN_SENSOR_KEYS, self._keys(ocsens))

    def test_hidden_binary_sensor_keys_exist(self):
        self.assertLessEqual(oc.HIDDEN_BINARY_SENSOR_KEYS, self._keys(ocbs))

    def test_hidden_text_sensor_keys_exist(self):
        self.assertLessEqual(octs.HIDDEN_TEXT_SENSOR_KEYS, self._keys(octs))


class ModelEntitySets(unittest.TestCase):
    # A key matching no row is a model restriction that silently does nothing.

    def test_every_schema_model_has_an_entity_set(self):
        self.assertEqual(set(oc.MODELS), set(oc.MODEL_ENTITY_SETS))
        self.assertIn(oc.DEFAULT_MODEL, oc.MODELS)

    def test_listed_keys_exist_on_their_platform(self):
        for model, sets in oc.MODEL_ENTITY_SETS.items():
            for kind in ("unavailable", "dev"):
                for platform, keys in sets[kind].items():
                    self.assertIn(platform, PLATFORMS, f"{model}.{kind}")
                    rows = {key for key, _n in PLATFORMS[platform]._DEFAULT_NAMES}
                    self.assertLessEqual(keys, rows, f"{model}.{kind}.{platform}")

    def test_a_row_is_not_both_dev_and_unavailable(self):
        for model, sets in oc.MODEL_ENTITY_SETS.items():
            for platform, keys in sets["dev"].items():
                missing = sets["unavailable"].get(platform, frozenset())
                self.assertFalse(keys & missing, f"{model}.{platform}")

    def test_each_model_has_rows_the_other_lacks(self):
        # Guards the point of the option: drop either set and both models build
        # the same entities again.
        elite = oc.MODEL_ENTITY_SETS[oc.MODEL_X_PRO_ELITE]["unavailable"]
        ultra = oc.MODEL_ENTITY_SETS[oc.MODEL_X_ULTRA_20]["unavailable"]
        self.assertIn("voice_prompts", elite["switch"])
        self.assertIn("quadrant_upper_left", ultra["sensor"])

    def test_auto_mode_is_a_switch_on_the_x_ultra_20_only(self):
        elite = oc.MODEL_ENTITY_SETS[oc.MODEL_X_PRO_ELITE]["unavailable"]
        ultra = oc.MODEL_ENTITY_SETS[oc.MODEL_X_ULTRA_20]["unavailable"]
        self.assertIn("auto_mode", elite["switch"])
        self.assertNotIn("auto_mode", ultra.get("switch", frozenset()))
        self.assertIn("auto_mode", elite["binary_sensor"])
        self.assertIn("auto_mode", ultra["binary_sensor"])

    def test_elite_settings_bytes_without_a_setting_are_not_built(self):
        # constant zero, copies of bytes 0 and 1, a flag nothing writes, and a
        # pause flag the next session clears
        elite = oc.MODEL_ENTITY_SETS[oc.MODEL_X_PRO_ELITE]
        self.assertLessEqual(
            {
                "fill_brush",
                "auto_mode",
                "volume_enabled",
                "calendar_enabled",
                "splash_prevent",
            },
            elite["unavailable"]["binary_sensor"],
        )
        self.assertIn("volume_index", elite["unavailable"]["sensor"])
        self.assertNotIn("binary_sensor", elite["dev"])
        self.assertNotIn("sensor", elite["dev"])

    def test_x_ultra_20_leaves_out_what_its_firmware_never_fills(self):
        # 0.0.1.6 never writes settings byte 1 and never counts head use
        ultra = oc.MODEL_ENTITY_SETS[oc.MODEL_X_ULTRA_20]["unavailable"]
        self.assertLessEqual(
            {"head_used_time", "head_used_days", "head_used_times"}, ultra["sensor"]
        )
        self.assertIn("head_max_minutes", ultra["number"])
        self.assertIn("reset_head", ultra["button"])
        self.assertIn("network", ultra["binary_sensor"])

    def test_teaching_and_retail_mode_are_x_ultra_20_switches(self):
        elite = oc.MODEL_ENTITY_SETS[oc.MODEL_X_PRO_ELITE]["unavailable"]
        ultra = oc.MODEL_ENTITY_SETS[oc.MODEL_X_ULTRA_20]["unavailable"]
        for key in ("voice_teaching", "demo_mode"):
            self.assertIn(key, elite["switch"])
            self.assertNotIn(key, ultra["switch"])
            self.assertIn(key, elite["binary_sensor"])
            self.assertIn(key, ultra["binary_sensor"])
            self.assertIn(key, ocsw.HUB_SETTERS)

    def test_both_models_build_the_brushing_mode_select(self):
        for model, sets in oc.MODEL_ENTITY_SETS.items():
            missing = sets["unavailable"].get("select", frozenset())
            self.assertNotIn("brush_scheme", missing, model)
            self.assertFalse(
                {key for key, *_row in ocnum.CUSTOM_PARAMS}
                & sets["unavailable"].get("number", frozenset()),
                model,
            )

    def test_quadrants_are_hidden_and_elite_only(self):
        keys = {f"quadrant_{pos}" for pos in oc.QUADRANT_POSITIONS}
        self.assertEqual(len(keys), 4)
        self.assertLessEqual(keys, oc.HIDDEN_SENSOR_KEYS)
        ultra = oc.MODEL_ENTITY_SETS[oc.MODEL_X_ULTRA_20]["unavailable"]
        elite = oc.MODEL_ENTITY_SETS[oc.MODEL_X_PRO_ELITE]["unavailable"]
        self.assertLessEqual(keys, ultra["sensor"])
        self.assertFalse(keys & elite["sensor"])

    def test_gesture_zones_available_on_both_models(self):
        # the full cloud record carries gestureArray[0..7] on the X Ultra 20, so
        # the zone sensors are built there too (unlike the 4 quadrants)
        zones = {f"gesture_zone_{i + 1}" for i in range(8)}
        for model in (oc.MODEL_X_PRO_ELITE, oc.MODEL_X_ULTRA_20):
            unavail = oc.MODEL_ENTITY_SETS[model]["unavailable"].get(
                "sensor", frozenset()
            )
            self.assertFalse(zones & unavail, model)


class ModelEntityDefaults(unittest.TestCase):
    def test_overridden_rows_exist_and_are_built_on_that_model(self):
        for model, platforms in oc.MODEL_ENTITY_DEFAULTS.items():
            sets = oc.MODEL_ENTITY_SETS[model]
            for platform, rows in platforms.items():
                keys = {key for key, _n in PLATFORMS[platform]._DEFAULT_NAMES}
                missing = sets["unavailable"].get(platform, frozenset())
                for key in rows:
                    self.assertIn(key, keys, f"{model}.{platform}.{key}")
                    self.assertNotIn(key, missing, f"{model}.{platform}.{key}")

    def test_names_stay_unique_on_each_model(self):
        for model, sets in oc.MODEL_ENTITY_SETS.items():
            overrides = oc.MODEL_ENTITY_DEFAULTS.get(model, {})
            for platform, mod in PLATFORMS.items():
                missing = sets["unavailable"].get(platform, frozenset())
                rows = overrides.get(platform, {})
                names = [
                    rows.get(key, (name, None))[0]
                    for key, name in mod._DEFAULT_NAMES
                    if key not in missing
                ]
                self.assertEqual(len(names), len(set(names)), f"{model}.{platform}")


class VoiceSwitches(unittest.TestCase):
    def test_indexes_cover_the_three_flags_once(self):
        indexes = sorted(index for _key, index, *_row in ocsw.VOICE_SWITCHES)
        self.assertEqual(indexes, [0, 1, 2])


class SwitchSetters(unittest.TestCase):
    def test_every_command_switch_has_a_readback_setter(self):
        # switch.py indexes HUB_SETTERS directly, so a missing entry is a
        # KeyError during codegen, not a validation error.
        for key in ocsw.HUB_SETTERS:
            self.assertIn(key, {k for k, _n in ocsw._DEFAULT_NAMES})

    def test_setters_are_distinct(self):
        setters = list(ocsw.HUB_SETTERS.values())
        self.assertEqual(len(setters), len(set(setters)))


class SchemeTable(unittest.TestCase):
    def test_labels_are_unique(self):
        # The select matches an incoming option by label, so two schemes
        # sharing one would make the second unreachable.
        labels = [ocsel._scheme_label(pnum) for pnum in ocsel.SCHEMES]
        self.assertEqual(len(labels), len(set(labels)))

    def test_options_match_the_table(self):
        self.assertEqual(len(ocsel.SCHEME_OPTIONS), len(ocsel.SCHEMES))
        self.assertEqual(len(set(ocsel.SCHEME_OPTIONS)), len(ocsel.SCHEME_OPTIONS))

    def test_every_scheme_has_steps_in_range(self):
        for pnum, (label, steps) in ocsel.SCHEMES.items():
            self.assertTrue(steps, f"{pnum} {label}")
            for gear, duration in steps:
                self.assertGreaterEqual(gear, 1)
                self.assertGreater(duration, 0)

    def test_pnum_fits_the_wire_byte(self):
        for pnum in ocsel.SCHEMES:
            self.assertGreaterEqual(pnum, 0)
            self.assertLessEqual(pnum, 0xFF)

    def test_x_ultra_20_fixed_modes_stay_clear_of_the_custom_ids(self):
        # 1-5 screen modes, 6 voice teaching, as buffer 11 numbers them
        self.assertEqual(sorted(ocsel.X20_FIXED_MODES), [1, 2, 3, 4, 5, 6])
        self.assertLess(max(ocsel.X20_FIXED_MODES), ocsel.CUSTOM_PNUM)
        labels = ocsel.fixed_options(oc.MODEL_X_ULTRA_20)
        self.assertEqual(len(labels), len(set(labels)))
        self.assertNotIn(ocsel.CUSTOM_OPTION_LABEL, labels)
        self.assertEqual(
            ocsel.fixed_options(oc.MODEL_X_PRO_ELITE), ocsel.SCHEME_OPTIONS
        )

    def test_gear_cap_per_model(self):
        self.assertEqual(oc.GEAR_MAX, {"x_pro_elite": 41, "x_ultra_20": 54})
        self.assertEqual(set(oc.GEAR_MAX), set(oc.MODELS))
        for _pnum, (_label, steps) in ocsel.SCHEMES.items():
            for gear, _duration in steps:
                self.assertLessEqual(gear, oc.GEAR_MAX[oc.MODEL_X_PRO_ELITE])


class LanguageTable(unittest.TestCase):
    def test_ids_are_contiguous_from_one(self):
        self.assertEqual(
            sorted(ocsel.LANGUAGES), list(range(1, len(ocsel.LANGUAGES) + 1))
        )

    def test_names_are_unique(self):
        names = list(ocsel.LANGUAGES.values())
        self.assertEqual(len(names), len(set(names)))

    def test_options_match_the_table(self):
        self.assertEqual(len(ocsel.LANGUAGE_OPTIONS), len(ocsel.LANGUAGES))


class BlufiWifi(unittest.TestCase):
    def test_hub_override_wins(self):
        wifi = {"networks": [{"ssid": "other", "password": "x"}]}
        self.assertEqual(oc.resolve_blufi_wifi("net", "pw", wifi), ("net", "pw"))

    def test_override_ssid_without_password(self):
        self.assertEqual(oc.resolve_blufi_wifi("net", None, None), ("net", ""))

    def test_falls_back_to_node_wifi(self):
        wifi = {"networks": [{"ssid": "home", "password": "secret"}]}
        self.assertEqual(oc.resolve_blufi_wifi(None, None, wifi), ("home", "secret"))

    def test_nothing_configured(self):
        self.assertEqual(oc.resolve_blufi_wifi(None, None, None), ("", ""))
        self.assertEqual(oc.resolve_blufi_wifi(None, None, {"networks": []}), ("", ""))


class BlufiSsidAvailable(unittest.TestCase):
    def _check(self, provisioning, wifi_config):
        hub = {oc.CONF_ID: "x20", oc.CONF_WIFI_PROVISIONING: provisioning}
        oc._blufi_ssid_available(hub, wifi_config)

    def test_provisioning_without_any_ssid_fails(self):
        with self.assertRaises(cv.Invalid):
            self._check(True, None)

    def test_the_node_wifi_is_enough(self):
        self._check(True, {"networks": [{"ssid": "home"}]})

    def test_no_provisioning_needs_nothing(self):
        self._check(False, None)


class CloudReceiverWebServer(unittest.TestCase):
    # The receiver reuses web_server_base, so a web_server must be configured.
    def _check(self, cloud_receiver, with_web_server):
        hub = {oc.CONF_ID: "x20", oc.CONF_CLOUD_RECEIVER: cloud_receiver}
        full = {oc.DOMAIN: [hub]}
        if with_web_server:
            full["web_server"] = {"port": 80}
        oc._cloud_receiver_needs_web_server(hub, full)

    def test_receiver_with_web_server_ok(self):
        self._check(True, True)

    def test_receiver_without_web_server_fails(self):
        with self.assertRaises(cv.Invalid):
            self._check(True, False)

    def test_no_receiver_needs_nothing(self):
        self._check(False, False)


class WeatherOption(unittest.TestCase):
    def test_takes_a_weather_entity(self):
        self.assertEqual(
            oc._weather_entity("weather.forecast_home"), "weather.forecast_home"
        )

    def test_refuses_another_domain(self):
        with self.assertRaises(cv.Invalid):
            oc._weather_entity("sensor.outside_temperature")

    def test_needs_the_receiver_and_a_clock(self):
        hub = {oc.CONF_WEATHER: "weather.home", oc.CONF_CLOUD_RECEIVER: True}
        with self.assertRaises(cv.Invalid):
            oc._validate_weather(hub)
        hub[oc.CONF_TIME_ID] = "ha_time"
        self.assertEqual(oc._validate_weather(hub), hub)
        hub[oc.CONF_CLOUD_RECEIVER] = False
        with self.assertRaises(cv.Invalid):
            oc._validate_weather(hub)

    def test_unset_needs_nothing(self):
        self.assertEqual(oc._validate_weather({}), {})


class WeatherApiAndOneHub(unittest.TestCase):
    # The entity comes over the native API and the brush request names no brush.
    def test_needs_api(self):
        hub = {oc.CONF_ID: "x20", oc.CONF_WEATHER: "weather.home"}
        with self.assertRaises(cv.Invalid):
            oc._weather_needs_api_and_one_hub(hub, {oc.DOMAIN: [hub]})
        oc._weather_needs_api_and_one_hub(hub, {oc.DOMAIN: [hub], "api": {}})

    def test_second_weather_hub_fails(self):
        first = {oc.CONF_ID: "x20", oc.CONF_WEATHER: "weather.home"}
        second = {oc.CONF_ID: "x20b", oc.CONF_WEATHER: "weather.home"}
        full = {oc.DOMAIN: [first, second], "api": {}}
        oc._weather_needs_api_and_one_hub(first, full)
        with self.assertRaises(cv.Invalid):
            oc._weather_needs_api_and_one_hub(second, full)

    def test_hub_without_weather_needs_nothing(self):
        oc._weather_needs_api_and_one_hub({oc.CONF_ID: "elite"}, {oc.DOMAIN: []})


if __name__ == "__main__":
    unittest.main()
