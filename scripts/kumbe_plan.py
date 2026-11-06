#!/usr/bin/env python3
"""Read-only KUMBE dependency and policy planner; never applies patches or fetches sources."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


BLOCKING_CLASSIFICATIONS = {"blocked", "rejected", "not-applicable", "semantic-manual-port"}


def build_plan(manifest: dict[str, Any]) -> dict[str, Any]:
    changes = manifest.get("changes", [])
    by_id: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    blockers: list[dict[str, str]] = []

    for change in changes:
        change_id = change.get("change_id")
        if not isinstance(change_id, str) or not change_id:
            errors.append("Every change must have a non-empty change_id.")
            continue
        if change_id in by_id:
            errors.append(f"Duplicate change_id: {change_id}")
        else:
            by_id[change_id] = change

    dependencies: dict[str, set[str]] = {}
    for change_id, change in by_id.items():
        deps = set(change.get("dependencies", []))
        dependencies[change_id] = deps
        for dependency in sorted(deps):
            if dependency not in by_id:
                errors.append(f"{change_id} depends on missing change: {dependency}")
        classification = change.get("classification", "blocked")
        if classification in BLOCKING_CLASSIFICATIONS:
            blockers.append({"change_id": change_id, "reason": f"classification:{classification}"})
        if change.get("risk") in {"high", "critical", "unknown"}:
            blockers.append({"change_id": change_id, "reason": f"risk:{change.get('risk')}"})
        if change.get("abi_impact", "unknown") in {"userspace-facing", "module-abi", "unknown"}:
            blockers.append({"change_id": change_id, "reason": f"abi-impact:{change.get('abi_impact', 'unknown')}"})

    policy = manifest.get("policy", {})
    if manifest.get("kernel_line") == "legacy-eol" and not policy.get("allow_eol_target", False):
        blockers.append({"change_id": "*", "reason": "legacy-eol-target-not-enabled"})
    if policy.get("protected_branch_write") is not False:
        blockers.append({"change_id": "*", "reason": "protected-branch-write-must-be-false"})
    if policy.get("human_approval_required") is not True:
        blockers.append({"change_id": "*", "reason": "human-approval-required"})

    # Stable Kahn topological sort. Any nodes left with incoming edges are cyclic.
    indegree = {key: 0 for key in by_id}
    dependents = {key: set() for key in by_id}
    for change_id, deps in dependencies.items():
        for dependency in deps:
            if dependency in by_id:
                indegree[change_id] += 1
                dependents[dependency].add(change_id)
    ready = sorted(key for key, degree in indegree.items() if degree == 0)
    order: list[str] = []
    while ready:
        current = ready.pop(0)
        order.append(current)
        for child in sorted(dependents[current]):
            indegree[child] -= 1
            if indegree[child] == 0:
                ready.append(child)
                ready.sort()
    cyclic = sorted(key for key, degree in indegree.items() if degree > 0)
    if cyclic:
        errors.append("Dependency cycle involves: " + ", ".join(cyclic))

    return {
        "planner_schema_version": "1.0.0",
        "migration_id": manifest.get("migration_id"),
        "mode": "read-only-dry-run",
        "valid": not errors,
        "dependency_order": order,
        "blocked_changes": sorted(blockers, key=lambda item: (item["change_id"], item["reason"])),
        "errors": errors,
        "execution": {
            "patches_applied": False,
            "sources_fetched": False,
            "files_modified": False,
            "builds_started": False,
            "protected_branches_written": False,
            "ready_for_isolated_apply": False,
        },
        "decision": "blocked" if errors or blockers else "manual-approval-required",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="path to a KUMBE JSON manifest")
    args = parser.parse_args()
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    if not isinstance(manifest, dict):
        parser.error("manifest root must be a JSON object")
    plan = build_plan(manifest)
    print(json.dumps(plan, indent=2, sort_keys=True))
    return 0 if plan["valid"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
