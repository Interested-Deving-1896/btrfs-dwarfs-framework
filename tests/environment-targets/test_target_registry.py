import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

MODULE_PATH = Path(__file__).parents[2] / "environments" / "resolve_target.py"
SPEC = importlib.util.spec_from_file_location("resolve_target", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


class TargetRegistryTests(unittest.TestCase):
    def setUp(self):
        self.registry = MODULE.load_registry()

    def test_kde_neon_is_default_for_both_directions(self):
        for operation in ("import", "export"):
            result = MODULE.resolve_target(self.registry, operation)
            self.assertEqual(result["target"], "kde-neon-editions")
            self.assertEqual(result["resolutionStatus"], "selection-only")
            self.assertFalse(result["conversionPerformed"])

    def test_additional_os_can_be_selected_without_changing_defaults(self):
        self.registry["targets"].append({
            "id": "example-secondary-os",
            "name": "Example secondary OS",
            "enabled": True,
            "profile": "example/profile.json",
            "adapter": "example-adapter",
            "selectionOperations": ["import", "export"],
        })
        self.assertEqual(
            MODULE.resolve_target(self.registry, "import", "example-secondary-os")["target"],
            "example-secondary-os",
        )
        self.assertEqual(
            MODULE.resolve_target(self.registry, "export")["target"],
            "kde-neon-editions",
        )

    def test_disabled_target_is_rejected(self):
        self.registry["targets"][0]["enabled"] = False
        with self.assertRaises(MODULE.RegistryError):
            MODULE.resolve_target(self.registry, "import")

    def test_unknown_target_is_rejected(self):
        with self.assertRaises(MODULE.RegistryError):
            MODULE.resolve_target(self.registry, "export", "not-configured")

    def test_duplicate_ids_are_rejected(self):
        duplicate = dict(self.registry["targets"][0])
        self.registry["targets"].append(duplicate)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "registry.json"
            path.write_text(json.dumps(self.registry), encoding="utf-8")
            with self.assertRaises(MODULE.RegistryError):
                MODULE.load_registry(path)

    def test_unsafe_profile_paths_are_rejected(self):
        self.registry["targets"][0]["profile"] = "../outside.json"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "registry.json"
            path.write_text(json.dumps(self.registry), encoding="utf-8")
            with self.assertRaises(MODULE.RegistryError):
                MODULE.load_registry(path)


if __name__ == "__main__":
    unittest.main()
