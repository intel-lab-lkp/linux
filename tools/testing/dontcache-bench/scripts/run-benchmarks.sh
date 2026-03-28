#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Local filesystem I/O mode benchmark suite.
#
# Runs the same test matrix as run-benchmarks.sh but on a local filesystem
# using fio's io_uring engine with the RWF_DONTCACHE flag instead of NFSD's
# debugfs mode knobs.
#
# Usage: ./run-local-benchmarks.sh [options]
#   -t <dir>    Test directory (must be on a filesystem supporting FOP_DONTCACHE)
#   -s <size>   File size (default: auto-sized to exceed RAM)
#   -f <path>   Path to fio binary (default: fio in PATH)
#   -o <dir>    Output directory for results (default: ./results/<timestamp>)
#   -d          Dry run (print commands without executing)

set -euo pipefail

# Defaults
TEST_DIR=""
SIZE=""
FIO_BIN="fio"
RESULTS_DIR=""
DRY_RUN=0
MODES="0 1 2"
PERF_LOCK=0
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIO_JOBS_DIR="${SCRIPT_DIR}/../fio-jobs"

usage() {
	echo "Usage: $0 -t <test-dir> [-s <size>] [-f <fio-path>] [-o <output-dir>] [-D] [-p] [-d]"
	echo ""
	echo "  -t <dir>    Test directory (required, must support RWF_DONTCACHE)"
	echo "  -s <size>   File size (default: 2x RAM)"
	echo "  -f <path>   Path to fio binary (default: fio)"
	echo "  -o <dir>    Output directory (default: ./results/<timestamp>)"
	echo "  -D          Dontcache only (skip buffered and direct tests)"
	echo "  -p          Profile kernel lock contention with perf lock"
	echo "  -d          Dry run"
	exit 1
}

while getopts "t:s:f:o:Dpdh" opt; do
	case $opt in
	t) TEST_DIR="$OPTARG" ;;
	s) SIZE="$OPTARG" ;;
	f) FIO_BIN="$OPTARG" ;;
	o) RESULTS_DIR="$OPTARG" ;;
	D) MODES="1" ;;
	p) PERF_LOCK=1 ;;
	d) DRY_RUN=1 ;;
	h) usage ;;
	*) usage ;;
	esac
done

if [ -z "$TEST_DIR" ]; then
	echo "ERROR: -t <test-dir> is required"
	usage
fi

# Auto-size to 2x RAM if not specified
if [ -z "$SIZE" ]; then
	mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
	SIZE="$(( mem_kb * 2 / 1024 ))M"
fi

if [ -z "$RESULTS_DIR" ]; then
	RESULTS_DIR="./results/local-$(date +%Y%m%d-%H%M%S)"
fi

mkdir -p "$RESULTS_DIR"

log() {
	echo "[$(date '+%H:%M:%S')] $*"
}

run_cmd() {
	if [ "$DRY_RUN" -eq 1 ]; then
		echo "  [DRY RUN] $*"
	else
		"$@"
	fi
}

# I/O mode definitions:
#   buffered:  direct=0, uncached=0
#   dontcache: direct=0, uncached=1
#   direct:    direct=1, uncached=0
#
# Mode name from numeric value
mode_name() {
	case $1 in
	0) echo "buffered" ;;
	1) echo "dontcache" ;;
	2) echo "direct" ;;
	esac
}

# Return fio command-line flags for a given mode.
# "direct" is a standard fio option and works on the command line.
# "uncached" is an io_uring engine option that must be in the job file,
# so we inject it via make_job_file() below.
mode_fio_args() {
	case $1 in
	0) echo "--direct=0" ;;           # buffered
	1) echo "--direct=0" ;;           # dontcache
	2) echo "--direct=1" ;;           # direct
	esac
}

# Return the uncached= value for a given mode.
mode_uncached() {
	case $1 in
	0) echo "0" ;;
	1) echo "1" ;;
	2) echo "0" ;;
	esac
}

# Create a temporary job file with uncached=N injected into [global].
# For uncached=0 (buffered/direct), return the original file unchanged.
make_job_file() {
	local job_file=$1
	local uncached=$2

	if [ "$uncached" -eq 0 ]; then
		echo "$job_file"
		return
	fi

	local tmp
	tmp=$(mktemp)
	sed "/^\[global\]/a uncached=${uncached}" "$job_file" > "$tmp"
	echo "$tmp"
}

