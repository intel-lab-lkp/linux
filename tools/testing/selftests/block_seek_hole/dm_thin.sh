#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# dm_thin.sh
#
# Test that dm-thin supports SEEK_HOLE/SEEK_DATA.

set -e

# check <actual> <expected>
# Check that the actual output matches the expected output.
check() {
	if [ "$1" != "$2" ]; then
		echo 'FAIL expected:'
		echo "$2"
		echo 'Does not match device output:'
		echo "$1"
		exit 1
	fi
}

cleanup() {
	if [ -n "$thin_name" ]; then
		dmsetup remove $thin_name
	fi
	if [ -n "$pool_name" ]; then
		dmsetup remove $pool_name
	fi
	if [ -n "$metadata_path" ]; then
		losetup --detach "$metadata_path"
	fi
	if [ -n "$data_path" ]; then
		losetup --detach "$data_path"
	fi
	rm -f pool-metadata pool-data
}
trap cleanup EXIT

rm -f pool-metadata pool-data
truncate -s 256M pool-metadata
truncate -s 1G pool-data

size_sectors=$((1024 * 1024 * 1024 / 512)) # 1 GB
metadata_path=$(losetup --show --find pool-metadata)
data_path=$(losetup --show --find pool-data)
pool_name=pool-$$
thin_name=thin-$$

dmsetup create $pool_name \
	--table "0 $size_sectors thin-pool $metadata_path $data_path 128 $size_sectors"
dmsetup message /dev/mapper/$pool_name 0 'create_thin 0'
dmsetup create $thin_name --table "0 $size_sectors thin /dev/mapper/$pool_name 0"

# Verify that the device is empty
check "$(./map_holes.py /dev/mapper/$thin_name)" 'TYPE START END SIZE
HOLE 0 1073741824 1073741824'

# Write 4k at offset 128M but dm-thin will actually map an entire 64k block
dd if=/dev/urandom of=/dev/mapper/$thin_name bs=4k count=1 seek=32768 status=none
check "$(./map_holes.py /dev/mapper/$thin_name)" 'TYPE START END SIZE
HOLE 0 134217728 134217728
DATA 134217728 134283264 65536
HOLE 134283264 1073741824 939458560'

# Write at the beginning of the device
dd if=/dev/urandom of=/dev/mapper/$thin_name bs=4k count=1 status=none
check "$(./map_holes.py /dev/mapper/$thin_name)" 'TYPE START END SIZE
DATA 0 65536 65536
HOLE 65536 134217728 134152192
DATA 134217728 134283264 65536
HOLE 134283264 1073741824 939458560'

# Write at the end of the device
dd if=/dev/urandom of=/dev/mapper/$thin_name bs=4k count=1 seek=262143 status=none
check "$(./map_holes.py /dev/mapper/$thin_name)" 'TYPE START END SIZE
DATA 0 65536 65536
HOLE 65536 134217728 134152192
DATA 134217728 134283264 65536
HOLE 134283264 1073676288 939393024
DATA 1073676288 1073741824 65536'
