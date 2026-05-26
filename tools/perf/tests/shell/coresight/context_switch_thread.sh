#!/bin/bash -e
# CoreSight / Context switch thread attribution (exclusive)

# SPDX-License-Identifier: GPL-2.0

TEST="context_switch_loop"

if [ "$(id -u)" != 0 ]; then
	# Requires root for "-C 0" in record command
	echo "[Skip] No root permission"
	exit 2
fi

# shellcheck source=../lib/coresight.sh
. "$(dirname $0)"/../lib/coresight.sh

DATA="$DATD/perf-$TEST.data"
SCRIPT="$DATD/perf-$TEST.script"

check_samples() {
	owner_samples=$(grep -c "thread1.*thread1" "$SCRIPT" || true)
	next_samples=$(grep -c "thread2.*thread2" "$SCRIPT" || true)

	if [ "$owner_samples" -eq 0 ] || [ "$next_samples" -eq 0 ]; then
		err "No samples found"
	fi

	if grep "thread2.*thread1" "$SCRIPT"; then
		err "Thread1 symbol was attributed to thread2"
	fi

	if grep "thread1.*thread2" "$SCRIPT"; then
		err "Thread2 symbol was attributed to thread1"
	fi
}

# Pin to one CPU so the two threads alternate running but record into the
# same trace buffer.
perf record -o "$DATA" -e cs_etm/timestamp=0/u -C 0 \
	-- taskset --cpu-list 0 "$BIN" 20 > /dev/null 2>&1

# Test both instruction and branch sample generation modes.
perf script -i "$DATA" --itrace=i4 -F comm,pid,tid,ip,sym > "$SCRIPT" 2>/dev/null
check_samples
perf script -i "$DATA" --itrace=b -F comm,pid,tid,ip,sym > "$SCRIPT" 2>/dev/null
check_samples

exit 0
