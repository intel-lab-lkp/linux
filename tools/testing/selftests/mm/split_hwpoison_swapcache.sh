#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Setup swap and run the mappingless swapcache hwpoison split test

ksft_skip=4
swap_file=
loop_dev=

skip() {
	echo "skip: $*"
	exit "$ksft_skip"
}

cleanup() {
	if [ -n "$loop_dev" ]; then
		swapoff "$loop_dev" 2>/dev/null || true
		losetup -d "$loop_dev" 2>/dev/null || true
	fi
	if [ -n "$swap_file" ]; then
		rm -f "$swap_file"
	fi
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

if [ "$(id -u)" -ne 0 ]; then
	skip "must run as root"
fi

if [ ! -w /sys/kernel/debug/hwpoison/corrupt-pfn ]; then
	skip "hwpoison injection is not available"
fi

# Use a loop device because the kselftest directory may be on 9p or NFS.
if ! swap_file=$(mktemp /tmp/hwpoison_swap.XXXXXX); then
	echo "FAIL: could not create a temporary swap file"
	exit 1
fi
if ! dd if=/dev/zero of="$swap_file" bs=1M count=128 status=none; then
	echo "FAIL: could not initialize the temporary swap file"
	exit 1
fi
if ! loop_dev=$(losetup --find --show "$swap_file"); then
	skip "no loop device is available"
fi
if ! mkswap "$loop_dev" >/dev/null; then
	echo "FAIL: could not initialize swap on $loop_dev"
	exit 1
fi
if ! swapon "$loop_dev"; then
	echo "FAIL: could not enable swap on $loop_dev"
	exit 1
fi

"$(dirname "$(readlink -f "$0")")"/split_hwpoison_swapcache_test
