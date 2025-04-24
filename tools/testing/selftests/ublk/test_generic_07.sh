#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

. "$(cd "$(dirname "$0")" && pwd)"/test_common.sh

TID="generic_07"
ERR_CODE=0

_prep_test "generic" "test UBLK_F_NEED_GET_DATA"

_create_backfile 0 256M
dev_id=$(_add_ublk_dev -t loop -q 2 -g 1 "${UBLK_BACKFILES[0]}")
_check_add_dev $TID $?

# run fio over the ublk disk
if ! _run_fio_verify_io --filename=/dev/ublkb"${dev_id}" --size=256M; then
	_cleanup_test "generic"
	_show_result $TID 255
fi

_mkfs_mount_test /dev/ublkb"${dev_id}"
ERR_CODE=$?

_cleanup_test "generic"
_show_result $TID $ERR_CODE
