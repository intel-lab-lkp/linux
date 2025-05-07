#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

. "$(cd "$(dirname "$0")" && pwd)"/test_common.sh

TID="generic_08"
ERR_CODE=0

if ! _have_program bpftrace; then
	exit "$UBLK_SKIP_CODE"
fi

_prep_test "null" "do imbalanced load, it should be balanced over I/O threads"

NTHREADS=8
dev_id=$(_add_ublk_dev -t null -q 4 -d 16 --nthreads $NTHREADS --round_robin)
_check_add_dev $TID $?

dev_t=$(_get_disk_dev_t "$dev_id")
bpftrace trace/count_ios_per_tid.bt "$dev_t" > "$UBLK_TMP" 2>&1 &
btrace_pid=$!
sleep 2

if ! kill -0 "$btrace_pid" > /dev/null 2>&1; then
	_cleanup_test "null"
	exit "$UBLK_SKIP_CODE"
fi

# do imbalanced I/O on the ublk device
# single-threaded because while tags are assigned round-robin, I/O
# completions can come in any order, and this can cause imperfect
# balance (in practice, balance is close to perfect, with less than 0.1%
# error, but prefer perfection/determinism for automated tests)
# pin to cpu 0 to prevent migration/only target one queue
IOS_PER_THREAD=1024
TOTAL_IOS=$(($IOS_PER_THREAD * $NTHREADS))
taskset -c 0 dd if=/dev/urandom of=/dev/ublkb"${dev_id}" \
        oflag=direct bs=4k count=${TOTAL_IOS} > /dev/null 2>&1
ERR_CODE=$?
kill "$btrace_pid"
wait

# check for perfectly balanced I/O
# note that this depends on few things:
# - $NTHREADS should divide the queue depth
# - the queue depth should divide $IOS_PER_THREAD
# - no I/O on the device should happen while the bpftrace script is
#   running, besides the dd in this script
# the check below could be made more sophisticated to relax the first
# two constraints above
grep '@' < ${UBLK_TMP} | cut -d ' ' -f2 | while read ios; do
        if [ "${ios}" -ne "${IOS_PER_THREAD}" ]; then
                echo "imbalanced i/o detected!"
                cat "$UBLK_TMP"
                exit 255
        fi
done
ERR_CODE=$?

_cleanup_test "null"
_show_result $TID $ERR_CODE
