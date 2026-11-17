#!/usr/bin/env python3
"""Resolve BDFS OS import/export targets without performing conversions."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any

DEFAULT_REGISTRY = Path(__file__).with_name("target-registry.json")
VALID_OPERATIONS = {"import", "export"}


class RegistryError(ValueError):
    """Raised when the target registry is malformed or unsafe."""


def load_registry(path: Path = DEFAULT_REGISTRY) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RegistryError(f"cannot read target registry: {exc}") from exc

    if not isinstance(data, dict) or data.get("schemaVersion") != "1.0":
        raise RegistryError("registry must be an object with schemaVersion '1.0'")
    targets = data.get("targets")
    if not isinstance(targets, list) or not targets:
        raise RegistryError("registry targets must be a non-empty array")

    seen: set[str] = set()
    for target in targets:
        if not isinstance(target, dict):
            raise RegistryError("each target must be an object")
        target_id = target.get("id")
        if not isinstance(target_id, str) or not target_id.strip():
            raise RegistryError("each target requires a non-empty id")
        if target_id in seen:
            raise RegistryError(f"duplicate target id: {target_id}")
        seen.add(target_id)
        if not isinstance(target.get("enabled"), bool):
            raise RegistryError(f"target {target_id} requires boolean enabled")
        for field in ("profile", "adapter"):
            value = target.get(field)
            if not isinstance(value, str) or not value.strip():
                raise RegistryError(f"target {target_id} requires {field}")
            if Path(value).is_absolute() or ".." in Path(value).parts:
                raise RegistryError(f"target {target_id} has unsafe {field} path")
        operations = target.get("selectionOperations")
        if not isinstance(operations, list) or not set(operations) <= VALID_OPERATIONS:
            raise RegistryError(f"target {target_id} has invalid selectionOperations")

    for operation in VALID_OPERATIONS:
        default_id = data.get(f"default{operation.title()}Target")
        if default_id not in seen:
            raise RegistryError(f"default {operation} target is not registered")
        selected = next(item for item in targets if item["id"] == default_id)
        if not selected["enabled"]:
            raise RegistryError(f"default {operation} target is disabled")
        if operation not in selected["selectionOperations"]:
            raise RegistryError(f"default {operation} target does not declare selection support")
    return data


def resolve_target(registry: dict[str, Any], operation: str,
                   target_id: str | None = None) -> dict[str, Any]:
    if operation not in VALID_OPERATIONS:
        raise RegistryError(f"unsupported operation: {operation}")
    selected_id = target_id or registry[f"default{operation.title()}Target"]
    for target in registry["targets"]:
        if target["id"] == selected_id:
            if not target["enabled"]:
                raise RegistryError(f"target is disabled: {selected_id}")
            if operation not in target["selectionOperations"]:
                raise RegistryError(
                    f"target {selected_id} does not declare {operation} selection"
                )
            return {
                "schemaVersion": "1.0",
                "operation": operation,
                "target": selected_id,
                "profile": target["profile"],
                "adapter": target["adapter"],
                "resolutionStatus": "selection-only",
                "conversionPerformed": False,
                "deploymentPerformed": False,
            }
    raise RegistryError(f"unknown target: {selected_id}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    parser.add_argument("--operation", choices=sorted(VALID_OPERATIONS))
    parser.add_argument("--target", help="explicit enabled target id; overrides the default")
    parser.add_argument("--list", action="store_true", help="list configured targets")
    args = parser.parse_args()

    try:
        registry = load_registry(args.registry)
        if args.list:
            print(json.dumps({
                "defaultImportTarget": registry["defaultImportTarget"],
                "defaultExportTarget": registry["defaultExportTarget"],
                "targets": registry["targets"],
            }, indent=2))
            return 0
        if not args.operation:
            parser.error("--operation is required unless --list is used")
        print(json.dumps(resolve_target(registry, args.operation, args.target), indent=2))
        return 0
    except RegistryError as exc:
        print(f"target registry error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
