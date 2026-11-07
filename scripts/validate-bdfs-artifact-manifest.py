#!/usr/bin/env python3
"""Validate a BDFS artifact manifest without executing or importing its payload."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

try:
    from jsonschema import Draft202012Validator, FormatChecker
except ImportError as exc:  # pragma: no cover - exercised by CLI users
    raise SystemExit(
        "Missing dependency: install jsonschema (python3 -m pip install jsonschema)"
    ) from exc

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SCHEMA = ROOT / "doc" / "schemas" / "bdfs-artifact-manifest.schema.json"


def load_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"{path}: {exc}") from exc


def validate_manifest(
    manifest: Any, schema: Any, *, for_import: bool = False, payload_path: Path | None = None
) -> list[str]:
    validator = Draft202012Validator(schema, format_checker=FormatChecker())
    errors = sorted(validator.iter_errors(manifest), key=lambda error: list(error.absolute_path))
    messages = [
        f"{'.'.join(map(str, error.absolute_path)) or '<root>'}: {error.message}"
        for error in errors
    ]
    if messages:
        return messages

    security = manifest.get("security", {})
    capabilities = manifest.get("capabilities", [])
    if security.get("hooksPresent") and security.get("hooksDefaultState") not in {"inert", "absent"}:
        messages.append("security.hooksDefaultState: hooks must be inert or absent")
    if for_import and "run-hooks" in capabilities:
        messages.append("capabilities: run-hooks is forbidden during import validation")
    if for_import and security.get("hooksPresent") and security.get("hooksDefaultState") != "inert":
        messages.append("security: imported hooks must be explicitly inert")

    if payload_path is not None:
        digest = manifest["payload"].get("digest")
        if not digest:
            messages.append("payload.digest: required when --payload is supplied")
        else:
            algorithm, expected = digest.split(":", 1)
            if payload_path.is_symlink() or not payload_path.is_file():
                messages.append("payload: must be a regular, non-symlink file")
            else:
                hasher = hashlib.new(algorithm)
                try:
                    with payload_path.open("rb") as stream:
                        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                            hasher.update(chunk)
                except OSError as exc:
                    messages.append(f"payload: cannot read file: {exc}")
                else:
                    if hasher.hexdigest().lower() != expected.lower():
                        messages.append("payload.digest: checksum mismatch")
    return messages


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="manifest JSON file")
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA)
    parser.add_argument("--payload", type=Path, help="optional local payload file to checksum")
    parser.add_argument(
        "--for-import",
        action="store_true",
        help="apply additional inert-hook and no-run-hooks import policy",
    )
    args = parser.parse_args()

    try:
        manifest = load_json(args.manifest)
        schema = load_json(args.schema)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    messages = validate_manifest(
        manifest, schema, for_import=args.for_import, payload_path=args.payload
    )
    if messages:
        for message in messages:
            print(f"ERROR: {message}", file=sys.stderr)
        return 1
    print(f"Valid BDFS artifact manifest: {args.manifest}")
    if args.payload:
        print(f"Payload checksum verified: {args.payload}")
    print("No payload was extracted, executed, imported, or deployed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
