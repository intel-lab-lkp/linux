#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Usage: $ ./pahole-class.sh pahole
#
# Prints pahole's ELF class, such as ELF64

if [ ! -x "$(command -v "$@")" ]; then
	echo 0
	exit 1
fi

PAHOLE="$(which "$@")"
CLASS="$(readelf -h "$PAHOLE" 2>/dev/null | sed -n 's/.*Class: *// p')"

# Scripts like scripts/dummy-tools/pahole
if [ -n "$CLASS" ]; then
	echo "$CLASS"
else
	echo ELF64
fi
