#!/usr/bin/env bash
# bdfs-incus-runtime — Incus as the default bdfs runtime
#
# Top-level integration that establishes Incus as btrfs-dwarfs-framework's
# default container/VM runtime, replacing Docker and Podman as host-level
# dependencies.
#
# Architecture:
#   btrfs-dwarfs-framework (filesystem layer)
#     └── Incus (runtime layer)
#           ├── System containers  (lightweight, shared kernel)
#           ├── VMs                (KVM/QEMU, full isolation)
#           ├── OCI containers     (incus launch oci:<image>)
#           └── Nested instances   (Docker/Podman inside Incus)
#
# Docker and Podman are not removed — they become optional workloads that
# run *inside* Incus instances when a specific workflow requires them.
# The host never needs Docker or Podman installed.
#
# Subcommands:
#   init            Initialise Incus as the bdfs runtime (one-time setup)
#   launch          Launch a container or VM from any image source
#   run             Run an ephemeral command in a container (like docker run --rm)
#   workspace       Create a bdfs workspace from a running Incus instance
#   publish         Publish a bdfs workspace as an Incus image
#   nested-docker   Launch an Incus container with Docker installed + bdfs workspace mounted
#   nested-podman   Launch an Incus container with Podman installed + bdfs workspace mounted
#   profile         Manage bdfs Incus profiles (apply, list, show)
#   status          Show all bdfs-managed Incus instances and images
#
# Environment:
#   INCUS_CMD              Incus CLI binary (default: incus)
#   BDFS_INCUS_POOL        Storage pool (default: default)
#   BDFS_INCUS_PROFILE     Default profile for bdfs instances (default: bdfs)
#   BDFS_INCUS_OCI_REMOTE  OCI remote name for Docker Hub (default: oci-docker)
#
# Dependencies: incus, bdfs
#
# Usage:
#   bdfs-incus-runtime.sh init
#   bdfs-incus-runtime.sh launch          IMAGE [NAME] [--vm] [--config K=V] [--device D]
#   bdfs-incus-runtime.sh run             IMAGE [--vm] [--] CMD [ARGS...]
#   bdfs-incus-runtime.sh workspace       --instance NAME [--name WS_NAME]
#   bdfs-incus-runtime.sh publish         --workspace NAME [--alias ALIAS] [--push OCI_REF]
#   bdfs-incus-runtime.sh nested-docker   --workspace NAME [--instance NAME] [--start]
#   bdfs-incus-runtime.sh nested-podman   --workspace NAME [--instance NAME] [--start]
#   bdfs-incus-runtime.sh profile         apply|list|show [ARGS]
#   bdfs-incus-runtime.sh status

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/bdfs-incus.sh"

BDFS_CMD="${BDFS_CMD:-bdfs}"
BDFS_INCUS_PROFILE="${BDFS_INCUS_PROFILE:-bdfs}"

info() { echo "[bdfs-incus-runtime] $*"; }
warn() { echo "[bdfs-incus-runtime] WARN: $*" >&2; }
die()  { echo "[bdfs-incus-runtime] ERROR: $*" >&2; exit 1; }

# ── init ──────────────────────────────────────────────────────────────────────
# One-time setup: initialise Incus (if needed), create the bdfs profile,
# register the Docker Hub OCI remote.

cmd_init() {
    _bdfs_incus_require

    info "Initialising Incus as bdfs runtime..."

    # Register Docker Hub OCI remote (idempotent)
    _bdfs_incus_ensure_oci_remote
    info "  OCI remote '${BDFS_INCUS_OCI_REMOTE}' registered"

    # Create the bdfs Incus profile if it doesn't exist
    if ! "$INCUS_CMD" profile list --format csv 2>/dev/null | grep -q "^${BDFS_INCUS_PROFILE},"; then
        "$INCUS_CMD" profile create "$BDFS_INCUS_PROFILE" \
            || die "Failed to create Incus profile: ${BDFS_INCUS_PROFILE}"
        # Apply sensible defaults to the bdfs profile
        "$INCUS_CMD" profile set "$BDFS_INCUS_PROFILE" \
            limits.cpu=2 limits.memory=2GiB \
            || true
        info "  Profile '${BDFS_INCUS_PROFILE}' created"
    else
        info "  Profile '${BDFS_INCUS_PROFILE}' already exists"
    fi

    info "Incus runtime ready."
    info "  Pool:       ${BDFS_INCUS_POOL}"
    info "  Profile:    ${BDFS_INCUS_PROFILE}"
    info "  OCI remote: ${BDFS_INCUS_OCI_REMOTE}"
}

