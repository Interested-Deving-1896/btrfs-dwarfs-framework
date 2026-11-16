#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Rebuild an LTS branch from FEATURE_BRANCH rebased onto the upstream
# BASE_BRANCH. This script is read-only unless DRY_RUN=false and both
# repository-level policy gates are explicitly enabled.
#
# Required: GH_TOKEN, TARGET_REPO
# Optional: BASE_BRANCH (master), FEATURE_BRANCH (all-features),
#           LTS_BRANCH (lts), DRY_RUN (true)

set -euo pipefail

: "${GH_TOKEN:?GH_TOKEN is required}"
: "${TARGET_REPO:?TARGET_REPO is required}"

BASE_BRANCH="${BASE_BRANCH:-master}"
FEATURE_BRANCH="${FEATURE_BRANCH:-all-features}"
LTS_BRANCH="${LTS_BRANCH:-lts}"
DRY_RUN="${DRY_RUN:-true}"

if [[ ! "$TARGET_REPO" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
  echo "ERROR: TARGET_REPO must be owner/repository." >&2
  exit 2
fi

for branch in "$BASE_BRANCH" "$FEATURE_BRANCH" "$LTS_BRANCH"; do
  if ! git check-ref-format --branch "$branch" >/dev/null 2>&1; then
    echo "ERROR: Invalid branch name: $branch" >&2
    exit 2
  fi
done

if [[ "$DRY_RUN" != "true" && "$DRY_RUN" != "false" ]]; then
  echo "ERROR: DRY_RUN must be exactly 'true' or 'false'." >&2
  exit 2
fi

if [[ "$DRY_RUN" == "false" ]]; then
  if [[ "${BDFS_SOURCE_AUDIT_COMPLETE:-}" != "true" ||
        "${BDFS_LTS_REBASE_ENABLED:-}" != "true" ]]; then
    echo "ERROR: LTS writes require BDFS_SOURCE_AUDIT_COMPLETE=true and BDFS_LTS_REBASE_ENABLED=true." >&2
    exit 2
  fi
fi

API="https://api.github.com"
REPO_URL="https://x-access-token:${GH_TOKEN}@github.com/${TARGET_REPO}.git"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

echo "── Inspecting target repository ──"
repo_json=$(curl -fsS \
  -H "Authorization: Bearer ${GH_TOKEN}" \
  -H "Accept: application/vnd.github+json" \
  -H "X-GitHub-Api-Version: 2022-11-28" \
  "${API}/repos/${TARGET_REPO}")
upstream_repo=$(printf '%s' "$repo_json" | jq -r '.parent.full_name // empty')

echo "  Target: $TARGET_REPO"
echo "  Upstream: ${upstream_repo:-not a fork; using target base branch}"

echo "── Fetching branches into an isolated workspace ──"
git clone --no-checkout --no-tags "$REPO_URL" "$WORK_DIR/repo"
cd "$WORK_DIR/repo"
git config user.name "BDFS LTS Rebuild"
git config user.email "bdfs-lts-rebuild@users.noreply.github.com"

git fetch --no-tags origin \
  "+refs/heads/${BASE_BRANCH}:refs/remotes/origin/${BASE_BRANCH}" \
  "+refs/heads/${FEATURE_BRANCH}:refs/remotes/origin/${FEATURE_BRANCH}"

feature_expected=$(git ls-remote --heads origin "refs/heads/${FEATURE_BRANCH}" | awk 'NR == 1 {print $1}')
if [[ -z "$feature_expected" ]]; then
  echo "ERROR: Remote feature branch '$FEATURE_BRANCH' does not exist." >&2
  exit 1
fi

lts_expected=$(git ls-remote --heads origin "refs/heads/${LTS_BRANCH}" | awk 'NR == 1 {print $1}')
if [[ -n "$lts_expected" ]]; then
  git fetch --no-tags origin \
    "+refs/heads/${LTS_BRANCH}:refs/remotes/origin/${LTS_BRANCH}"
fi

base_ref="refs/remotes/origin/${BASE_BRANCH}"
if [[ -n "$upstream_repo" ]]; then
  echo "── Fetching upstream base without mutating the target ──"
  git remote add upstream "https://github.com/${upstream_repo}.git"
  git fetch --no-tags upstream \
    "+refs/heads/${BASE_BRANCH}:refs/remotes/upstream/${BASE_BRANCH}"
  base_ref="refs/remotes/upstream/${BASE_BRANCH}"
fi

base_tip=$(git rev-parse "$base_ref")
git checkout -B "$FEATURE_BRANCH" "refs/remotes/origin/${FEATURE_BRANCH}"

echo "── Rebasing without automatic conflict resolution ──"
echo "  Base: $base_tip"
echo "  Feature before rebase: $feature_expected"

if ! git -c core.editor=true rebase "$base_tip"; then
  git rebase --abort || true
  echo "ERROR: Rebase conflicted or failed. No remote refs were changed." >&2
  exit 1
fi

result_sha=$(git rev-parse HEAD)
echo "  Proposed result: $result_sha"

if [[ "$DRY_RUN" == "true" ]]; then
  echo "DRY RUN: no remote refs were changed."
  exit 0
fi

echo "── Atomically updating feature and LTS refs with leases ──"
feature_lease="--force-with-lease=refs/heads/${FEATURE_BRANCH}:${feature_expected}"
lts_lease="--force-with-lease=refs/heads/${LTS_BRANCH}:${lts_expected}"

# A single atomic push prevents a partial feature/LTS update. There is no
# unconditional force-push fallback; any stale lease or server rejection aborts.
git push --atomic "$feature_lease" "$lts_lease" origin \
  "HEAD:refs/heads/${FEATURE_BRANCH}" \
  "HEAD:refs/heads/${LTS_BRANCH}"

echo "LTS rebuild completed."
echo "  Repository: $TARGET_REPO"
echo "  Base: $base_tip"
echo "  Feature and LTS: $result_sha"
