#!/usr/bin/env python3
"""Validate BDFS target profiles using only the Python standard library."""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent
PROFILE_DIR = ROOT / "targets"
REQUIRED = {
    "schema_version", "target_id", "display_name", "os_family", "kernel_family",
    "kernel_abi", "architectures", "object_format", "toolchain", "sdk_sysroot",
    "runtime", "boot_chain", "filesystem_capabilities", "build_backend",
    "source_revision", "patchset_digest", "provenance", "compatibility_tier",
    "integration_status",
}
ENUMS = {
    "os_family": {"linux", "freebsd", "darwin"},
    "kernel_family": {"linux", "freebsd", "xnu"},
    "object_format": {"ELF", "Mach-O"},
    "architectures": {"x86_64", "aarch64", "armv7", "riscv64"},
    "compatibility_tier": {"native", "host-side-artifact-tooling", "read-only-ingestion", "experimental-port", "unsupported"},
    "integration_status": {"baseline", "profile-only", "experimental", "implemented"},
}

def fail(path, message):
    print(f"FAIL {path}: {message}", file=sys.stderr)
    return False

def validate(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return fail(path.name, f"cannot read valid JSON ({exc})")
    ok = True
    if not isinstance(data, dict):
        return fail(path.name, "profile root must be an object")
    missing = REQUIRED - data.keys()
    extra = data.keys() - REQUIRED
    if missing:
        ok &= fail(path.name, f"missing keys: {', '.join(sorted(missing))}")
    if extra:
        ok &= fail(path.name, f"unknown keys: {', '.join(sorted(extra))}")
    if data.get("schema_version") != 1:
        ok &= fail(path.name, "schema_version must be 1")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", str(data.get("target_id", ""))):
        ok &= fail(path.name, "target_id must be lowercase kebab-case")
    for key, allowed in ENUMS.items():
        value = data.get(key)
        values = value if key == "architectures" else [value]
        if key == "architectures" and (not isinstance(value, list) or not value):
            ok &= fail(path.name, "architectures must be a non-empty array")
        elif any(v not in allowed for v in values):
            ok &= fail(path.name, f"{key} contains an unsupported value")
    for key in ("display_name", "kernel_abi", "sdk_sysroot", "runtime", "boot_chain", "source_revision", "patchset_digest"):
        if not isinstance(data.get(key), str) or not data[key].strip():
            ok &= fail(path.name, f"{key} must be a non-empty string")
    for key in ("toolchain", "build_backend"):
        if not isinstance(data.get(key), list) or not data[key] or not all(isinstance(x, str) and x for x in data[key]):
            ok &= fail(path.name, f"{key} must be a non-empty string array")
    caps = data.get("filesystem_capabilities")
    if not isinstance(caps, dict) or not all(isinstance(k, str) and isinstance(v, bool) for k, v in caps.items()):
        ok &= fail(path.name, "filesystem_capabilities must map names to booleans")
    provenance = data.get("provenance")
    if not isinstance(provenance, dict) or set(provenance) != {"status", "notes"} or provenance.get("status") not in {"unresolved", "declared", "verified"} or not isinstance(provenance.get("notes"), str):
        ok &= fail(path.name, "provenance must contain status and notes")
    os_family, kernel = data.get("os_family"), data.get("kernel_family")
    if os_family == "linux" and kernel != "linux":
        ok &= fail(path.name, "Linux OS profiles must use the Linux kernel family")
    if os_family == "freebsd" and kernel != "freebsd":
        ok &= fail(path.name, "FreeBSD OS profiles must use the FreeBSD kernel family")
    if os_family == "darwin" and kernel != "xnu":
        ok &= fail(path.name, "Darwin OS profiles must use the XNU kernel family")
    if data.get("integration_status") == "profile-only" and data.get("compatibility_tier") == "native":
        ok &= fail(path.name, "profile-only targets cannot claim native compatibility")
    if data.get("integration_status") in {"profile-only", "experimental"} and provenance and provenance.get("status") == "verified":
        ok &= fail(path.name, "unimplemented target cannot claim verified implementation provenance")
    if ok:
        print(f"PASS {path.name}: {data['target_id']} ({kernel})")
    return bool(ok)

def main():
    profiles = sorted(PROFILE_DIR.glob("*.json"))
    if not profiles:
        print("No target profiles found", file=sys.stderr)
        return 1
    results = [validate(path) for path in profiles]
    print(f"Validated {len(profiles)} target profile(s)")
    return 0 if all(results) else 1

if __name__ == "__main__":
    raise SystemExit(main())
