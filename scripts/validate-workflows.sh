#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Parse every tracked GitHub Actions workflow without executing it.
set -eu

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WORKFLOW_DIR="$ROOT_DIR/.github/workflows"

if ! command -v ruby >/dev/null 2>&1; then
    echo "ruby is required to validate workflow YAML syntax" >&2
    exit 1
fi

find "$WORKFLOW_DIR" -type f \( -name '*.yml' -o -name '*.yaml' \) -print |
while IFS= read -r workflow; do
    ruby -e 'require "yaml"; YAML.parse_file(ARGV.fetch(0))' "$workflow" || {
        echo "Invalid workflow YAML: $workflow" >&2
        exit 1
    }
    echo "PASS: ${workflow#"$ROOT_DIR"/}"
done
