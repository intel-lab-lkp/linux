# SPDX-License-Identifier: GPL-2.0
#
#	init.sh
#	Author: Michael Petlan <mpetlan@redhat.com>
#
#	Description:
#
#		This file should be used for initialization of basic functions
#	for checking, reporting results etc.
#
#


. "$(dirname $0)/../common/settings.sh"
. "$(dirname $0)/../common/patterns.sh"

THIS_TEST_NAME=`basename $0 .sh`

_echo()
{
	test "$TESTLOG_VERBOSITY" -ne 0 && echo -e "$@"
}

print_results()
{
	PERF_RETVAL="$1"; shift
	CHECK_RETVAL="$1"; shift
	FAILURE_REASON=""
	TASK_COMMENT="$*"
	if [ $PERF_RETVAL -eq 0 ] && [ $CHECK_RETVAL -eq 0 ]; then
		_echo "$MPASS-- [ PASS ] --$MEND $TEST_NAME :: $THIS_TEST_NAME :: $TASK_COMMENT"
		return 0
	else
		if [ $PERF_RETVAL -ne 0 ]; then
			FAILURE_REASON="command exitcode"
		fi
		if [ $CHECK_RETVAL -ne 0 ]; then
			test -n "$FAILURE_REASON" && FAILURE_REASON="$FAILURE_REASON + "
			FAILURE_REASON="$FAILURE_REASON""output regexp parsing"
		fi
		_echo "$MFAIL-- [ FAIL ] --$MEND $TEST_NAME :: $THIS_TEST_NAME :: $TASK_COMMENT ($FAILURE_REASON)"
		return 1
	fi
}

print_overall_results()
{
	RETVAL="$1"; shift
	TASK_COMMENT="$*"
	test -n "$TASK_COMMENT" && TASK_COMMENT=":: $TASK_COMMENT"

	if [ $RETVAL -eq 0 ]; then
		_echo "$MALLPASS## [ PASS ] ##$MEND $TEST_NAME :: $THIS_TEST_NAME SUMMARY"
	else
		_echo "$MALLFAIL## [ FAIL ] ##$MEND $TEST_NAME :: $THIS_TEST_NAME SUMMARY :: $RETVAL failures found $TASK_COMMENT"
	fi
	return $RETVAL
}

print_testcase_skipped()
{
	TASK_COMMENT="$*"
	_echo "$MSKIP-- [ SKIP ] --$MEND $TEST_NAME :: $THIS_TEST_NAME :: $TASK_COMMENT :: testcase skipped"
	return 0
}

print_overall_skipped()
{
	_echo "$MSKIP## [ SKIP ] ##$MEND $TEST_NAME :: $THIS_TEST_NAME :: testcase skipped"
	return 0
}

print_warning()
{
	WARN_COMMENT="$*"
	_echo "$MWARN-- [ WARN ] --$MEND $TEST_NAME :: $THIS_TEST_NAME :: $WARN_COMMENT"
	return 0
}

# this function should skip a testcase if the testsuite is not run in
# a runmode that fits the testcase --> if the suite runs in BASIC mode
# all STANDARD and EXPERIMENTAL testcases will be skipped; if the suite
# runs in STANDARD mode, all EXPERIMENTAL testcases will be skipped and
# if the suite runs in EXPERIMENTAL mode, nothing is skipped
consider_skipping()
{
	TESTCASE_RUNMODE="$1"
	# the runmode of a testcase needs to be at least the current suite's runmode
	if [ $PERFTOOL_TESTSUITE_RUNMODE -lt $TESTCASE_RUNMODE ]; then
		print_overall_skipped
		exit 2
	fi
}

detect_baremetal()
{
	# return values:
	# 0 = bare metal
	# 1 = virtualization detected
	# 2 = unknown state
	VIRT=`systemd-detect-virt 2>/dev/null`
	test $? -eq 127 && return 2
	test "$VIRT" = "none"
}

detect_intel()
{
	# return values:
	# 0 = is Intel
	# 1 = is not Intel or unknown
	grep "vendor_id" < /proc/cpuinfo | grep -q "GenuineIntel"
}

detect_amd()
{
	# return values:
	# 0 = is AMD
	# 1 = is not AMD or unknown
	grep "vendor_id" < /proc/cpuinfo | grep -q "AMD"
}

# base probe utility
check_kprobes_available()
{
	test -e /sys/kernel/debug/tracing/kprobe_events
}

check_uprobes_available()
{
	test -e /sys/kernel/debug/tracing/uprobe_events
}

# Disable every kprobe and uprobe event. The listings name each probe as
# "TYPE:GROUP/EVENT ARGS...", for instance "p:probe/vfs_read vfs_read", and
# events/GROUP/EVENT/enable is the switch for it.
disable_all_probes()
{
	PROBE_SPECS=`cat /sys/kernel/debug/tracing/kprobe_events \
		/sys/kernel/debug/tracing/uprobe_events 2> /dev/null |
		cut -d ' ' -f 1`
	for PROBE_SPEC in $PROBE_SPECS
	do
		case "$PROBE_SPEC" in
			*:*/*) ;;
			*) continue ;;
		esac
		PROBE_ENABLE="/sys/kernel/debug/tracing/events/${PROBE_SPEC#*:}/enable"
		test -e "$PROBE_ENABLE" && echo 0 > "$PROBE_ENABLE"
	done
}

clear_all_probes()
{
	# Disable the probes before removing them. Writing to kprobe_events or
	# uprobe_events is all or nothing: dyn_events_release_all() walks every
	# probe of that type first and returns -EBUSY without removing any of
	# them if it finds one that is still enabled, which would leave stale
	# probes behind to collide with the next run. That covers probes this
	# test suite never created, so disable all of them and not just the
	# ones in the groups perf uses.
	#
	# Only probes are disabled. Writing to events/enable would also silence
	# the tracepoints of any perf record, perf trace or ftrace session
	# sharing the machine, which breaks those tests when they run in
	# parallel with this one.
	disable_all_probes
	check_kprobes_available && echo > /sys/kernel/debug/tracing/kprobe_events
	check_uprobes_available && echo > /sys/kernel/debug/tracing/uprobe_events
}

check_sdt_support()
{
	$CMD_PERF list sdt | grep sdt > /dev/null 2> /dev/null
}
