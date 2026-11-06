#!/usr/bin/env python3
"""Read-only KUMBE manifest dry-run planner.

This tool evaluates manifest structure, dependency ordering, immutable source
identity, and explicit approval gates. It never fetches sources, applies patches,
writes files, builds kernels, or changes repository state.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

FULL_COMMIT = re.compile(r"^[0-9a-fA-F]{40,64}$")
HIGH_RISK = {"high", "critical", "unknown"}
MANUAL_CLASSES = {"semantic-manual-port", "context-adjusted-backport"}


def plan(manifest: dict) -> dict:
    blockers: list[str] = []
    approvals: list[str] = []
    warnings: list[str] = []

    if not isinstance(manifest, dict):
        return {"planner_schema_version": "1.0.0", "decision": "blocked",
                "blockers": ["Manifest root must be an object."],
                "approval_gates": [], "ordered_changes": [], "read_only": True}

    source = manifest.get("source")
    if not isinstance(source, dict):
        source = {}
        blockers.append("Source must be an object.")
    commit = source.get("commit")
    if not isinstance(commit, str) or not FULL_COMMIT.fullmatch(commit):
        blockers.append("Source identity is not pinned to a full 40-64 character commit.")

    target = manifest.get("target")
    if not isinstance(target, dict):
        target = {}
        blockers.append("Target must be an object.")
    if not isinstance(target.get("architectures"), list) or not target.get("architectures"):
        blockers.append("Target architectures are missing or invalid.")

    policy = manifest.get("policy")
    if not isinstance(policy, dict):
        policy = {}
        blockers.append("Policy must be an object.")
    if policy.get("human_approval_required") is not True:
        blockers.append("Policy must require human approval.")
    if policy.get("protected_branch_write") is not False:
        blockers.append("Protected-branch writes must remain disabled.")
    if policy.get("dry_run") is not True:
        warnings.append("Manifest does not request dry-run mode; planner still performs no writes.")

    if manifest.get("kernel_line") == "legacy-eol" and policy.get("allow_eol_target") is not True:
        blockers.append("Legacy/EOL target is not explicitly allowed by policy.")
    elif manifest.get("kernel_line") == "legacy-eol":
        approvals.append("Explicit human approval for best-effort legacy/EOL target.")

    changes = manifest.get("changes")
    if not isinstance(changes, list) or not changes:
        blockers.append("At least one change is required.")
        changes = []

    by_id = {}
    for item in changes:
        if not isinstance(item, dict) or not isinstance(item.get("change_id"), str) or not item["change_id"]:
            blockers.append("Every change must have a non-empty change_id.")
            continue
        change_id = item["change_id"]
        if change_id in by_id:
            blockers.append(f"Duplicate change_id: {change_id}")
        by_id[change_id] = item

    for change_id, item in by_id.items():
        classification = item.get("classification")
        if not isinstance(classification, str):
            blockers.append(f"{change_id} classification must be a string.")
            classification = None
        if classification == "blocked":
            blockers.append(f"{change_id} is explicitly classified blocked.")
        elif classification == "rejected":
            blockers.append(f"{change_id} is explicitly rejected.")
        elif classification == "not-applicable":
            warnings.append(f"{change_id} is marked not-applicable and will be excluded.")
        elif classification not in {
            "clean-forward-port", "clean-backport", "context-adjusted-backport",
            "semantic-manual-port"
        }:
            blockers.append(f"{change_id} has no actionable, recognized classification.")

        risk = item.get("risk", "unknown")
        if not isinstance(risk, str):
            warnings.append(f"{change_id} risk is malformed; treating it as unknown.")
            risk = "unknown"
        if risk in HIGH_RISK:
            approvals.append(f"Human review required for {change_id}: {risk} risk.")
        if classification in MANUAL_CLASSES:
            approvals.append(f"Human approval required for {change_id}: {classification}.")
        if item.get("abi_impact", "unknown") != "none-known":
            approvals.append(f"ABI/config review required for {change_id}: {item.get('abi_impact', 'unknown')}.")

        deps = item.get("dependencies", [])
        if not isinstance(deps, list):
            blockers.append(f"{change_id} dependencies must be an array.")
            continue
        for dep in deps:
            if not isinstance(dep, str) or not dep:
                blockers.append(f"{change_id} dependencies must contain non-empty string IDs.")
            elif dep not in by_id:
                blockers.append(f"{change_id} depends on missing change {dep}.")

    # Stable topological ordering; input order breaks ties deterministically.
    indegree = {key: 0 for key in by_id}
    children = {key: [] for key in by_id}
    for key, item in by_id.items():
        for dep in item.get("dependencies", []) if isinstance(item.get("dependencies", []), list) else []:
            if isinstance(dep, str) and dep in by_id:
                indegree[key] += 1
                children[dep].append(key)
    ready = [key for key in by_id if indegree[key] == 0]
    ordered = []
    while ready:
        current = ready.pop(0)
        if by_id[current].get("classification") != "not-applicable":
            ordered.append(current)
        for child in children[current]:
            indegree[child] -= 1
            if indegree[child] == 0:
                ready.append(child)
    if len(ordered) + sum(1 for item in by_id.values() if item.get("classification") == "not-applicable") != len(by_id):
        blockers.append("Dependency cycle detected; no safe ordering exists.")

    blockers = list(dict.fromkeys(blockers))
    approvals = list(dict.fromkeys(approvals))
    warnings = list(dict.fromkeys(warnings))
    decision = "blocked" if blockers else ("approval-required" if approvals else "dry-run-ready")
    return {
        "planner_schema_version": "1.0.0",
        "migration_id": manifest.get("migration_id"),
        "provider": manifest.get("provider"),
        "decision": decision,
        "read_only": True,
        "patch_application_performed": False,
        "kernel_build_performed": False,
        "blockers": blockers,
        "approval_gates": approvals,
        "warnings": warnings,
        "ordered_changes": ordered,
        "limitations": [
            "Ordering and metadata checks do not prove patch applicability or semantic correctness.",
            "No upstream sources or patch contents are fetched or inspected.",
            "No patch application, kernel build, module build, boot test, or runtime test is performed."
        ]
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="path to a KUMBE JSON manifest")
    args = parser.parse_args()
    try:
        data = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(json.dumps({"decision": "blocked", "read_only": True,
                          "blockers": [f"Unable to read valid JSON manifest: {exc}"]}, indent=2))
        return 2
    result = plan(data)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 2 if result["decision"] == "blocked" else 0


if __name__ == "__main__":
    raise SystemExit(main())
