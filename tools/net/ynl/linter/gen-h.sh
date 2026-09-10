#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

# Usage: ./gen-h.sh $CFLAGS < in.c > out.h
#
# Extract valid symbols, and define that they are available. This is a
# workaround, to avoid compile failures due to non-existent enum members.
#
# Capitalized keywords found in the preprocessor output, are mostly enum
# members.
#
# As an example, existence of FOO can be checked with:
#   #if defined(FOO) || defined(LINTER_HAS_FOO)
# thus checking for FOO either as a macro or as an enum members.

set -e

echo '/* This is an auto-generated file */'
grep '^#include <linux/' |
	cpp -x c "$@" - |
	grep -wo '[A-Z][A-Z0-9_]\+' |
	sort | uniq |
	sed -e 's/^/#define LINTER_HAS_/g'