drop_caches() {
	run_cmd bash -c "sync && echo 3 > /proc/sys/vm/drop_caches"
}

# perf lock profiling — uses BPF-based live contention tracing
PERF_LOCK_PID=""

start_perf_lock() {
	local outdir=$1

	if [ "$PERF_LOCK" -ne 1 ]; then
		return
	fi

	log "Starting perf lock contention tracing"
	perf lock contention -a -b --max-stack 8 \
		> "${outdir}/perf-lock-contention.txt" 2>&1 &
	PERF_LOCK_PID=$!
}

stop_perf_lock() {
	local outdir=$1

	if [ -z "$PERF_LOCK_PID" ]; then
		return
	fi

	log "Stopping perf lock contention tracing"
	kill -TERM "$PERF_LOCK_PID" 2>/dev/null || true
	wait "$PERF_LOCK_PID" 2>/dev/null || true
	PERF_LOCK_PID=""
}

# Background monitors
VMSTAT_PID=""
IOSTAT_PID=""
MEMINFO_PID=""

start_monitors() {
	local outdir=$1
	log "Starting monitors in $outdir"
	run_cmd vmstat 1 > "${outdir}/vmstat.log" 2>&1 &
	VMSTAT_PID=$!
	run_cmd iostat -x 1 > "${outdir}/iostat.log" 2>&1 &
	IOSTAT_PID=$!
	(while true; do
		echo "=== $(date '+%s') ==="
		cat /proc/meminfo
		sleep 1
	done) > "${outdir}/meminfo.log" 2>&1 &
	MEMINFO_PID=$!
}

stop_monitors() {
	log "Stopping monitors"
	kill "$VMSTAT_PID" "$IOSTAT_PID" "$MEMINFO_PID" 2>/dev/null || true
	wait "$VMSTAT_PID" "$IOSTAT_PID" "$MEMINFO_PID" 2>/dev/null || true
}

cleanup_test_files() {
	local filepath="${TEST_DIR}/$1"
	log "Cleaning up $filepath"
	run_cmd rm -f "$filepath"
}

# Run a single fio benchmark
run_fio() {
	local job_file=$1
	local outdir=$2
	local filename=$3
	local fio_size=${4:-$SIZE}
	local keep=${5:-}
	local extra_args=${6:-}
	local uncached=${7:-0}

	# Inject uncached=N into the job file if needed
	local actual_job
	actual_job=$(make_job_file "$job_file" "$uncached")

	local job_name
	job_name=$(basename "$job_file" .fio)

	log "Running fio job: $job_name -> $outdir (file=${TEST_DIR}/$filename size=$fio_size)"
	mkdir -p "$outdir"

	drop_caches
	start_monitors "$outdir"
	# Skip perf lock profiling for precreate/setup runs
	[ "$keep" != "keep" ] && start_perf_lock "$outdir"

	# shellcheck disable=SC2086
	run_cmd "$FIO_BIN" "$actual_job" \
		--output-format=json \
		--output="${outdir}/${job_name}.json" \
		--filename="${TEST_DIR}/$filename" \
		--size="$fio_size" \
		$extra_args

	[ "$keep" != "keep" ] && stop_perf_lock "$outdir"
	stop_monitors
	log "Finished: $job_name"

	# Clean up temp job file if one was created
	[ "$actual_job" != "$job_file" ] && rm -f "$actual_job"

	if [ "$keep" != "keep" ]; then
		cleanup_test_files "$filename"
	fi
}

########################################################################
# Preflight
########################################################################
preflight() {
	log "=== Preflight checks ==="

	if ! command -v "$FIO_BIN" &>/dev/null; then
		echo "ERROR: fio not found at $FIO_BIN"
		exit 1
	fi

	if [ ! -d "$TEST_DIR" ]; then
		echo "ERROR: Test directory $TEST_DIR does not exist"
		exit 1
	fi

	# Quick check that RWF_DONTCACHE works on this filesystem
	local testfile="${TEST_DIR}/.dontcache_test"
	if ! "$FIO_BIN" --name=test --ioengine=io_uring --rw=write \
		--bs=4k --size=4k --direct=0 --uncached=1 \
		--filename="$testfile" 2>/dev/null; then
		echo "WARNING: RWF_DONTCACHE may not be supported on $TEST_DIR"
		echo "         (filesystem must support FOP_DONTCACHE)"
	fi
	rm -f "$testfile"

	log "Test directory: $TEST_DIR"
	log "File size: $SIZE"
	log "fio binary: $FIO_BIN"
	log "Results: $RESULTS_DIR"

	# Record system info
	{
		echo "Timestamp: $(date +%Y%m%d-%H%M%S)"
		echo "Kernel: $(uname -r)"
		echo "Hostname: $(hostname)"
		echo "Filesystem: $(df -T "$TEST_DIR" | tail -1 | awk '{print $2}')"
		echo "File size: $SIZE"
		echo "Test dir: $TEST_DIR"
	} > "${RESULTS_DIR}/sysinfo.txt"
}

