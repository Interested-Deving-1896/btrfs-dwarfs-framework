#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run_all.sh - Run all integration tests and report results

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BOLD='\033[1m'; NC='\033[0m'

TOTAL_PASS=0; TOTAL_FAIL=0; TOTAL_SKIP=0
FAILED_SUITES=()

run_suite() {
    local script=$1
    local name
    name=$(basename "$script" .sh)

    echo ""
    echo -e "${BOLD}━━━ ${name} ━━━${NC}"

    local output
    local rc=0
    output=$(bash "$script" 2>&1) || rc=$?

    echo "$output"

    # Parse pass/fail/skip counts from the summary line
    local pass skip fail
    pass=$(echo "$output" | grep -oP '\d+(?= passed)' | tail -1 || echo 0)
    fail=$(echo "$output" | grep -oP '\d+(?= failed)' | tail -1 || echo 0)
    skip=$(echo "$output" | grep -oP '\d+(?= skipped)' | tail -1 || echo 0)

    TOTAL_PASS=$(( TOTAL_PASS + pass ))
    TOTAL_FAIL=$(( TOTAL_FAIL + fail ))
    TOTAL_SKIP=$(( TOTAL_SKIP + skip ))

    if [[ $rc -ne 0 || $fail -gt 0 ]]; then
        FAILED_SUITES+=("$name")
    fi
}

echo -e "${BOLD}BTRFS+DwarFS Framework Integration Tests${NC}"
echo "========================================="
echo "Date: $(date)"
echo "Kernel: $(uname -r)"
echo ""

if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}error:${NC} Integration tests require root."
    exit 1
fi

# Run all test suites
for suite in \
    "$SCRIPT_DIR/test_dwarfs_partition.sh" \
    "$SCRIPT_DIR/test_btrfs_partition.sh" \
    "$SCRIPT_DIR/test_blend_layer.sh" \
    "$SCRIPT_DIR/test_snapshot_lifecycle.sh"
do
    if [[ -f "$suite" ]]; then
        run_suite "$suite"
    else
        echo -e "${YELLOW}MISSING${NC} $suite"
    fi
done

# ── Final summary ─────────────────────────────────────────────────────────────

TOTAL=$(( TOTAL_PASS + TOTAL_FAIL + TOTAL_SKIP ))
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${BOLD}Overall: ${TOTAL} tests${NC}"
echo -e "  ${GREEN}Passed:  ${TOTAL_PASS}${NC}"
echo -e "  ${RED}Failed:  ${TOTAL_FAIL}${NC}"
echo -e "  ${YELLOW}Skipped: ${TOTAL_SKIP}${NC}"

if [[ ${#FAILED_SUITES[@]} -gt 0 ]]; then
    echo ""
    echo -e "${RED}Failed suites:${NC}"
    for s in "${FAILED_SUITES[@]}"; do
        echo "  - $s"
    done
    exit 1
else
    echo ""
    echo -e "${GREEN}All suites passed.${NC}"
    exit 0
fi