# ── launch ────────────────────────────────────────────────────────────────────

cmd_launch() {
    _bdfs_incus_require
    local image="${1:-}"
    [[ -n "$image" ]] || die "Usage: bdfs-incus-runtime.sh launch IMAGE [NAME] [OPTIONS]"
    shift

    local name=""
    local -a extra_args=()

    # First non-option arg after image is the instance name
    if [[ $# -gt 0 && "${1:0:1}" != "-" ]]; then
        name="$1"; shift
    fi
    [[ -z "$name" ]] && name="bdfs-$(date +%s)"

    extra_args+=("$@")
    _bdfs_incus_launch "$image" "$name" "${extra_args[@]}"
    info "Launched: ${name}"
}

# ── run ───────────────────────────────────────────────────────────────────────
# Ephemeral one-shot command execution — equivalent to `docker run --rm`.

cmd_run() {
    _bdfs_incus_require
    local image="${1:-}"
    [[ -n "$image" ]] || die "Usage: bdfs-incus-runtime.sh run IMAGE [--vm] [--] CMD [ARGS...]"
    shift

    local vm=false
    local -a cmd_args=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --vm) vm=true; shift ;;
            --)   shift; cmd_args+=("$@"); break ;;
            *)    cmd_args+=("$1"); shift ;;
        esac
    done

    local name="bdfs-run-$$"
    local -a launch_args=(--ephemeral)
    $vm && launch_args+=(--vm)

    _bdfs_incus_launch "$image" "$name" "${launch_args[@]}"
    _bdfs_incus_wait_ready "$name" 60

    local rc=0
    if [[ ${#cmd_args[@]} -gt 0 ]]; then
        _bdfs_incus_exec "$name" -- "${cmd_args[@]}" || rc=$?
    else
        _bdfs_incus_exec "$name" -- /bin/sh || rc=$?
    fi

    # Ephemeral instance deletes itself on stop
    _bdfs_incus_stop "$name"
    return $rc
}

# ── workspace ─────────────────────────────────────────────────────────────────

cmd_workspace() {
    _bdfs_incus_require
    command -v bdfs &>/dev/null || die "bdfs not found"
    local instance="" ws_name=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --instance) instance="$2"; shift 2 ;;
            --name)     ws_name="$2";  shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    [[ -n "$instance" ]] || die "--instance required"
    [[ -z "$ws_name"  ]] && ws_name="${instance}-ws-$(date +%Y%m%d%H%M%S)"

    local rootfs
    rootfs="$(_bdfs_incus_rootfs_path "$instance")"
    info "Creating bdfs workspace '${ws_name}' from instance '${instance}'"
    "$BDFS_CMD" workspace create --name "$ws_name" --source "$rootfs"
    info "Workspace: ${ws_name}"
}

# ── publish ───────────────────────────────────────────────────────────────────

