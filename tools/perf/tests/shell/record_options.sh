#!/bin/bash
# perf record option tests
# SPDX-License-Identifier: GPL-2.0

set -e

temp_dir=$(mktemp -d /tmp/__perf_test_record_opts.XXXXXX)
err=0

cleanup() {
	rm -rf "${temp_dir}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

test_record() {
	local desc="$1" out="$2"

	shift 2
	echo "Record ${desc}"
	if ! perf record -q -e cpu-clock -c 1 "$@" -o "${temp_dir}/${out}" -- sleep 0.1
	then
		echo "Record ${desc} [Failed: perf record failed]"
		err=1
		return
	fi
	if ! perf report -i "${temp_dir}/${out}" -q 2> /dev/null | grep -q sleep
	then
		echo "Record ${desc} [Failed: no samples]"
		err=1
		return
	fi
	echo "Record ${desc} [Success]"
}

test_timestamp_filename() {
	echo "Record timestamp filename"
	if ! perf record -q -e cpu-clock -c 1 --timestamp-filename \
			-o "${temp_dir}/tsfn" -- sleep 0.1
	then
		echo "Record timestamp filename [Failed: perf record failed]"
		err=1
		return
	fi
	if ! ls "${temp_dir}/tsfn".* > /dev/null 2>&1
	then
		echo "Record timestamp filename [Failed: no timestamped file]"
		err=1
		return
	fi
	echo "Record timestamp filename [Success]"
}

test_switch_output() {
	echo "Record switch output"
	if ! perf record -q -e cpu-clock -c 1 --switch-output=size=64K \
			-o "${temp_dir}/sw" -- sleep 0.2
	then
		echo "Record switch output [Failed: perf record failed]"
		err=1
		return
	fi
	if ! ls "${temp_dir}/sw".* > /dev/null 2>&1
	then
		echo "Record switch output [Failed: no switched files]"
		err=1
		return
	fi
	echo "Record switch output [Success]"
}

test_clockid() {
	echo "Record clockid"
	if ! perf record -q -e cpu-clock -c 1 -k CLOCK_MONOTONIC \
			-o "${temp_dir}/clock" -- sleep 0.1
	then
		echo "Record clockid [Failed: perf record failed]"
		err=1
		return
	fi
	if ! perf script -i "${temp_dir}/clock" -F tod 2> /dev/null | \
			grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}'
	then
		echo "Record clockid [Failed: no wall clock timestamps]"
		err=1
		return
	fi
	echo "Record clockid [Success]"
}

test_record_option() {
	local desc="$1"

	shift 1
	echo "Record option ${desc}"
	rm -f "${temp_dir}/${desc}.data"
	if ! perf record -q -e cpu-clock -c 1 "$@" -o "${temp_dir}/${desc}.data" -- sleep 0.1
	then
		echo "Record option ${desc} [Failed: perf record failed]"
		err=1
		return
	fi
	if [ ! -s "${temp_dir}/${desc}.data" ]
	then
		echo "Record option ${desc} [Failed: no data recorded]"
		err=1
		return
	fi
	echo "Record option ${desc} [Success]"
}

test_dry_run() {
	echo "Record option dry run"
	if ! perf record -q -e cpu-clock -c 1 --dry-run \
			-o "${temp_dir}/dry-run.data" -- sleep 0.1
	then
		echo "Record option dry run [Failed: perf record failed]"
		err=1
		return
	fi
	if [ -e "${temp_dir}/dry-run.data" ]
	then
		echo "Record option dry run [Failed: recorded data]"
		err=1
		return
	fi
	echo "Record option dry run [Success]"
}

test_record "aio" "aio" --aio=2
test_record "threads core" "threads-core" --threads=core
test_record "threads package" "threads-package" --threads=package
test_record "threads numa" "threads-numa" --threads=numa
test_record "affinity" "affinity" --affinity=cpu
test_record "mmap flush" "mmap-flush" --mmap-flush=1
test_record "max size" "max-size" --max-size=1M
test_record "timestamp boundary" "ts-boundary" --timestamp-boundary
test_record "user callchains" "callchains" --user-callchains --call-graph fp
test_timestamp_filename
test_switch_output
test_clockid
test_dry_run
test_record_option no-samples -n
test_record_option buildid-all --buildid-all
test_record_option buildid-mmap --buildid-mmap
test_record_option synth-no --synth no
test_record_option synth-task --synth task
test_record_option synth-mmap --synth mmap
test_record_option sample-cpu --sample-cpu
test_record_option weight -W
test_record_option raw-samples -R
test_record_option no-buffering --no-buffering
test_record_option no-bpf-event --no-bpf-event
test_record_option proc-map-timeout --proc-map-timeout 100
test_record_option num-thread-synthesize --num-thread-synthesize 1
test_record_option aio-one --aio=1
test_record_option affinity-node --affinity=node
test_record_option delay -D 10
test_record_option tail-synthesize --tail-synthesize
test_record_option overwrite --overwrite
test_record_option data -d
test_record_option mmap-pages -m 4,8
test_record_option clockid-monotonic-raw -k CLOCK_MONOTONIC_RAW

cleanup
exit $err
