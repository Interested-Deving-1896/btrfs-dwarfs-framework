#!/usr/bin/env python3
"""Validate the Calamares ↔ BDFS integration declaration without side effects."""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
METADATA = ROOT / "integration.json"


def validate() -> list[str]:
    errors: list[str] = []
    try:
        data = json.loads(METADATA.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"cannot read integration metadata: {exc}"]

    if data.get("schemaVersion") != 1:
        errors.append("schemaVersion must be 1")
    if data.get("id") != "calamares-bdfs":
        errors.append("unexpected integration id")
    if data.get("status") != "contract-only":
        errors.append("initial integration status must remain contract-only")

    upstream = data.get("upstream", {})
    if upstream.get("historicalRepositoryArchived") is not True:
        errors.append("historical GitHub upstream must be marked archived")
    if upstream.get("maintainedRepository") != "https://codeberg.org/Calamares/calamares":
        errors.append("maintained Calamares repository must use the Codeberg URL")
    if upstream.get("sourceRevision") is not None:
        errors.append("source revision must remain unset until independently reviewed")

    directions = data.get("directions", {})
    for key in ("calamaresConsumesBdfs", "bdfsInvokesCalamares"):
        if directions.get(key, {}).get("status") != "planned":
            errors.append(f"{key} must remain planned until implementation is tested")

    capabilities = data.get("capabilities", {})
    for key in ("mount", "partition", "format", "filesystemConversion",
                "bootloaderMutation", "install", "hostMutation",
                "privilegedExecution", "importedHookExecution"):
        if capabilities.get(key) is not False:
            errors.append(f"unsafe capability {key} must remain false")
    if capabilities.get("inventory") is not True:
        errors.append("read-only inventory capability must be declared")
    if capabilities.get("manifestValidation") is not True:
        errors.append("manifest validation capability must be declared")

    safety = data.get("safety", {})
    for key in ("destructiveOperationsRequireExplicitPlan",
                "destructiveOperationsRequireUserConfirmation",
                "testsRequireDisposableVirtualDisks"):
        if safety.get(key) is not True:
            errors.append(f"safety invariant {key} must be true")

    schema_path = (ROOT / data.get("artifactContract", {}).get("schema", "")).resolve()
    repo_root = ROOT.parents[1].resolve()
    if repo_root not in schema_path.parents or not schema_path.is_file():
        errors.append("artifact manifest schema path must resolve inside the repository")

    return errors


if __name__ == "__main__":
    problems = validate()
    if problems:
        for problem in problems:
            print(f"ERROR: {problem}")
        raise SystemExit(1)
    print("Calamares ↔ BDFS integration contract: valid (contract-only; no installer operations enabled)")
