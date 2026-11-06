import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "kumbe-plan.py"
spec = importlib.util.spec_from_file_location("kumbe_plan", SCRIPT)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def manifest():
    return {
        "schema_version": "1.0.0",
        "migration_id": "test-lts-plan",
        "provider": "other",
        "kernel_line": "lts",
        "source": {"repository": "upstream/linux", "url": "https://example.invalid/linux",
                   "ref": "v6.12.1", "commit": "a" * 40, "kernel_version": "6.12.1"},
        "target": {"repository": "downstream/linux", "ref": "lts-test",
                   "kernel_version": "6.6.90", "architectures": ["x86_64"]},
        "operation": "backport",
        "changes": [
            {"change_id": "base", "upstream_commit": "b" * 40,
             "classification": "clean-backport", "subsystem": "net",
             "license": "GPL-2.0-only", "risk": "low", "dependencies": [],
             "abi_impact": "none-known"},
            {"change_id": "dependent", "upstream_commit": "c" * 40,
             "classification": "clean-backport", "subsystem": "net",
             "license": "GPL-2.0-only", "risk": "low", "dependencies": ["base"],
             "abi_impact": "none-known"}
        ],
        "policy": {"dry_run": True, "human_approval_required": True,
                   "protected_branch_write": False, "allow_eol_target": False},
        "status": "proposed"
    }


class PlannerTests(unittest.TestCase):
    def test_dependency_order_is_stable(self):
        result = module.plan(manifest())
        self.assertEqual(result["decision"], "dry-run-ready")
        self.assertEqual(result["ordered_changes"], ["base", "dependent"])
        self.assertTrue(result["read_only"])
        self.assertFalse(result["patch_application_performed"])

    def test_missing_dependency_blocks(self):
        data = manifest()
        data["changes"][1]["dependencies"] = ["absent"]
        result = module.plan(data)
        self.assertEqual(result["decision"], "blocked")
        self.assertTrue(any("missing change absent" in x for x in result["blockers"]))

    def test_dependency_cycle_blocks(self):
        data = manifest()
        data["changes"][0]["dependencies"] = ["dependent"]
        result = module.plan(data)
        self.assertEqual(result["decision"], "blocked")
        self.assertTrue(any("cycle" in x for x in result["blockers"]))

    def test_high_risk_requires_approval(self):
        data = manifest()
        data["changes"][0]["risk"] = "critical"
        result = module.plan(data)
        self.assertEqual(result["decision"], "approval-required")
        self.assertTrue(result["approval_gates"])

    def test_unpinned_source_blocks(self):
        data = manifest()
        data["source"]["commit"] = "main"
        self.assertEqual(module.plan(data)["decision"], "blocked")

    def test_eol_requires_explicit_opt_in(self):
        data = manifest()
        data["kernel_line"] = "legacy-eol"
        self.assertEqual(module.plan(data)["decision"], "blocked")

    def test_protected_branch_write_must_be_false(self):
        data = manifest()
        data["policy"]["protected_branch_write"] = True
        self.assertEqual(module.plan(data)["decision"], "blocked")

    def test_malformed_nested_objects_block_without_crashing(self):
        for field in ("source", "target", "policy"):
            data = manifest()
            data[field] = ["malformed"]
            result = module.plan(data)
            self.assertEqual(result["decision"], "blocked")
            self.assertTrue(result["blockers"])

    def test_non_string_dependency_blocks_without_crashing(self):
        data = manifest()
        data["changes"][1]["dependencies"] = [["not", "an", "id"]]
        result = module.plan(data)
        self.assertEqual(result["decision"], "blocked")
        self.assertTrue(any("non-empty string IDs" in x for x in result["blockers"]))

    def test_malformed_risk_is_conservatively_approval_gated(self):
        data = manifest()
        data["changes"][0]["risk"] = ["unknown"]
        result = module.plan(data)
        self.assertEqual(result["decision"], "approval-required")
        self.assertTrue(any("unknown risk" in x for x in result["approval_gates"]))

    def test_v11_accepts_independently_pinned_git_and_archive_inputs(self):
        data = manifest()
        data["schema_version"] = "1.1.0"
        data.pop("source")
        data["source_inputs"] = [
            {"input_id": "linux-upstream", "role": "kernel-source", "kind": "git",
             "repository": "upstream/linux", "url": "https://example.invalid/linux.git",
             "ref": "refs/tags/v6.12.1", "commit": "a" * 40,
             "kernel_version": "6.12.1", "integrity": "verified"},
            {"input_id": "vendor-patches", "role": "provider-patches", "kind": "archive",
             "url": "https://example.invalid/patches.tar.xz", "archive_version": "2026.11",
             "sha256": "d" * 64, "integrity": "verified"}
        ]
        result = module.plan(data)
        self.assertEqual(result["decision"], "dry-run-ready")
        self.assertTrue(any("does not fetch sources" in x for x in result["warnings"]))

    def test_v11_unverified_archive_blocks(self):
        data = manifest()
        data["schema_version"] = "1.1.0"
        data.pop("source")
        data["source_inputs"] = [
            {"input_id": "linux", "role": "kernel-source", "kind": "git",
             "repository": "upstream/linux", "url": "https://example.invalid/linux.git",
             "ref": "v6.12.1", "commit": "a" * 40, "integrity": "verified"},
            {"input_id": "patches", "role": "provider-patches", "kind": "archive",
             "url": "https://example.invalid/patches.tar.xz", "archive_version": "1",
             "sha256": "d" * 64, "integrity": "unverified"}
        ]
        result = module.plan(data)
        self.assertEqual(result["decision"], "blocked")
        self.assertTrue(any("integrity is 'unverified'" in x for x in result["blockers"]))

    def test_malformed_schema_version_blocks_without_crashing(self):
        data = manifest()
        data["schema_version"] = ["1.1.0"]
        result = module.plan(data)
        self.assertEqual(result["decision"], "blocked")
        self.assertTrue(any("Unsupported manifest schema_version" in x for x in result["blockers"]))

    def test_v11_requires_unique_ids_and_kernel_source(self):
        data = manifest()
        data["schema_version"] = "1.1.0"
        data.pop("source")
        data["source_inputs"] = [
            {"input_id": "same", "role": "provider-patches", "kind": "archive",
             "url": "https://example.invalid/a.tar", "archive_version": "1",
             "sha256": "d" * 64, "integrity": "verified"},
            {"input_id": "same", "role": "provider-patches", "kind": "archive",
             "url": "https://example.invalid/b.tar", "archive_version": "1",
             "sha256": "e" * 64, "integrity": "verified"}
        ]
        result = module.plan(data)
        self.assertEqual(result["decision"], "blocked")
        self.assertTrue(any("Duplicate source input_id" in x for x in result["blockers"]))
        self.assertTrue(any("kernel-source" in x for x in result["blockers"]))

    def test_cli_emits_json_without_mutating_manifest(self):
        data = manifest()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            original = json.dumps(data, indent=2)
            path.write_text(original, encoding="utf-8")
            result = subprocess.run([sys.executable, str(SCRIPT), str(path)],
                                    capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["decision"], "dry-run-ready")
            self.assertEqual(path.read_text(encoding="utf-8"), original)


if __name__ == "__main__":
    unittest.main()
