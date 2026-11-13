#!/usr/bin/env bash
#
# Syncs all KDE group mirror repos under
# openos-project/kde-ecosystem-deving/kde-groups from invent.kde.org.
#
# For each GitLab project in the kde-groups hierarchy, derives the upstream
# KDE Invent URL from the project path and pushes branches + tags.
# Skips refs/merge-requests/* which GitLab rejects as hidden refs.
#
# Required CI variable: GITLAB_TOKEN — PAT with api + write_repository scope
#
# This script is designed to be resumable: it processes all projects in
# alphabetical order and continues on individual failures.

set -uo pipefail

GITLAB_API="https://gitlab.com/api/v4"
KDE_BASE="https://invent.kde.org"
KDE_GROUPS_GL_ID="130743027"  # openos-project/kde-ecosystem-deving/kde-groups

: "${GITLAB_TOKEN:?GITLAB_TOKEN is required}"

info()  { echo "[kde-groups] $*"; }
warn()  { echo "[kde-groups][warn] $*" >&2; }

SYNCED=0
FAILED=0
SKIPPED=0

info "Fetching all projects under kde-groups (id=${KDE_GROUPS_GL_ID})..."

# Collect all projects across all pages.
projects_json=$(python3 - <<PYEOF
import json
import urllib.request

TOKEN = "${GITLAB_TOKEN}"
API = "${GITLAB_API}"
GROUP_ID = "${KDE_GROUPS_GL_ID}"

projects = []
page = 1
while True:
    req = urllib.request.Request(
        f"{API}/groups/{GROUP_ID}/projects?include_subgroups=true&per_page=100&page={page}&archived=false",
        headers={"PRIVATE-TOKEN": TOKEN},
    )
    with urllib.request.urlopen(req, timeout=20) as response:
        batch = json.load(response)
    if not batch:
        break
    projects.extend(batch)
    page += 1

for project in sorted(projects, key=lambda item: item["path_with_namespace"]):
    full_path = project["path_with_namespace"]
    prefix = "openos-project/kde-ecosystem-deving/kde-groups/"
    kde_path = full_path[len(prefix):] if full_path.startswith(prefix) else project["path"]
    print(f"{project['http_url_to_repo']}\t{kde_path}")
PYEOF
)

total=$(printf '%s\n' "$projects_json" | awk 'END { print NR }')
info "Found ${total} projects to sync"

while IFS=$'\t' read -r gl_url kde_path; do
    [ -n "$gl_url" ] || continue

    kde_url="${KDE_BASE}/${kde_path}.git"
    gl_auth_url="${gl_url/https:\/\//https://oauth2:${GITLAB_TOKEN}@}"

    info "Syncing ${kde_path} ..."
    work_dir=$(mktemp -d) || {
        warn "  ❌ ${kde_path} — could not create temporary directory"
        FAILED=$((FAILED + 1))
        continue
    }

    if git clone --mirror "${kde_url}" "${work_dir}" 2>/dev/null; then
        # Push only branches and tags — skip merge-request refs.
        if git -C "${work_dir}" push "${gl_auth_url}" \
            "+refs/heads/*:refs/heads/*" \
            "+refs/tags/*:refs/tags/*" 2>/dev/null; then
            info "  ✅ ${kde_path}"
            SYNCED=$((SYNCED + 1))
        else
            warn "  ❌ ${kde_path} — push failed"
            FAILED=$((FAILED + 1))
        fi
    else
        warn "  ❌ ${kde_path} — clone from invent.kde.org failed (repo may be empty or moved)"
        FAILED=$((FAILED + 1))
    fi

    rm -rf "${work_dir}"
done <<< "$projects_json"

echo ""
info "Done — synced=${SYNCED} | failed=${FAILED} | skipped=${SKIPPED}"
[ "${FAILED}" -eq 0 ] || exit 1
