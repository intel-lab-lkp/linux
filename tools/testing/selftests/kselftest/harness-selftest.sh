#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Selftest for kselftest_harness.h
#

DIR="$(dirname $(readlink -f "$0"))"

TMPFILE="$(mktemp)"
trap 'rm "$TMPFILE"' EXIT

$DIR/harness-selftest > "$TMPFILE"

diff -u "$DIR"/harness-selftest.expected "$TMPFILE"
