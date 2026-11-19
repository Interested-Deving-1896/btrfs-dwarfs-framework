#!/usr/bin/env bash
# Validate that vendored defaults do not hard-code a specific Linux distribution.
# Usage: bash scripts/check-vendor-agnostic.sh [vendor-dir]
set -euo pipefail

target="${1:-vendor}"
if [[ "$target" != "vendor" && "$target" != vendor/* ]]; then
  echo "ERROR: scan target must be vendor/ or a subdirectory of vendor/" >&2
  exit 2
fi
if [[ ! -d "$target" ]]; then
  echo "ERROR: scan target does not exist: $target" >&2
  exit 2
fi

repo_root="$(pwd -P)"
target_real="$(cd -- "$target" && pwd -P)"
case "$target_real/" in
  "$repo_root/vendor/"|"$repo_root/vendor/"*)
    ;;
  *)
    echo "ERROR: resolved scan target is outside vendor/: $target_real" >&2
    exit 2
    ;;
esac

python3 - "$target_real" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
extensions = {".sh", ".bash", ".yml", ".yaml", ".ts", ".tsx", ".js", ".mjs"}
distro = re.compile(
    r"\b(?:ubuntu|debian|devuan|fedora|rhel|red\s*hat|centos|rocky|almalinux|"
    r"arch(?:linux)?|manjaro|alpine|opensuse|suse|gentoo|nixos|void|kali|"
    r"linux\s*mint|mint|pop(?:!|_)?os|zorin|elementary|kde\s*neon)\b|"
    r"\b(?:archive|security)\.(?:ubuntu|debian)\.com\b|"
    r"\b(?:mirrorlist|mirror-list|mirrors\.list|archlinux\.mirrorlist|fedora\.repo)\b",
    re.IGNORECASE,
)
shell_default = re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*:-([^}]*)\}")
literal_fallback = re.compile(r"(?:\|\||\?\?)\s*(['\"])(.*?)\1")

violations = []
for path in sorted(root.rglob("*")):
    if not path.is_file() or path.suffix.lower() not in extensions:
        continue
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (UnicodeDecodeError, OSError):
        continue
    for number, line in enumerate(lines, 1):
        stripped = line.lstrip()
        if stripped.startswith(("#", "//")):
            continue
        for pattern in (shell_default, literal_fallback):
            for match in pattern.finditer(line):
                value = match.group(1) if pattern is shell_default else match.group(2)
                if distro.search(value):
                    relative = path.relative_to(root.parent.parent)
                    violations.append(
                        f"{relative}:{number}: distro-specific fallback {match.group(0)!r}"
                    )

if violations:
    print("Distro-specific defaults found in vendored source:", file=sys.stderr)
    for item in violations:
        print(f"  {item}", file=sys.stderr)
    sys.exit(1)

print(f"Vendor agnosticism check passed: {root}")
PY
