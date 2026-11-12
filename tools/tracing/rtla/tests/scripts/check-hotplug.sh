#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Offline and online a CPU while rtla timerlat runs, and check that the
# CPU coming back did not install a workload kthread.
#
# timerlat can sample in two ways.  With OSN_WORKLOAD set the sampling is
# done by a "timerlat/<cpu>" kernel thread; with it clear (what rtla's
# user-space mode uses) the sampling is done by the user thread itself,
# named "timerlatu/<cpu>".  Only one of the two may exist for a CPU.
#
# Onlining a CPU used to install the kernel thread even in user-space mode,
# because the hotplug worker called start_kthread() without the check that
# start_per_cpu_kthreads() does.  stop_kthread() then picked the teardown
# path from the current OSN_WORKLOAD, treated that kthread as a user thread
# and freed it with put_task_struct() although kthread_stop() owns it, which
# ends in warnings from stop_kthread, __put_task_struct and free_task, and
# then in NULL pointer faults.
#
# The stray kthread shows up within a fraction of a second of the CPU coming
# back, well before the teardown that crashes, so asserting on it gives a
# clean failure instead of taking the kernel down.
#
# Usage: check-hotplug.sh <rtla> <cpu>
#
# Prints "HOTPLUG_FAIL: ..." on failure, "HOTPLUG_SKIP: ..." when the system
# cannot run the test, and "HOTPLUG_OK" otherwise.

RTLA=${1:?usage: check-hotplug.sh <rtla> <cpu>}
CPU=${2:?usage: check-hotplug.sh <rtla> <cpu>}
ONLINE=/sys/devices/system/cpu/cpu$CPU/online

[ -x "$RTLA" ] || { echo "HOTPLUG_SKIP: $RTLA is not executable"; exit 0; }
[ -w "$ONLINE" ] || { echo "HOTPLUG_SKIP: $ONLINE is not writable"; exit 0; }

# Names of the timerlat workload threads that currently exist, one per line.
timerlat_threads() {
	for c in /proc/[0-9]*/comm; do
		read -r name <"$c" 2>/dev/null || continue
		case "$name" in
		timerlat/*) echo "$name" ;;
		esac
	done
}

"$RTLA" timerlat top -q -d 20s -c 0,"$CPU" -P F:1 >/dev/null 2>&1 &
pid=$!

# Let rtla create its instance and settle into user-space sampling.
sleep 3

if ! echo 0 >"$ONLINE"; then
	echo "HOTPLUG_SKIP: could not offline cpu$CPU"
	kill -INT $pid 2>/dev/null; wait $pid 2>/dev/null; exit 0
fi
if ! echo 1 >"$ONLINE"; then
	echo "HOTPLUG_SKIP: could not online cpu$CPU"
	kill -INT $pid 2>/dev/null; wait $pid 2>/dev/null; exit 0
fi

# The hotplug work is queued on the CPU that just came up, and the damage is
# done as part of bringing it up: stop_kthread() runs before this point and
# the log is already tainted.  Check both, so that a build where the stray
# kthread is gone but the teardown still misbehaves does not slip through.
stray=''
i=0
while [ $i -lt 20 ]; do
	stray=$(timerlat_threads)
	[ -n "$stray" ] && break
	sleep 0.2
	i=$((i + 1))
done

# Give the run time to reach its own stop path, which is where the kthread
# gets torn down, and then let it finish.
sleep 2
kill -INT $pid 2>/dev/null
wait $pid 2>/dev/null
sleep 1

warn=$(dmesg 2>/dev/null | grep -c 'WARNING: at')
fault=$(dmesg 2>/dev/null | grep -c 'BUG: kernel NULL pointer')

if [ -n "$stray" ]; then
	echo "stray workload kthread(s): $(echo $stray)"
	echo "HOTPLUG_FAIL: cpu$CPU came back online and a timerlat" \
		"workload kthread was started in user-space mode"
	exit 1
fi

if [ "$fault" -gt 0 ] || [ "$warn" -gt 0 ]; then
	dmesg | grep -A12 'WARNING: at' | tail -30
	echo "HOTPLUG_FAIL: the kernel warned ($warn) or faulted ($fault) during the hotplug run"
	exit 1
fi

echo "HOTPLUG_OK"
