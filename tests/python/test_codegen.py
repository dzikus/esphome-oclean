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


if __name__ == "__main__":
    unittest.main()
