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

import oclean as oc
import oclean.binary_sensor as ocbs
import oclean.button as ocbtn
import oclean.number as ocnum
import oclean.select as ocsel
import oclean.sensor as ocsens
import oclean.switch as ocsw
import oclean.text_sensor as octs

PLATFORMS = {
    "sensor": ocsens,
    "binary_sensor": ocbs,
    "text_sensor": octs,
    "switch": ocsw,
    "select": ocsel,
    "number": ocnum,
    "button": ocbtn,
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

    def test_dev_gated_keys_exist(self):
        self.assertLessEqual(ocsens.DEV_SENSOR_KEYS, self._keys(ocsens))
        self.assertLessEqual(ocbs.DEV_BINARY_SENSOR_KEYS, self._keys(ocbs))
        self.assertLessEqual(ocsw.DEV_SWITCH_KEYS, self._keys(ocsw))


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


if __name__ == "__main__":
    unittest.main()
