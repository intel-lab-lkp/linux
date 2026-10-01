#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

. "$(cd "$(dirname "$0")" && pwd)"/test_common.sh

ERR_CODE=0
CANCEL_PROG="$(_ublk_test_top_dir)/ublk_cancel_ready"

_prep_test "generic" "start device over canceled io commands"

# the modes are described in ublk_cancel_ready.c
for mode in stop_start partial_fetch recovery stop_restart stop_attached \
		stop_live_restart race_start race_fetch race_async_fetch; do
	dmesg_before=$(dmesg | wc -l)
	timeout 60 "$CANCEL_PROG" "$mode" > "$UBLK_TMP" 2>&1
	res=$?
	msg=""

	if dmesg | tail -n +"$((dmesg_before + 1))" | \
			grep -q -e "BUG:" -e "Oops" -e "WARNING:"; then
		msg="$mode: kernel oops/warning"
		ERR_CODE=255
	elif [ "$res" -eq "$UBLK_SKIP_CODE" ]; then
		[ "$ERR_CODE" -eq 0 ] && ERR_CODE=$UBLK_SKIP_CODE
	elif [ "$res" -ne 0 ]; then
		msg="$mode: failed ($res)"
		ERR_CODE=255
	fi
	# the output once: on failure, or always when not quiet
	[ -n "$msg" ] && echo "$msg"
	if [ -n "$msg" ] || [ "$UBLK_TEST_QUIET" -eq 0 ]; then
		cat "$UBLK_TMP"
	fi
done

_cleanup_test
_show_result $TID $ERR_CODE