########################################################################
# Deliverable 1: Single-client benchmarks
########################################################################
run_deliverable1() {
	log "=========================================="
	log "Deliverable 1: Single-client benchmarks"
	log "=========================================="

	# Sequential write
	for mode in $MODES; do
		local mname
		mname=$(mode_name $mode)
		local fio_args
		fio_args=$(mode_fio_args $mode)

		drop_caches
		run_fio "${FIO_JOBS_DIR}/seq-write.fio" \
			"${RESULTS_DIR}/seq-write/${mname}" \
			"seq-write_testfile" "$SIZE" "" "$fio_args" \
			"$(mode_uncached $mode)"
	done

	# Random write
	for mode in $MODES; do
		local mname
		mname=$(mode_name $mode)
		local fio_args
		fio_args=$(mode_fio_args $mode)

		drop_caches
		run_fio "${FIO_JOBS_DIR}/rand-write.fio" \
			"${RESULTS_DIR}/rand-write/${mname}" \
			"rand-write_testfile" "$SIZE" "" "$fio_args" \
			"$(mode_uncached $mode)"
	done

	# Sequential read — pre-create file, then read with each mode
	log "Pre-creating sequential read test file"
	run_fio "${FIO_JOBS_DIR}/seq-write.fio" \
		"${RESULTS_DIR}/seq-read/precreate" \
		"seq-read_testfile" "$SIZE" "keep"

	for rmode in $MODES; do
		local mname
		mname=$(mode_name $rmode)
		local fio_args
		fio_args=$(mode_fio_args $rmode)
		local keep="keep"
		[ "$rmode" -eq 2 ] && keep=""

		drop_caches
		run_fio "${FIO_JOBS_DIR}/seq-read.fio" \
			"${RESULTS_DIR}/seq-read/${mname}" \
			"seq-read_testfile" "$SIZE" "$keep" "$fio_args" \
			"$(mode_uncached $rmode)"
	done

	# Random read — pre-create file, then read with each mode
	log "Pre-creating random read test file"
	run_fio "${FIO_JOBS_DIR}/seq-write.fio" \
		"${RESULTS_DIR}/rand-read/precreate" \
		"rand-read_testfile" "$SIZE" "keep"

	for rmode in $MODES; do
		local mname
		mname=$(mode_name $rmode)
		local fio_args
		fio_args=$(mode_fio_args $rmode)
		local keep="keep"
		[ "$rmode" -eq 2 ] && keep=""

		drop_caches
		run_fio "${FIO_JOBS_DIR}/rand-read.fio" \
			"${RESULTS_DIR}/rand-read/${mname}" \
			"rand-read_testfile" "$SIZE" "$keep" "$fio_args" \
			"$(mode_uncached $rmode)"
	done
}

