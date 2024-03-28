#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# dm_zero.sh
#
# Test that dm-zero reports data because it does not have a custom
# SEEK_HOLE/SEEK_DATA implementation.

set -e

dev_name=test-$$
size=$((1024 * 1024 * 1024 / 512)) # 1 GB

cleanup() {
	dmsetup remove $dev_name
}
trap cleanup EXIT

dmsetup create $dev_name --table "0 $size zero"

output=$(./map_holes.py /dev/mapper/$dev_name)
expected='TYPE START END SIZE
DATA 0 1073741824 1073741824'

if [ "$output" != "$expected" ]; then
	echo 'FAIL expected:'
	echo "$expected"
	echo 'Does not match device output:'
	echo "$output"
	exit 1
fi
