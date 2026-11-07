#!/usr/bin/env python3
"""Focused, read-only invariants for the BDFS Ageless Linux profile."""
import json
import re
import subprocess
import sys
import tempfile
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
# Exercise the read-only inventory contract against disposable filesystem fixtures.
inventory_tool = ROOT / "integrations/ageless-linux/inventory.py"
with tempfile.TemporaryDirectory() as tmp:
    fixture = Path(tmp) / "root"
    (fixture / "etc").mkdir(parents=True)
    (fixture / "etc/os-release").write_text('ID=ageless\\nVERSION_ID=1\\n', encoding="utf-8")
    (fixture / "etc/os-release.pre-ageless").write_text(
        'ID=debian\\nVERSION_ID="13"\\nNAME="Debian GNU/Linux"\\n',
        encoding="utf-8",
    )
    manifest = Path(tmp) / "manifest.json"
    manifest.write_text(json.dumps({
        "architecture": "x86_64",
        "package_manager": "apt",
        "init_system": "systemd",
        "kernel_release": "6.12.0",
        "kernel_source": "debian",
    }), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, str(inventory_tool), "--root", str(fixture), "--manifest", str(manifest)],
        check=True, capture_output=True, text=True,
    )
    report = json.loads(result.stdout)
    assert report["identity_source"] == "os-release.pre-ageless"
    assert report["distribution"]["id"] == "debian"
    assert report["distribution"]["release"] == "13"
    assert report["architecture"] == "x86_64"
    assert report["package_manager"] == "apt"
    assert report["init_system"] == "systemd"
    assert report["kernel"]["source"] == "debian"
    assert report["compatibility"]["status"] == "unknown"
    assert report["safety"]["read_only"] is True
    assert report["safety"]["conversion_executed"] is False
    assert report["safety"]["patches_applied"] is False
    assert report["safety"]["builds_started"] is False

print("BDFS Ageless profile and read-only inventory invariants: PASS")
