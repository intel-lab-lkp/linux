#!/bin/bash
# perf report option tests
# SPDX-License-Identifier: GPL-2.0

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/perf_has_symbol.sh
. "${shelldir}"/lib/perf_has_symbol.sh

testprog="perf test -w thloop"
testsym="test_loop"

skip_test_missing_symbol "${testsym}"

perfdata=$(mktemp /tmp/__perf_test_report_opts.perf.data.XXXXX)
cgdata=$(mktemp /tmp/__perf_test_report_opts.cg.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test_report_opts.perf.out.XXXXX)
err=0

cleanup() {
	rm -f "${perfdata}" "${perfdata}".old
	rm -f "${cgdata}" "${cgdata}".old
	rm -f "${perfout}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

check_report() {
	local desc="$1" expect="$2"

	shift 2
	echo "Report ${desc}"
	if ! perf report -i "${perfdata}" --stdio "$@" > "${perfout}" 2>&1
	then
		echo "Report ${desc} [Failed: perf report failed]"
		err=1
		return
	fi
	if ! grep -qE "${expect}" "${perfout}"
	then
		echo "Report ${desc} [Failed: missing '${expect}']"
		cat "${perfout}"
		err=1
		return
	fi
	echo "Report ${desc} [Success]"
}

# The call graph data, for --no-children: 'absent', when set, must not
# be found in the output
check_cg_report() {
	local desc="$1" expect="$2" absent="$3"

	shift 3
	echo "Report ${desc}"
	if ! perf report -i "${cgdata}" --stdio "$@" > "${perfout}" 2>&1
	then
		echo "Report ${desc} [Failed: perf report failed]"
		err=1
		return
	fi
	if [ -n "${absent}" ] && grep -qE "${absent}" "${perfout}"
	then
		echo "Report ${desc} [Failed: still has '${absent}']"
		err=1
		return
	fi
	if ! grep -qE "${expect}" "${perfout}"
	then
		echo "Report ${desc} [Failed: missing '${expect}']"
		cat "${perfout}"
		err=1
		return
	fi
	echo "Report ${desc} [Success]"
}

perf record -q -e cpu-clock --sample-cpu -o "${perfdata}" ${testprog}

# -n adds a Samples column, -t, joins the header fields with commas,
# -w pads them to the given width.  -U, --percentage, --inline and
# --full-source-path change nothing in this output: every entry
# resolves to a symbol, nothing is filtered and no source lines are
# printed, so they run to check that perf takes them and still prints
# the table.
check_report "nr samples" "Overhead[[:space:]]+Samples" -n
check_report "hide unresolved" "^# Overhead" -U
check_report "fields" "^ *perf " -F comm,dso,symbol,period
check_report "percentage" "^# Overhead" --percentage absolute
check_report "field separator" "^# Overhead," -t,
check_report "inline" "^# Overhead" --inline
check_report "full source path" "^# Overhead" --full-source-path
check_report "column widths" "^# {2,}Overhead" -w 20,20,20,20,20,20

# --disable-order is rejected together with a latency field, which
# tells that the option reached the event ordering code
echo "Report disable order"
if ! perf report -i "${perfdata}" --stdio -F latency --disable-order 2>&1 | \
	grep -q "incompatible with --disable-order"
then
	echo "Report disable order [Failed: missing the rejection]"
	err=1
else
	echo "Report disable order [Success]"
fi

# Recording call graphs adds a Children column, --no-children must
# remove it while keeping the entries
perf record -q --call-graph fp -e cpu-clock --sample-cpu -o "${cgdata}" ${testprog}
check_cg_report "children column" "Children" ""
check_cg_report "no children" "${testsym}" "Children" --no-children

cleanup
exit $err
