#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e

DIR="$(dirname "$(readlink -f "$0")")"
# shellcheck source=../kselftest/ktap_helpers.sh
source "${DIR}"/../kselftest/ktap_helpers.sh

ksft_skip()
{
	ktap_skip_all "$1"
	exit "$KSFT_SKIP"
}

[ "$(id -u)" -eq 0 ] || ksft_skip "must be run as root"

for tool in mke2fs tune2fs losetup truncate; do
	command -v "$tool" >/dev/null 2>&1 ||
		ksft_skip "missing required command: $tool"
done

tmpdir=$(mktemp -d)
loop_dev=

cleanup()
{
	if [ -n "$loop_dev" ]; then
		losetup -d "$loop_dev"
	fi
	rm -rf "$tmpdir"
}
trap cleanup EXIT

image="$tmpdir/ext4.img"
truncate -s 16M "$image"
mke2fs -q -F -t ext4 -b 4096 -N 1024 -m 10 -O ^has_journal "$image"
tune2fs -u 123 -g 456 "$image" >/dev/null

loop_dev=$(losetup --find --show "$image") ||
	ksft_skip "could not allocate a loop device"

./ext4_resgid_test "$loop_dev"
