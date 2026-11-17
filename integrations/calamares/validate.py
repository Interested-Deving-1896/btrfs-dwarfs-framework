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

    source_import = data.get("sourceImport", {})
    if source_import.get("mechanism") != "Fork-Sync-All scripts/import-repo.sh via .github/workflows/import-repo.yml":
        errors.append("Codeberg source must use the existing Fork-Sync-All importer")
    if source_import.get("sourceUrl") != "https://codeberg.org/Calamares/calamares":
        errors.append("unexpected Codeberg source URL")
    if source_import.get("targetRepository") != "Interested-Deving-1896/calamares-upstream":
        errors.append("upstream import must use a separate repository name")
    if source_import.get("targetRepositoryStatus") not in {"created", "not-yet-created"}:
        errors.append("import destination status must be explicit")
    if source_import.get("targetRepositoryStatus") == "created":
        if source_import.get("workflowRun") != 42430496420:
            errors.append("created destination must reference the observed import workflow run")
        if source_import.get("importExecutionStatus") != "partial-needs-ref-and-default-branch-audit":
            errors.append("created import must remain marked partial pending audit")
        if source_import.get("sourceDefaultBranch") != "calamares":
            errors.append("Calamares source default branch must be recorded")
        if source_import.get("observedTargetDefaultBranch") == source_import.get("sourceDefaultBranch"):
            errors.append("default branch mismatch must not be hidden")
        if source_import.get("defaultBranchCorrectionRequired") is not True:
            errors.append("default branch correction must remain explicit")
        if source_import.get("hiddenPullRequestRefsRejected") is not True:
            errors.append("rejected GitHub-managed refs must be recorded")
    if source_import.get("existingNameCollision", {}).get("overwriteAllowed") is not False:
        errors.append("existing Calamares settings fork must never be overwritten")
    workflow_inputs = source_import.get("initialWorkflowInputs", {})
    if workflow_inputs.get("repoName") != "calamares-upstream":
        errors.append("import workflow must use the isolated upstream destination")
    if workflow_inputs.get("mirrorToOspOoc") is not False or workflow_inputs.get("ongoingSync") is not False:
        errors.append("initial import must not enable mirrors or scheduled synchronization")
    if source_import.get("targetRepositoryStatus") == "not-yet-created" and source_import.get("importExecutionStatus") != "not-dispatched":
        errors.append("an uncreated destination must not claim a dispatched import")
    if source_import.get("mirrorAndScheduledSync") not in {"disabled", "deferred-pending-source-audit"}:
        errors.append("mirroring and scheduled synchronization must remain disabled")

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
