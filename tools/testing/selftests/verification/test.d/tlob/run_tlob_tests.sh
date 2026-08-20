#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Standalone runner for tlob selftests

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
FTRACETEST="$(cd "$SCRIPT_DIR/../../../ftrace" && pwd)/ftracetest"

# Rebuild helpers when the Makefile is present (source-tree run).
# Skip silently in installed kselftest environments where source is absent.
if [ -f "$BINDIR/Makefile" ]; then
	make -C "$BINDIR" tlob_target tlob_sym
fi

if [ ! -x "$BINDIR/tlob_target" ] || [ ! -x "$BINDIR/tlob_sym" ]; then
	echo "ERROR: tlob_target or tlob_sym not found in $BINDIR" >&2
	exit 1
fi

export VERIFICATIONTEST_BINDIR="$BINDIR"

exec "$FTRACETEST" -K --rv "$SCRIPT_DIR" "$@"
