#!/usr/bin/env python3
"""Read-only BDFS base-OS inventory for Ageless compatibility planning.

This tool never sources os-release, executes conversion scripts, installs packages,
or changes the inspected root. It emits descriptive metadata only.
"""
from __future__ import annotations

import argparse
import json
import platform
import re
from pathlib import Path
from typing import Any

KEYS = ("ID", "VERSION_ID", "NAME", "PRETTY_NAME", "ID_LIKE")


def parse_os_release(path: Path) -> dict[str, str]:
    """Parse the simple os-release assignment format without shell evaluation."""
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[A-Z0-9_]+", key):
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]
        values[key] = value
    return values


def inventory(root: Path, manifest_path: Path | None = None) -> dict[str, Any]:
    root = root.resolve()
    manifest: dict[str, Any] = {}
    if manifest_path:
        loaded = json.loads(manifest_path.read_text(encoding="utf-8"))
        if not isinstance(loaded, dict):
            raise ValueError("image manifest must contain a JSON object")
        manifest = loaded

    release_path = root / "etc/os-release"
    release = parse_os_release(release_path)
    original_path = root / "etc/os-release.pre-ageless"
    if release.get("ID", "").lower() == "ageless" and original_path.is_file():
        release = parse_os_release(original_path)
        identity_source = "os-release.pre-ageless"
    elif manifest.get("base_os") and not release.get("ID"):
        base = manifest["base_os"]
        if not isinstance(base, dict):
            raise ValueError("manifest base_os must be a JSON object")
        release = {str(k).upper(): str(v) for k, v in base.items() if v is not None}
        identity_source = "image-manifest"
    else:
        identity_source = "etc/os-release"

    def first(*values: Any) -> str | None:
        return next((str(v) for v in values if v not in (None, "")), None)

    arch = first(manifest.get("architecture"), platform.machine() if root == Path("/") else None)
    pkg = first(manifest.get("package_manager"))
    if not pkg:
        for name in ("apt-get", "apt", "dpkg", "dnf", "yum", "zypper", "pacman", "apk", "xbps-install", "emerge"):
            if any((root / p / name).exists() for p in ("usr/bin", "usr/sbin", "bin", "sbin")):
                pkg = name
                break
    init = first(manifest.get("init_system"))
    if not init:
        init_link = root / "sbin/init"
        if init_link.is_symlink():
            init = init_link.resolve().name
        elif (root / "usr/lib/systemd/systemd").exists():
            init = "systemd"
        elif (root / "sbin/openrc-init").exists():
            init = "openrc"
        elif root == Path("/") and Path("/proc/1/comm").is_file():
            init = Path("/proc/1/comm").read_text(encoding="utf-8").strip() or None

    metadata = {
        "schema_version": 1,
        "profile": "bdfs.base-os-inventory",
        "identity_source": identity_source,
        "distribution": {
            "id": first(release.get("ID")),
            "id_like": first(release.get("ID_LIKE")),
            "name": first(release.get("NAME")),
            "pretty_name": first(release.get("PRETTY_NAME")),
            "release": first(release.get("VERSION_ID")),
        },
        "architecture": arch,
        "package_manager": pkg,
        "init_system": init,
        "kernel": {
            "release": first(manifest.get("kernel_release")),
            "source": first(manifest.get("kernel_source")),
            "abi": first(manifest.get("kernel_abi")),
        },
        "provenance": {
            "manifest_supplied": bool(manifest_path),
            "kernel_metadata_source": "image-manifest" if any(manifest.get(k) for k in ("kernel_release", "kernel_source", "kernel_abi")) else "unknown",
        },
        "compatibility": {"status": "unknown", "reason": "inventory does not establish provider support"},
        "safety": {
            "read_only": True,
            "conversion_executed": False,
            "patches_applied": False,
            "builds_started": False,
            "packages_installed": False,
        },
    }
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("/"), help="filesystem root to inspect")
    parser.add_argument("--manifest", type=Path, help="optional trusted image metadata JSON")
    args = parser.parse_args()
    print(json.dumps(inventory(args.root, args.manifest), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
