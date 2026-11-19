#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-python3}
command -v "$PYTHON" >/dev/null 2>&1 || {
    echo "python3 is required to validate BDFS target profiles" >&2
    exit 127
}
exec "$PYTHON" "$SCRIPT_DIR/validate.py"
