#!/usr/bin/env python3
"""Build conservative, read-only Ageless compatibility reports for kernel providers."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

REQUIRED = (
    "distribution.id",
    "distribution.release",
    "architecture",
    "package_manager",
    "init_system",
)


def get_path(value: dict[str, Any], dotted: str) -> Any:
    current: Any = value
    for part in dotted.split("."):
        if not isinstance(current, dict) or part not in current:
            return None
        current = current[part]
    return current


def build_report(inventory: dict[str, Any], providers: dict[str, dict[str, Any]]) -> dict[str, Any]:
    if inventory.get("schema_version") != 1 or inventory.get("profile") != "bdfs.base-os-inventory":
        raise ValueError("unsupported BDFS inventory schema/profile")
    if not isinstance(providers, dict) or not providers:
        raise ValueError("at least one provider contract is required")

    missing = [field for field in REQUIRED if get_path(inventory, field) in (None, "")]
    reports: dict[str, Any] = {}
    for name, contract in sorted(providers.items()):
        if not isinstance(contract, dict):
            raise ValueError(f"provider contract for {name} must be a JSON object")
        if contract.get("schema_version") != 1 or contract.get("profile") != "ageless-linux":
            raise ValueError(f"provider contract for {name} has an unsupported schema/profile")
        declared = contract.get("inventory_contract", {})
        if declared.get("producer") != "bdfs.integrations.ageless-linux.inventory":
            raise ValueError(f"provider {name} does not declare the BDFS inventory producer")
        if declared.get("schema_version") != 1:
            raise ValueError(f"provider {name} declares an unsupported inventory schema")
        reports[name] = {
            "provider": name,
            "provider_profile": contract.get("provider"),
            "compatibility_status": "unknown",
            "support_status": "inventory-only",
            "inventory_complete": not missing,
            "missing_required_inventory_fields": missing,
            "base_os": inventory.get("distribution", {}),
            "architecture": inventory.get("architecture"),
            "package_manager": inventory.get("package_manager"),
            "init_system": inventory.get("init_system"),
            "kernel": inventory.get("kernel", {}),
            "provider_constraints": contract.get("provider_constraints", {}),
            "evidence_required": contract.get("evidence_required", []),
            "decision": (
                "required inventory fields are missing"
                if missing
                else "inventory is descriptive only; provider-specific isolated evidence is absent"
            ),
            "actions": {
                "conversion": False,
                "patch_apply": False,
                "build": False,
                "install": False,
                "publish": False,
            },
        }
    return {
        "schema_version": 1,
        "profile": "bdfs.ageless-provider-compatibility-report",
        "source_inventory_schema": 1,
        "compatibility_status": "unknown",
        "reports": reports,
        "safety": {
            "read_only": True,
            "conversion_executed": False,
            "patches_applied": False,
            "builds_started": False,
            "packages_installed": False,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=Path, help="BDFS inventory JSON")
    parser.add_argument(
        "--provider", action="append", required=True, metavar="NAME=PATH",
        help="provider compatibility contract; may be repeated",
    )
    parser.add_argument("--output", type=Path, help="write report to this path; stdout otherwise")
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
    providers: dict[str, dict[str, Any]] = {}
    for entry in args.provider:
        if "=" not in entry:
            parser.error("--provider must be NAME=PATH")
        name, filename = entry.split("=", 1)
        if not name or name in providers:
            parser.error("provider names must be non-empty and unique")
        providers[name] = json.loads(Path(filename).read_text(encoding="utf-8"))
    report = build_report(inventory, providers)
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