cmd_publish() {
    _bdfs_incus_require
    command -v bdfs &>/dev/null || die "bdfs not found"
    local workspace="" alias="" push_ref=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace) workspace="$2"; shift 2 ;;
            --alias)     alias="$2";     shift 2 ;;
            --push)      push_ref="$2";  shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    [[ -n "$workspace" ]] || die "--workspace required"
    [[ -z "$alias"     ]] && alias="bdfs-${workspace}-$(date +%Y%m%d%H%M%S)"

    local ws_root
    ws_root="$("$BDFS_CMD" workspace path "$workspace")" \
        || die "Workspace not found: ${workspace}"

    local tmp_tar
    tmp_tar="$(mktemp /tmp/bdfs-publish-XXXXXX.tar.gz)"
    info "Packing workspace '${workspace}'..."
    tar --numeric-owner -czf "$tmp_tar" -C "$ws_root" . \
        || die "Failed to tar workspace"

    local result_alias
    result_alias="$(_bdfs_incus_image_import "$tmp_tar" "$alias")"
    rm -f "$tmp_tar"
    info "Published: local alias='${result_alias}'"

    if [[ -n "$push_ref" ]]; then
        _bdfs_incus_image_copy_out "$result_alias" "$push_ref"
        info "Pushed: ${push_ref}"
    fi
}

# ── nested-docker / nested-podman ─────────────────────────────────────────────
#
# Launches an Incus system container with Docker (or Podman) installed inside
# it, and the specified bdfs workspace bind-mounted at /workspace.
#
# Docker requires security.nesting=true plus syscall interception to work
# inside an Incus container. Podman only needs security.nesting=true.
#
# The instance is NOT ephemeral — it persists so the user can exec into it
# and run docker/podman commands against the workspace.

_cmd_nested_engine() {
    local engine="$1"   # "docker" or "podman"
    shift
    _bdfs_incus_require
    command -v bdfs &>/dev/null || die "bdfs not found"

    local workspace="" instance="" start=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace) workspace="$2"; shift 2 ;;
            --instance)  instance="$2";  shift 2 ;;
            --start)     start=true;     shift   ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    [[ -n "$workspace" ]] || die "--workspace required"
    [[ -z "$instance"  ]] && instance="bdfs-nested-${engine}-${workspace}"

    local ws_root
    ws_root="$("$BDFS_CMD" workspace path "$workspace")" \
        || die "Workspace not found: ${workspace}"
    local abs_ws
    abs_ws="$(realpath "$ws_root")"

    # Check if instance already exists
    if "$INCUS_CMD" info "$instance" &>/dev/null; then
        info "Instance '${instance}' already exists."
        $start && { _bdfs_incus_wait_ready "$instance" 30 2>/dev/null \
            || "$INCUS_CMD" start "$instance"; }
        info "Connect: incus exec ${instance} -- bash"
        return 0
    fi

    info "Creating nested-${engine} instance: ${instance}"
    info "  Workspace: ${abs_ws} → /workspace (inside instance)"

    # Config for Docker-in-Incus (needs syscall interception)
    local -a configs=(
        "security.nesting=true"
    )
    if [[ "$engine" == "docker" ]]; then
        configs+=(
            "security.syscalls.intercept.mknod=true"
            "security.syscalls.intercept.setxattr=true"
        )
    fi

    local -a devices=(
        "workspace,type=disk,source=${abs_ws},path=/workspace"
    )

    # Use Ubuntu 24.04 as the base — well-tested with both Docker and Podman
    local base_image="ubuntu:24.04"

    local -a launch_args=()
    for c in "${configs[@]}";  do launch_args+=(--config  "$c"); done
    for d in "${devices[@]}";  do launch_args+=(--device  "$d"); done
    launch_args+=(--no-start)

    _bdfs_incus_launch "$base_image" "$instance" "${launch_args[@]}"

    if $start; then
        "$INCUS_CMD" start "$instance"
        _bdfs_incus_wait_ready "$instance" 60

        # Install the engine inside the container
        info "Installing ${engine} inside '${instance}'..."
        case "$engine" in
            docker)
                _bdfs_incus_exec "$instance" -- bash -c "
                    apt-get update -qq &&
                    apt-get install -y --no-install-recommends ca-certificates curl gnupg &&
                    install -m 0755 -d /etc/apt/keyrings &&
                    curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
                        | gpg --dearmor -o /etc/apt/keyrings/docker.gpg &&
                    echo 'deb [arch=\$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
                        https://download.docker.com/linux/ubuntu noble stable' \
                        > /etc/apt/sources.list.d/docker.list &&
                    apt-get update -qq &&
                    apt-get install -y docker-ce docker-ce-cli containerd.io &&
                    systemctl enable --now docker
                " || warn "Docker installation may have partially failed"
                ;;
            podman)
                _bdfs_incus_exec "$instance" -- bash -c "
                    apt-get update -qq &&
                    apt-get install -y --no-install-recommends podman &&
                    podman info
                " || warn "Podman installation may have partially failed"
                ;;
        esac

        info "${engine^} installed in '${instance}'"
    fi

    info "Instance created: ${instance}"
    info "  Start:   incus start ${instance}"
    info "  Connect: incus exec ${instance} -- bash"
    info "  Workspace available at /workspace inside the instance"

    # Record instance name in workspace metadata
    local meta_dir
    meta_dir="$("$BDFS_CMD" workspace path "$workspace")/.bdfs"
    mkdir -p "$meta_dir"
    echo "$instance" >> "${meta_dir}/nested-instances"
}

