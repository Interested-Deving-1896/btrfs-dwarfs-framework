#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# test_blend_layer.sh - Blend layer (unified namespace) tests
#
# Tests the promote/demote cycle and unified namespace behaviour:
#   1. Set up a BTRFS upper layer and a DwarFS lower layer
#   2. Simulate the blend namespace by overlaying them with overlayfs
#      (the real bdfs_blend kernel module is tested separately; here we
#       validate the data-movement semantics using overlayfs as a proxy)
#   3. Verify read routing: lower layer visible through blend
#   4. Verify write routing: writes go to upper layer (copy-up)
#   5. Demote: export upper subvolume to DwarFS, verify image
#   6. Promote: extract DwarFS image to new subvolume, verify content
#   7. Verify promote→demote→promote round-trip preserves data

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

require_root
require_cmd mkfs.btrfs    || exit 0
require_cmd mkdwarfs      || exit 0
require_cmd dwarfs        || exit 0
require_cmd dwarfsextract || exit 0
require_cmd fusermount    || exit 0
require_module overlay    || exit 0

echo "=== Blend layer tests ==="

# ── Setup ────────────────────────────────────────────────────────────────────

BTRFS_MNT=$(mktemp -d /tmp/bdfs_blend_btrfs_XXXXXX)
DWARFS_MNT=$(mktemp -d /tmp/bdfs_blend_dwarfs_XXXXXX)
BLEND_MNT=$(mktemp -d /tmp/bdfs_blend_XXXXXX)
WORK_DIR=$(mktemp -d /tmp/bdfs_blend_work_XXXXXX)
DWARFS_IMG=$(mktemp /tmp/bdfs_blend_XXXXXX.dwarfs)
TEMP_DIRS+=("$BTRFS_MNT" "$DWARFS_MNT" "$BLEND_MNT" "$WORK_DIR" "$DWARFS_IMG")

make_loop_device 512 BTRFS_DEV
make_btrfs "$BTRFS_DEV" "bdfs_blend_test"
mount_btrfs "$BTRFS_DEV" "$BTRFS_MNT"

# Create upper and work subvolumes for overlayfs
btrfs subvolume create "$BTRFS_MNT/upper" &>/dev/null
btrfs subvolume create "$BTRFS_MNT/work"  &>/dev/null

# ── Test 1: Create lower layer (DwarFS image) ─────────────────────────────────

# Populate a directory to become the DwarFS lower layer
LOWER_SRC=$(mktemp -d /tmp/bdfs_lower_XXXXXX)
TEMP_DIRS+=("$LOWER_SRC")
mkdir -p "$LOWER_SRC/shared" "$LOWER_SRC/lower_only"
echo "lower_data" > "$LOWER_SRC/shared/file.txt"
echo "only_in_lower" > "$LOWER_SRC/lower_only/data.txt"
for i in $(seq 1 10); do
    printf 'lower content %d\n%.0s' {1..200} \
        > "$LOWER_SRC/lower_only/bulk_$i.txt"
done

mkdwarfs -i "$LOWER_SRC" -o "$DWARFS_IMG" \
    --compression zstd --num-workers 2 &>/dev/null
assert_file_exists "lower DwarFS image created" "$DWARFS_IMG"

# Mount DwarFS as lower layer
MOUNT_POINTS+=("$DWARFS_MNT")
dwarfs "$DWARFS_IMG" "$DWARFS_MNT" -o cache_size=32m &>/dev/null
sleep 0.5
assert_file_exists "lower layer accessible" "$DWARFS_MNT/shared/file.txt"

# ── Test 2: Mount blend (overlayfs: upper=BTRFS, lower=DwarFS) ───────────────

MOUNT_POINTS+=("$BLEND_MNT")
mount -t overlay overlay \
    -o "lowerdir=$DWARFS_MNT,upperdir=$BTRFS_MNT/upper,workdir=$BTRFS_MNT/work" \
    "$BLEND_MNT"

assert_dir_exists "blend mount point exists" "$BLEND_MNT"

# ── Test 3: Read routing — lower layer visible through blend ──────────────────

assert_file_exists "lower-only file visible in blend" \
    "$BLEND_MNT/lower_only/data.txt"
assert_eq "lower file content correct" \
    "$(cat "$BLEND_MNT/lower_only/data.txt")" "only_in_lower"
assert_eq "shared file content from lower" \
    "$(cat "$BLEND_MNT/shared/file.txt")" "lower_data"

