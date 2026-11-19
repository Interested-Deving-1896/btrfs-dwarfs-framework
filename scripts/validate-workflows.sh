#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Parse every GitHub Actions workflow; semantically lint workflows changed here.
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

# actionlint checks GitHub-specific schema, expressions, contexts and action
# inputs. Keep the semantic gate scoped to changed workflow files so legacy
# findings in unrelated workflows do not block this PR.
if [ -n "${GITHUB_BASE_REF:-}" ]; then
    git fetch --no-tags origin "+refs/heads/${GITHUB_BASE_REF}:refs/remotes/origin/${GITHUB_BASE_REF}"
    changed=$(git diff --name-only "origin/${GITHUB_BASE_REF}...HEAD" -- .github/workflows)
else
    changed=$(git diff --name-only HEAD^ HEAD -- .github/workflows)
fi

if [ -z "$changed" ]; then
    echo "No workflow files changed; semantic actionlint check skipped."
    exit 0
fi

tmp_bin=$(mktemp -d)
trap 'rm -rf "$tmp_bin"' EXIT HUP INT TERM
if command -v actionlint >/dev/null 2>&1; then
    actionlint_bin=$(command -v actionlint)
elif command -v go >/dev/null 2>&1; then
    GOBIN="$tmp_bin" go install github.com/rhysd/actionlint/cmd/actionlint@v1.7.7
    actionlint_bin="$tmp_bin/actionlint"
else
    echo "actionlint or Go is required for workflow semantic validation" >&2
    exit 1
fi

printf '%s\n' "$changed" |
while IFS= read -r workflow; do
    [ -n "$workflow" ] || continue
    "$actionlint_bin" "$ROOT_DIR/$workflow"
done