cmd_nested_docker() { _cmd_nested_engine "docker" "$@"; }
cmd_nested_podman() { _cmd_nested_engine "podman" "$@"; }

# ── profile ───────────────────────────────────────────────────────────────────

cmd_profile() {
    _bdfs_incus_require
    local action="${1:-list}"
    shift || true
    case "$action" in
        apply)
            local profile="${1:-$BDFS_INCUS_PROFILE}" instance="${2:-}"
            [[ -n "$instance" ]] || die "Usage: profile apply <profile> <instance>"
            "$INCUS_CMD" profile add "$instance" "$profile"
            info "Applied profile '${profile}' to '${instance}'"
            ;;
        list)
            "$INCUS_CMD" profile list
            ;;
        show)
            local profile="${1:-$BDFS_INCUS_PROFILE}"
            "$INCUS_CMD" profile show "$profile"
            ;;
        *) die "Unknown profile action: ${action}. Try: apply|list|show" ;;
    esac
}

# ── status ────────────────────────────────────────────────────────────────────

cmd_status() {
    _bdfs_incus_require
    echo "[bdfs-incus-runtime] Status"; echo ""

    echo "  Incus version:"
    "$INCUS_CMD" version 2>/dev/null | sed 's/^/    /' || echo "    (unavailable)"
    echo ""

    echo "  bdfs-managed instances:"
    "$INCUS_CMD" list --format csv 2>/dev/null | grep '^bdfs-' | sed 's/^/    /' \
        || echo "    none"
    echo ""

    echo "  bdfs-managed images:"
    "$INCUS_CMD" image list --format csv 2>/dev/null | grep '^bdfs-' | sed 's/^/    /' \
        || echo "    none"
    echo ""

    echo "  OCI remotes:"
    "$INCUS_CMD" remote list --format csv 2>/dev/null | grep 'oci' | sed 's/^/    /' \
        || echo "    none"
    echo ""

    if command -v bdfs &>/dev/null; then
        echo "  bdfs workspaces:"
        bdfs workspace list 2>/dev/null | sed 's/^/    /' || echo "    none"
    fi
}

# ── dispatch ──────────────────────────────────────────────────────────────────

SUBCMD="${1:-}"
[[ -z "$SUBCMD" ]] && { sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 1; }
shift
case "$SUBCMD" in
    init)           cmd_init           "$@" ;;
    launch)         cmd_launch         "$@" ;;
    run)            cmd_run            "$@" ;;
    workspace)      cmd_workspace      "$@" ;;
    publish)        cmd_publish        "$@" ;;
    nested-docker)  cmd_nested_docker  "$@" ;;
    nested-podman)  cmd_nested_podman  "$@" ;;
    profile)        cmd_profile        "$@" ;;
    status)         cmd_status         "$@" ;;
    --help|-h)      sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
    *) die "Unknown subcommand: ${SUBCMD}. Try: init|launch|run|workspace|publish|nested-docker|nested-podman|profile|status" ;;
esac
