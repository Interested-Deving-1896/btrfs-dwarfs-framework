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

# YAML parsing above catches syntax errors; actionlint additionally validates
# GitHub Actions workflow structure, expressions, contexts and action inputs.
cd "$ROOT_DIR"
if command -v actionlint >/dev/null 2>&1; then
    actionlint
elif command -v go >/dev/null 2>&1; then
    go run github.com/rhysd/actionlint/cmd/actionlint@v1.7.7
else
    echo "actionlint or Go is required for workflow semantic validation" >&2
    exit 1
fi
