#!/usr/bin/env python3
"""Structural and negative tests for the KUMBE v1 manifest schema."""
from copy import deepcopy
import json
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = ROOT / "schemas/kernel-upstream-migration/v1/manifest.schema.json"
FIXTURE_PATH = ROOT / "tests/kernel-migration/fixtures/valid-audit-only.json"

schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))

Draft202012Validator.check_schema(schema)
validator = Draft202012Validator(schema)
errors = sorted(validator.iter_errors(fixture), key=lambda error: list(error.path))
assert not errors, "valid fixture rejected: " + "; ".join(error.message for error in errors)

# Safety invariants must be enforced by the schema, not just documented.
unsafe_cases = []
case = deepcopy(fixture)
case["policy"]["protected_branch_write"] = True
unsafe_cases.append(("protected branch writes", case))

case = deepcopy(fixture)
case["policy"]["human_approval_required"] = False
unsafe_cases.append(("mandatory human approval", case))

case = deepcopy(fixture)
case["source"]["commit"] = "moving-branch"
unsafe_cases.append(("immutable source commit", case))

case = deepcopy(fixture)
case["validation"][0]["state"] = "passed"
case["validation"][0]["evidence_uri"] = None
unsafe_cases.append(("passed validation without evidence URI", case))

for label, candidate in unsafe_cases:
    if validator.is_valid(candidate):
        raise AssertionError(f"unsafe manifest unexpectedly accepted: {label}")

print("KUMBE schema checks passed (1 valid fixture, 3 rejected unsafe fixtures).")
