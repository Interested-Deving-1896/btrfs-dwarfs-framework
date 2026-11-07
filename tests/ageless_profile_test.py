#!/usr/bin/env python3
"""Focused, read-only invariants for the BDFS Ageless Linux profile."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
profile = json.loads((ROOT / "integrations/ageless-linux/profile.json").read_text())
lock = json.loads((ROOT / "integrations/ageless-linux/source-lock.json").read_text())

assert profile["profile"] == "ageless-linux"
assert profile["integration_kind"] == "os-identity-policy-conversion"
assert profile["support_status"] == "inventory-only"
assert profile["compatibility_status"] == "unknown"
assert profile["kernel_source_role"] == "none"
assert profile["base_distribution"]["identity_source"] == "pre-conversion-os-release-or-image-manifest"
assert profile["base_distribution"]["status"] == "must-be-detected-and-verified"
assert profile["conversion"]["host_execution_allowed"] is False
assert profile["conversion"]["isolated_test_required"] is True
for key in ("conversion_execution", "kernel_build", "package_publication", "installation"):
    assert profile["safety"][key] is False, key
assert lock["source"]["commit"] == "1e1b00f45d494a41412403e9e909b0cc737bc2ae"
assert re.fullmatch(r"[0-9a-f]{40}", lock["source"]["tree"])
assert lock["source"]["commit_signature"] == "unsigned"
assert lock["source"]["integrity"] == "content-addressed-but-not-authenticated"
assert lock["compatibility"]["status"] == "unknown"
assert lock["compatibility"]["supported_base_matrix"] == []
assert lock["safety"]["execute_conversion"] is False
assert lock["safety"]["execute_upstream_tests_on_runner"] is False
print("BDFS Ageless profile invariants: PASS")
