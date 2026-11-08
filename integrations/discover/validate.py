#!/usr/bin/env python3
"""Validate the declarative KDE Discover integration boundary."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
metadata = json.loads((Path(__file__).parent / "integration.json").read_text(encoding="utf-8"))
readme = (Path(__file__).parent / "README.md").read_text(encoding="utf-8")

assert metadata["schema_version"] == 1
assert metadata["id"] == "kde-discover"
assert metadata["role"] == "desktop-application-management-frontend"
assert metadata["repository"].endswith("/Interested-Deving-1896/discover")
assert metadata["upstream_repository"].endswith("/KDE/discover")
assert metadata["source_revision"] is None
assert metadata["support_status"] == "unknown"
assert metadata["capabilities"]["package_transactions"] is False
assert metadata["capabilities"]["kport_backend"] is False
assert metadata["capabilities"]["filesystem_mutation"] is False
for key in ("execute_untrusted_pacscript", "install_packages", "remove_packages", "modify_host_filesystem", "publish_artifacts"):
    assert metadata["safety"][key] is False
for marker in ("KPort", "BDFS", "not yet pinned", "not implemented"):
    assert marker.lower() in readme.lower(), f"missing boundary statement: {marker}"

print("KDE Discover integration metadata valid; privileged capabilities remain disabled.")
