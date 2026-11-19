# SPDX-License-Identifier: GPL-2.0-or-later
import base64
import hashlib
import importlib.util
import json
import os
import pathlib
import tempfile
import unittest
from unittest import mock

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "tools" / "bdfs-tree-inspect.py"
SPEC = importlib.util.spec_from_file_location("bdfs_tree_inspect", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


class TreeInspectTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name) / "root"
        self.root.mkdir()
        (self.root / "nested").mkdir()
        (self.root / "nested" / "payload.txt").write_bytes(b"bdfs tree fixture\n")
        os.link(self.root / "nested" / "payload.txt", self.root / "payload-hardlink")
        outside = pathlib.Path(self.temp.name) / "outside-secret"
        outside.write_text("must not be traversed", encoding="utf-8")
        os.symlink(outside, self.root / "outside-link")

    def tearDown(self):
        self.temp.cleanup()

    def test_manifest_records_file_hash_and_hardlink_identity(self):
        manifest = MODULE.inspect_tree(str(self.root))
        entries = {entry["path"]: entry for entry in manifest["entries"]}
        payload = entries["nested/payload.txt"]
        alias = entries["payload-hardlink"]
        expected = hashlib.sha256(b"bdfs tree fixture\n").hexdigest()
        self.assertEqual(payload["sha256"], expected)
        self.assertEqual(alias["sha256"], expected)
        self.assertEqual(payload["hardlink_group"], alias["hardlink_group"])
        self.assertEqual(manifest["manifest_version"], 1)
        self.assertEqual(manifest["summary"]["type_counts"]["file"], 2)

    def test_symlink_is_recorded_but_target_is_not_traversed(self):
        manifest = MODULE.inspect_tree(str(self.root))
        entries = {entry["path"]: entry for entry in manifest["entries"]}
        link = entries["outside-link"]
        target = base64.b64decode(link["target_b64"]).decode()
        self.assertEqual(link["type"], "symlink")
        self.assertTrue(target.endswith("outside-secret"))
        self.assertNotIn("outside-secret", entries)

    def test_path_bytes_are_preserved(self):
        manifest = MODULE.inspect_tree(str(self.root))
        for entry in manifest["entries"]:
            decoded = base64.b64decode(entry["path_b64"])
            if entry["path"] == ".":
                self.assertEqual(decoded, b"")
            else:
                self.assertEqual(os.fsdecode(decoded), entry["path"])

    def test_manifest_is_deterministic_for_unchanged_tree(self):
        first = MODULE.inspect_tree(str(self.root))
        second = MODULE.inspect_tree(str(self.root))
        self.assertEqual(
            json.dumps(first, sort_keys=True),
            json.dumps(second, sort_keys=True),
        )

    def test_output_inside_source_tree_is_rejected(self):
        output = self.root / "manifest.json"
        with mock.patch("sys.argv", [
            str(SCRIPT), "--source", str(self.root), "--output", str(output)
        ]):
            self.assertEqual(MODULE.main(), 1)
        self.assertFalse(output.exists())

    def test_strict_mode_fails_closed_on_unknown_fidelity(self):
        with mock.patch("sys.argv", [str(SCRIPT), "--source", str(self.root), "--strict"]):
            self.assertEqual(MODULE.main(), 1)

    def test_source_files_are_not_modified(self):
        before = (self.root / "nested" / "payload.txt").read_bytes()
        MODULE.inspect_tree(str(self.root))
        after = (self.root / "nested" / "payload.txt").read_bytes()
        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
