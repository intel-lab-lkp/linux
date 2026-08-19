#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Standalone runner for tlob selftests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FTRACETEST="$SCRIPT_DIR/../../../ftrace/ftracetest"

# Build test helpers
echo "Building tlob test helpers..."
make -C "$SCRIPT_DIR/../.." all

# Export VERIFICATIONTEST_BINDIR so test scripts can find tlob_target and
# tlob_sym (built in the verification directory)
export VERIFICATIONTEST_BINDIR="$(realpath "$SCRIPT_DIR/../..")"

# Forward all options to ftracetest
exec "$FTRACETEST" -K --rv "$SCRIPT_DIR" "$@"
