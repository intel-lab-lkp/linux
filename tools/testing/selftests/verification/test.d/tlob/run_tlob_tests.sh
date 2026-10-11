#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Standalone runner for tlob selftests

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
FTRACETEST="$(cd "$SCRIPT_DIR/../../../ftrace" && pwd)/ftracetest"

# The helper is built on the host by the selftests Makefile
# (TEST_GEN_FILES in $BINDIR/Makefile); the VM (virtme-ng) mounts the
# source read-only and may lack a compiler, so this runner never builds.
# Build it on the host first:
#   make -C "$BINDIR" tlob_target
if [ ! -x "$BINDIR/tlob_target" ]; then
	echo "ERROR: tlob_target not built in $BINDIR" >&2
	echo "       build it on the host: make -C $BINDIR tlob_target" >&2
	exit 1
fi

# Flag (not fail) the helper older than its source: git checkouts can bump
# source mtimes, so a hard check would misfire; a host rebuild picks up
# the new code.
if [ -f "$BINDIR/tlob_target.c" ] &&
   [ "$BINDIR/tlob_target" -ot "$BINDIR/tlob_target.c" ]; then
	echo "WARNING: $BINDIR/tlob_target is older than $BINDIR/tlob_target.c;" >&2
	echo "         rebuild on the host to test the current source" >&2
fi

export VERIFICATIONTEST_BINDIR="$BINDIR"

exec "$FTRACETEST" -K --rv "$SCRIPT_DIR" "$@"
