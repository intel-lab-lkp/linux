#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# perf script tests

set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/perf_has_symbol.sh
. "${shelldir}"/lib/perf_has_symbol.sh

# perf script -l reads the scripts directory from the perf exec path,
# which in a build tree points at a prefix that is not installed
if [ -z "${PERF_EXEC_PATH}" ] && [ -d "${shelldir}"/../../python ]; then
	PERF_EXEC_PATH=$(cd "${shelldir}"/../.. && pwd)
	export PERF_EXEC_PATH
fi

testprog="perf test -w thloop"
testsym="test_loop"

skip_test_missing_symbol "${testsym}"

temp_dir=$(mktemp -d /tmp/perf-test-script.XXXXXXXXXX)

err=0

cleanup()
{
	trap - EXIT TERM INT
	sane=$(echo "${temp_dir}" | cut -b 1-21)
	if [ "${sane}" = "/tmp/perf-test-script" ] ; then
		echo "--- Cleaning up ---"
		rm -rf "${temp_dir:?}/"*
		rmdir "${temp_dir}"
	fi
}

trap_cleanup()
{
	cleanup
	exit 1
}

trap trap_cleanup EXIT TERM INT

script_data="${temp_dir}/script-perf.data"

test_script_list()
{
	echo "perf script list test"
	if ! perf script -l | grep -q "List of available scripts"
	then
		echo "perf script list test [Failed missing list header]"
		err=1
		return
	fi
	if ! perf script -l | grep -q "export-to-sqlite"
	then
		echo "perf script list test [Failed missing export-to-sqlite]"
		err=1
	fi
}

test_script_fields()
{
	echo "perf script fields test"
	perf record -o "${script_data}" -e cpu-clock --sample-cpu ${testprog}
	if ! perf script -i "${script_data}" -F comm,tid,pid,time,cpu,event,ip,sym,dso,dsoff,symoff,period,machine_pid,vcpu,ipc,insn,insnlen,disasm,misc,metric,synth,callindent,flags | grep -q "${testsym}"
	then
		echo "perf script fields test [Failed missing ${testsym}]"
		err=1
		return
	fi
	if ! perf script -i "${script_data}" -F +cpu | grep -q "${testsym}"
	then
		echo "perf script fields test [Failed to add the cpu field]"
		err=1
	fi
	if ! perf script -i "${script_data}" -F -comm | grep -q "${testsym}"
	then
		echo "perf script fields test [Failed to remove the comm field]"
		err=1
	fi
}

test_script_per_event_dump()
{
	echo "perf script per-event dump test"
	if ! perf script -i "${script_data}" --per-event-dump > /dev/null
	then
		echo "perf script per-event dump test [Failed to write the dumps]"
		err=1
		return
	fi
	if ! grep -q "${testsym}" "${script_data}".*.dump
	then
		echo "perf script per-event dump test [Failed missing ${testsym}]"
		err=1
	fi
}

test_script_header()
{
	echo "perf script header test"
	if ! perf script -i "${script_data}" --header | grep -q "captured on"
	then
		echo "perf script header test [Failed missing the header]"
		err=1
		return
	fi
	if ! perf script -i "${script_data}" --header-only | grep -q "captured on"
	then
		echo "perf script header test [Failed missing the header only output]"
		err=1
	fi
}

test_script_events()
{
	echo "perf script events test"
	if ! perf script -i "${script_data}" --show-task-events --show-mmap-events | grep -q "PERF_RECORD_"
	then
		echo "perf script events test [Failed missing the events]"
		err=1
	fi
}

test_script_misc()
{
	echo "perf script misc options test"
	for opt in --reltime --deltatime -I -L --ns
	do
		if ! perf script -i "${script_data}" ${opt} | grep -q "${testsym}"
		then
			echo "perf script misc options test [Failed ${opt}]"
			err=1
		fi
	done
	if ! perf script -i "${script_data}" -D | grep -q "PERF_RECORD_SAMPLE"
	then
		echo "perf script misc options test [Failed --dump-raw-trace]"
		err=1
	fi
}

test_parallel_perf()
{
	echo "parallel-perf test"
	if ! python3 --version >/dev/null 2>&1 ; then
		echo "SKIP: no python3"
		# Don't turn a failure in the tests above into a skip
		if [ ${err} -eq 0 ]; then
			err=2
		fi
		return
	fi
	pp=$(dirname "$0")/../../python/parallel-perf.py
	if [ ! -f "${pp}" ] ; then
		echo "SKIP: parallel-perf.py script not found "
		if [ ${err} -eq 0 ]; then
			err=2
		fi
		return
	fi
	perf_data="${temp_dir}/pp-perf.data"
	output1_dir="${temp_dir}/output1"
	output2_dir="${temp_dir}/output2"
	perf record -o "${perf_data}" --sample-cpu uname
	python3 "${pp}" -o "${output1_dir}" --jobs 4 --verbose -- perf script -i "${perf_data}"
	python3 "${pp}" -o "${output2_dir}" --jobs 4 --verbose --per-cpu -- perf script -i "${perf_data}"
	echo "parallel-perf test [Success]"
}

test_script_list
test_script_fields
test_script_per_event_dump
test_script_header
test_script_events
test_script_misc
test_parallel_perf

cleanup

exit $err
