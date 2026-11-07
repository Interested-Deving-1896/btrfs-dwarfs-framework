"""Focused tests for the BDFS artifact manifest validator."""
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "scripts" / "validate-bdfs-artifact-manifest.py"
SCHEMA = ROOT / "doc" / "schemas" / "bdfs-artifact-manifest.schema.json"


def valid_manifest():
    return {
        "schemaVersion": "1.0",
        "artifactId": "test-artifact-1",
        "kind": "source-bundle",
        "createdAt": "2026-11-07T12:00:00Z",
        "source": {"adapter": "test"},
        "payload": {"format": "tar"},
        "security": {
            "containsSecrets": False,
            "containsMachineIdentity": False,
            "hooksPresent": False,
            "hooksDefaultState": "absent",
        },
    }


class ManifestValidatorTests(unittest.TestCase):
    def validate(self, manifest, *extra):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(VALIDATOR), str(path), *map(str, extra)],
                text=True,
                capture_output=True,
                check=False,
            )

    def test_accepts_valid_manifest(self):
        result = self.validate(valid_manifest())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_unknown_schema_version(self):
        manifest = valid_manifest()
        manifest["schemaVersion"] = "2.0"
        self.assertEqual(self.validate(manifest).returncode, 1)

    def test_rejects_secret_bearing_manifest(self):
        manifest = valid_manifest()
        manifest["security"]["containsSecrets"] = True
        self.assertEqual(self.validate(manifest).returncode, 1)

    def test_rejects_bad_digest_shape(self):
        manifest = valid_manifest()
        manifest["payload"]["digest"] = "md5:1234"
        self.assertEqual(self.validate(manifest).returncode, 1)

    def test_import_policy_rejects_hook_execution(self):
        manifest = valid_manifest()
        manifest["capabilities"] = ["inspect", "run-hooks"]
        result = self.validate(manifest, "--for-import")
        self.assertEqual(result.returncode, 1)
        self.assertIn("run-hooks", result.stderr)

    def test_import_policy_requires_hooks_inert(self):
        manifest = valid_manifest()
        manifest["security"].update(hooksPresent=True, hooksDefaultState="absent")
        result = self.validate(manifest, "--for-import")
        self.assertEqual(result.returncode, 1)
        self.assertIn("inert", result.stderr)

    def test_verifies_payload_checksum(self):
        with tempfile.TemporaryDirectory() as directory:
            payload = Path(directory) / "payload.tar"
            payload.write_bytes(b"fixture payload")
            manifest = valid_manifest()
            manifest["payload"]["digest"] = "sha256:" + hashlib.sha256(b"fixture payload").hexdigest()
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(VALIDATOR), str(path), "--payload", str(payload)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_payload_checksum_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            payload = Path(directory) / "payload.tar"
            payload.write_bytes(b"not the expected payload")
            manifest = valid_manifest()
            manifest["payload"]["digest"] = "sha256:" + hashlib.sha256(b"expected").hexdigest()
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(VALIDATOR), str(path), "--payload", str(payload)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("checksum mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main()
