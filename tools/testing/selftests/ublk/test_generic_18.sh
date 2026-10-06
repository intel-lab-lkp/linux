#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

. "$(cd "$(dirname "$0")" && pwd)"/test_common.sh

ERR_CODE=0

if ! _have_program fio; then
	exit "$UBLK_SKIP_CODE"
fi

if ! _have_feature "SUPPORT_RQ_AFFINITY"; then
	exit "$UBLK_SKIP_CODE"
fi

_prep_test "loop" "write and verify I/O across rq_affinity settings"

_create_backfile 0 256M

dev_id=$(_add_ublk_dev -t loop --rq_affinity "${UBLK_BACKFILES[0]}")
_check_add_dev $TID $?

rq_affinity_path=/sys/block/ublkb"${dev_id}"/queue/rq_affinity

for affinity in 0 1 2; do
	echo "${affinity}" > "${rq_affinity_path}"

	# Pin submission to a single CPU so rq_affinity has somewhere to
	# redirect completion to.
	taskset -c 0 fio --name=verify --rw=randwrite --direct=1 \
		--ioengine=libaio --bs=8k --iodepth=32 --verify=crc32c \
		--do_verify=1 --verify_state_save=0 \
		--filename=/dev/ublkb"${dev_id}" --size=256M > /dev/null
	if [ $? -ne 0 ]; then
		echo "fio verify failed with rq_affinity=${affinity}"
		ERR_CODE=255
		break
	fi
done

_cleanup_test
_show_result $TID $ERR_CODE