########################################################################
# Deliverable 2: Multi-client tests
########################################################################
run_deliverable2() {
	log "=========================================="
	log "Deliverable 2: Noisy-neighbor benchmarks"
	log "=========================================="

	local num_clients=4
	local client_size
	local mem_kb
	mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
	client_size="$(( mem_kb / 1024 / num_clients ))M"

	# Scenario A: Multiple writers
	for mode in $MODES; do
		local mname
		mname=$(mode_name $mode)
		local fio_args
		fio_args=$(mode_fio_args $mode)

		drop_caches
		run_fio "${FIO_JOBS_DIR}/multi-write.fio" \
			"${RESULTS_DIR}/multi-write/${mname}" \
			"multi-write_testfile" "$client_size" "" "$fio_args" \
			"$(mode_uncached $mode)"
	done

	# Scenario C: Noisy writer + latency-sensitive readers
	for mode in $MODES; do
		local mname
		mname=$(mode_name $mode)
		local fio_args
		fio_args=$(mode_fio_args $mode)
		local uncached
		uncached=$(mode_uncached $mode)
		local writer_job
		writer_job=$(make_job_file "${FIO_JOBS_DIR}/noisy-writer.fio" "$uncached")
		local reader_job
		reader_job=$(make_job_file "${FIO_JOBS_DIR}/lat-reader.fio" "$uncached")
		local outdir="${RESULTS_DIR}/noisy-neighbor/${mname}"
		mkdir -p "$outdir"

		# Pre-create read files
		for i in $(seq 1 $(( num_clients - 1 ))); do
			log "Pre-creating read file for reader $i"
			run_fio "${FIO_JOBS_DIR}/seq-write.fio" \
				"${outdir}/precreate_reader${i}" \
				"reader${i}_readfile" \
				"512M" "keep"
		done
		drop_caches
		start_monitors "$outdir"
		start_perf_lock "$outdir"

		# Noisy writer
		# shellcheck disable=SC2086
		run_cmd "$FIO_BIN" "$writer_job" \
			--output-format=json \
			--output="${outdir}/noisy_writer.json" \
			--filename="${TEST_DIR}/bulk_testfile" \
			--size="$SIZE" \
			$fio_args &
		local writer_pid=$!

		# Latency-sensitive readers
		local reader_pids=()
		for i in $(seq 1 $(( num_clients - 1 ))); do
			# shellcheck disable=SC2086
			run_cmd "$FIO_BIN" "$reader_job" \
				--output-format=json \
				--output="${outdir}/reader${i}.json" \
				--filename="${TEST_DIR}/reader${i}_readfile" \
				--size="512M" \
				$fio_args &
			reader_pids+=($!)
		done

		local rc=0
		wait "$writer_pid" || rc=$?
		for pid in "${reader_pids[@]}"; do
			wait "$pid" || rc=$?
		done

		stop_perf_lock "$outdir"
		stop_monitors
		[ $rc -ne 0 ] && log "WARNING: some fio jobs exited non-zero"

		[ "$writer_job" != "${FIO_JOBS_DIR}/noisy-writer.fio" ] && rm -f "$writer_job"
		[ "$reader_job" != "${FIO_JOBS_DIR}/lat-reader.fio" ] && rm -f "$reader_job"
		cleanup_test_files "bulk_testfile"
		for i in $(seq 1 $(( num_clients - 1 ))); do
			cleanup_test_files "reader${i}_readfile"
		done
	done

	# Scenario D: Mixed-mode noisy neighbor
	# dontcache writes + buffered reads
	local outdir="${RESULTS_DIR}/noisy-neighbor-mixed/dontcache-w_buffered-r"
	mkdir -p "$outdir"
	local writer_job
	writer_job=$(make_job_file "${FIO_JOBS_DIR}/noisy-writer.fio" 1)

	for i in $(seq 1 $(( num_clients - 1 ))); do
		log "Pre-creating read file for reader $i"
		run_fio "${FIO_JOBS_DIR}/seq-write.fio" \
			"${outdir}/precreate_reader${i}" \
			"reader${i}_readfile" \
			"512M" "keep"
	done
	drop_caches
	start_monitors "$outdir"
	start_perf_lock "$outdir"

	# Writer with dontcache
	run_cmd "$FIO_BIN" "$writer_job" \
		--output-format=json \
		--output="${outdir}/noisy_writer.json" \
		--filename="${TEST_DIR}/bulk_testfile" \
		--size="$SIZE" \
		--direct=0 &
	local writer_pid=$!

	# Readers with buffered (no uncached flag)
	local reader_pids=()
	for i in $(seq 1 $(( num_clients - 1 ))); do
		run_cmd "$FIO_BIN" "${FIO_JOBS_DIR}/lat-reader.fio" \
			--output-format=json \
			--output="${outdir}/reader${i}.json" \
			--filename="${TEST_DIR}/reader${i}_readfile" \
			--size="512M" \
			--direct=0 &
		reader_pids+=($!)
	done

	local rc=0
	wait "$writer_pid" || rc=$?
	for pid in "${reader_pids[@]}"; do
		wait "$pid" || rc=$?
	done

	stop_perf_lock "$outdir"
	stop_monitors
	[ $rc -ne 0 ] && log "WARNING: some fio jobs exited non-zero"

	[ "$writer_job" != "${FIO_JOBS_DIR}/noisy-writer.fio" ] && rm -f "$writer_job"
	cleanup_test_files "bulk_testfile"
	for i in $(seq 1 $(( num_clients - 1 ))); do
		cleanup_test_files "reader${i}_readfile"
	done

	# Scenario E: Competing writers (dontcache vs buffered on separate files)
	# This tests whether the dontcache flusher kick interferes with a
	# normal buffered writer sharing the same backing device.
	log "--- Scenario E: Competing writers (separate files) ---"

	# Sub-scenario: dontcache writer vs buffered writer
	local outdir="${RESULTS_DIR}/competing-writers/dontcache-vs-buffered"
	mkdir -p "$outdir"
	local dc_writer_job
	dc_writer_job=$(make_job_file "${FIO_JOBS_DIR}/noisy-writer.fio" 1)

	drop_caches
	start_monitors "$outdir"
	start_perf_lock "$outdir"

	# Writer A: dontcache
	run_cmd "$FIO_BIN" "$dc_writer_job" \
		--output-format=json \
		--output="${outdir}/writer_dontcache.json" \
		--filename="${TEST_DIR}/writer_a_testfile" \
		--size="$SIZE" \
		--direct=0 &
	local writer_a_pid=$!

	# Writer B: buffered
	run_cmd "$FIO_BIN" "${FIO_JOBS_DIR}/noisy-writer.fio" \
		--output-format=json \
		--output="${outdir}/writer_buffered.json" \
		--filename="${TEST_DIR}/writer_b_testfile" \
		--size="$SIZE" \
		--direct=0 &
	local writer_b_pid=$!

	local rc=0
	wait "$writer_a_pid" || rc=$?
	wait "$writer_b_pid" || rc=$?

	stop_perf_lock "$outdir"
	stop_monitors
	[ $rc -ne 0 ] && log "WARNING: some fio jobs exited non-zero"

	[ "$dc_writer_job" != "${FIO_JOBS_DIR}/noisy-writer.fio" ] && rm -f "$dc_writer_job"
	cleanup_test_files "writer_a_testfile"
	cleanup_test_files "writer_b_testfile"

	# Sub-scenario: buffered writer vs buffered writer (baseline)
	outdir="${RESULTS_DIR}/competing-writers/buffered-vs-buffered"
	mkdir -p "$outdir"

	drop_caches
	start_monitors "$outdir"
	start_perf_lock "$outdir"

	# Writer A: buffered
	run_cmd "$FIO_BIN" "${FIO_JOBS_DIR}/noisy-writer.fio" \
		--output-format=json \
		--output="${outdir}/writer_a.json" \
		--filename="${TEST_DIR}/writer_a_testfile" \
		--size="$SIZE" \
		--direct=0 &
	writer_a_pid=$!

	# Writer B: buffered
	run_cmd "$FIO_BIN" "${FIO_JOBS_DIR}/noisy-writer.fio" \
		--output-format=json \
		--output="${outdir}/writer_b.json" \
		--filename="${TEST_DIR}/writer_b_testfile" \
		--size="$SIZE" \
		--direct=0 &
	writer_b_pid=$!

	rc=0
	wait "$writer_a_pid" || rc=$?
	wait "$writer_b_pid" || rc=$?

	stop_perf_lock "$outdir"
	stop_monitors
	[ $rc -ne 0 ] && log "WARNING: some fio jobs exited non-zero"

	cleanup_test_files "writer_a_testfile"
	cleanup_test_files "writer_b_testfile"
}

########################################################################
# Deliverable 3: Axboe 32-file write benchmark
########################################################################
run_deliverable3() {
	log "=========================================="
	log "Deliverable 3: 32-file write (Axboe test)"
	log "=========================================="

	# Per-file size: 2x RAM / 32 so total written exceeds RAM
	local per_file_size
	local mem_kb
	mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
	per_file_size="$(( mem_kb * 2 / 1024 / 32 ))M"

	for mode in $MODES; do
		local mname
		mname=$(mode_name $mode)
		local fio_args
		fio_args=$(mode_fio_args $mode)

		drop_caches
		run_fio "${FIO_JOBS_DIR}/axboe-write.fio" \
			"${RESULTS_DIR}/axboe-write/${mname}" \
			"axboe-write_testfile" "$per_file_size" "" "$fio_args" \
			"$(mode_uncached $mode)"
	done
}

########################################################################
# Main
########################################################################
preflight
run_deliverable1
run_deliverable2
run_deliverable3

log "=========================================="
log "All benchmarks complete."
log "Results in: $RESULTS_DIR"
log "Parse with: scripts/parse-results.sh $RESULTS_DIR"
log "=========================================="
