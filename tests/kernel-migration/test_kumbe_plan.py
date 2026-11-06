#!/usr/bin/env python3
"""Unit tests for the read-only KUMBE dependency planner."""
import unittest
from copy import deepcopy
from pathlib import Path
import json
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from kumbe_plan import build_plan

FIXTURE = json.loads((ROOT / "tests/kernel-migration/fixtures/valid-audit-only.json").read_text(encoding="utf-8"))


class PlannerTests(unittest.TestCase):
    def test_stable_dependency_order(self):
        manifest = deepcopy(FIXTURE)
        manifest["changes"] = [
            {"change_id": "c", "dependencies": ["a"], "classification": "clean-backport", "risk": "low", "abi_impact": "internal"},
            {"change_id": "b", "dependencies": [], "classification": "clean-backport", "risk": "low", "abi_impact": "internal"},
            {"change_id": "a", "dependencies": ["b"], "classification": "clean-backport", "risk": "low", "abi_impact": "internal"},
        ]
        plan = build_plan(manifest)
        self.assertTrue(plan["valid"])
        self.assertEqual(plan["dependency_order"], ["b", "a", "c"])
        self.assertEqual(plan["decision"], "manual-approval-required")

    def test_duplicate_and_missing_dependencies_are_errors(self):
        manifest = deepcopy(FIXTURE)
        manifest["changes"] = [
            {"change_id": "a", "dependencies": ["missing"]},
            {"change_id": "a", "dependencies": []},
        ]
        plan = build_plan(manifest)
        self.assertFalse(plan["valid"])
        self.assertTrue(any("Duplicate" in error for error in plan["errors"]))
        self.assertTrue(any("missing change" in error for error in plan["errors"]))

    def test_cycle_is_reported(self):
        manifest = deepcopy(FIXTURE)
        manifest["changes"] = [
            {"change_id": "a", "dependencies": ["b"]},
            {"change_id": "b", "dependencies": ["a"]},
        ]
        plan = build_plan(manifest)
        self.assertFalse(plan["valid"])
        self.assertTrue(any("Dependency cycle" in error for error in plan["errors"]))

    def test_high_risk_and_unknown_abi_block(self):
        manifest = deepcopy(FIXTURE)
        manifest["changes"] = [
            {"change_id": "a", "dependencies": [], "classification": "clean-backport", "risk": "high", "abi_impact": "unknown"}
        ]
        plan = build_plan(manifest)
        self.assertEqual(len(plan["blocked_changes"]), 2)
        self.assertFalse(plan["execution"]["patches_applied"])
        self.assertFalse(plan["execution"]["ready_for_isolated_apply"])


if __name__ == "__main__":
    unittest.main()
