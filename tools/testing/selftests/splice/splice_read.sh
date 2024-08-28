#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -e
nl=$(./splice_read /etc/os-release | wc -l)

test "$nl" != 0 && exit 0

echo "splice_read broken"
exit 1