# ── Test 4: Write routing — writes go to upper (copy-up) ─────────────────────

echo "upper_data" > "$BLEND_MNT/shared/file.txt"
assert_eq "write visible in blend" \
    "$(cat "$BLEND_MNT/shared/file.txt")" "upper_data"

# Upper layer has the modified file
assert_eq "write landed in upper layer" \
    "$(cat "$BTRFS_MNT/upper/shared/file.txt")" "upper_data"

# Lower layer is unchanged
assert_eq "lower layer unmodified after write" \
    "$(cat "$DWARFS_MNT/shared/file.txt")" "lower_data"

# ── Test 5: New file in blend goes to upper ───────────────────────────────────

echo "new_file" > "$BLEND_MNT/new_upper_file.txt"
assert_file_exists "new file in upper layer" \
    "$BTRFS_MNT/upper/new_upper_file.txt"
assert_cmd_fails "new file not in lower layer" \
    test -f "$DWARFS_MNT/new_upper_file.txt"

# ── Test 6: Demote — export upper subvolume to DwarFS ─────────────────────────

DEMOTE_IMG=$(mktemp /tmp/bdfs_demote_XXXXXX.dwarfs)
TEMP_DIRS+=("$DEMOTE_IMG")

mkdwarfs -i "$BTRFS_MNT/upper" -o "$DEMOTE_IMG" \
    --compression zstd --num-workers 2 &>/dev/null
assert_file_exists "demoted DwarFS image created" "$DEMOTE_IMG"

# Verify demoted image contains the upper-layer writes
DEMOTE_MNT=$(mktemp -d /tmp/bdfs_demote_mnt_XXXXXX)
TEMP_DIRS+=("$DEMOTE_MNT")
MOUNT_POINTS+=("$DEMOTE_MNT")
dwarfs "$DEMOTE_IMG" "$DEMOTE_MNT" -o cache_size=32m &>/dev/null
sleep 0.5

assert_eq "demoted image has upper-layer content" \
    "$(cat "$DEMOTE_MNT/shared/file.txt")" "upper_data"
assert_file_exists "new file present in demoted image" \
    "$DEMOTE_MNT/new_upper_file.txt"

fusermount -u "$DEMOTE_MNT" &>/dev/null
MOUNT_POINTS=("${MOUNT_POINTS[@]/$DEMOTE_MNT}")

# ── Test 7: Promote — extract DwarFS image to new BTRFS subvolume ─────────────

btrfs subvolume create "$BTRFS_MNT/promoted" &>/dev/null
dwarfsextract -i "$DEMOTE_IMG" -o "$BTRFS_MNT/promoted" &>/dev/null

assert_file_exists "promoted subvol has upper-layer content" \
    "$BTRFS_MNT/promoted/shared/file.txt"
assert_eq "promoted content matches demoted" \
    "$(cat "$BTRFS_MNT/promoted/shared/file.txt")" "upper_data"

# ── Test 8: Round-trip data integrity ─────────────────────────────────────────

# Write → demote → promote → verify
echo "round_trip_test" > "$BLEND_MNT/round_trip.txt"
ROUND_IMG=$(mktemp /tmp/bdfs_round_XXXXXX.dwarfs)
TEMP_DIRS+=("$ROUND_IMG")

mkdwarfs -i "$BTRFS_MNT/upper" -o "$ROUND_IMG" \
    --compression zstd --num-workers 2 &>/dev/null

btrfs subvolume create "$BTRFS_MNT/round_promoted" &>/dev/null
dwarfsextract -i "$ROUND_IMG" -o "$BTRFS_MNT/round_promoted" &>/dev/null

assert_eq "round-trip data integrity" \
    "$(cat "$BTRFS_MNT/round_promoted/round_trip.txt")" "round_trip_test"

# ── Test 9: Snapshot of upper layer before demote ─────────────────────────────

btrfs subvolume snapshot -r \
    "$BTRFS_MNT/upper" \
    "$BTRFS_MNT/upper_snap" &>/dev/null
assert_dir_exists "pre-demote snapshot created" "$BTRFS_MNT/upper_snap"

# Modify upper after snapshot
echo "post_snap" > "$BLEND_MNT/post_snap.txt"
assert_cmd_fails "snapshot does not see post-snap write" \
    test -f "$BTRFS_MNT/upper_snap/post_snap.txt"

print_summary
