#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Interrupt the tool with SIGINT and check that it unwinds instead of
# hanging.  The caller reports LEFTOVER_INSTANCES separately.
# Usage: check-sigint.sh
# Output: "SIGINT handled OK" on success, an error otherwise.
#
# The tool is a child of the test shell, so once it has exited it stays
# visible as a zombie until it is reaped -- and kill -0 still succeeds on a
# zombie.  Check the process state instead, or a clean exit looks like a hang.

pid=$RTLA_PID
[ -n "$pid" ] || { echo "RTLA_PID not set"; exit 1; }

kill -INT "$pid" 2>/dev/null || { echo "Failed to signal rtla"; exit 1; }

# rtla's SIGINT handler stops tracing and lets the main loop return.  Give it
# a few seconds before declaring it stuck.
for _ in $(seq 30)
do
	[ -r "/proc/$pid/stat" ] || break
	[ "$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)" = "Z" ] && break
	sleep 0.2
done

if [ -r "/proc/$pid/stat" ] && [ "$(awk '{print $3}' "/proc/$pid/stat")" != "Z" ]
then
	echo "rtla still running 6s after SIGINT"
	exit 1
fi

echo "SIGINT handled OK"
