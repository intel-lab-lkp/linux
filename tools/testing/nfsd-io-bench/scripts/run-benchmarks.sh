#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# NFS server I/O mode benchmark suite
#
# Runs fio with the NFS ioengine against an NFS server on localhost,
# testing buffered, dontcache, and direct I/O modes.
#
# Usage: ./run-benchmarks.sh [OPTIONS]
#
# Options:
#   -e EXPORT_PATH   Server export path (default: /export)
#   -s SIZE          fio file size, should be >= 2x RAM (default: auto-detect)
#   -r RESULTS_DIR   Where to store results (default: ./results)
#   -n NFS_VER       NFS version: 3 or 4 (default: 3)
#   -j FIO_JOBS_DIR  Path to fio job files (default: ../fio-jobs)
#   -d               Dry run: print commands without executing
#   -h               Show this help

set -euo pipefail

# Defaults
EXPORT_PATH="/export"
SIZE=""
RESULTS_DIR="./results"
NFS_VER=3
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIO_JOBS_DIR="${SCRIPT_DIR}/../fio-jobs"
DRY_RUN=0
MODES="0 1 2"
PERF_LOCK=0

DEBUGFS_BASE="/sys/kernel/debug/nfsd"
IO_CACHE_READ="${DEBUGFS_BASE}/io_cache_read"
IO_CACHE_WRITE="${DEBUGFS_BASE}/io_cache_write"
DISABLE_SPLICE="${DEBUGFS_BASE}/disable-splice-read"

usage() {
	echo "Usage: $0 [OPTIONS]"
	echo "  -e EXPORT_PATH   Server export path (default: /export)"
	echo "  -s SIZE          fio file size (default: 2x RAM)"
	echo "  -r RESULTS_DIR   Results directory (default: ./results)"
	echo "  -n NFS_VER       NFS version: 3 or 4 (default: 3)"
	echo "  -j FIO_JOBS_DIR  Path to fio job files"
	echo "  -D               Dontcache only (skip buffered and direct tests)"
	echo "  -p               Profile kernel lock contention with perf lock"
	echo "  -d               Dry run"
	echo "  -h               Help"
	exit 1
}

while getopts "e:s:r:n:j:Dpdh" opt; do
	case $opt in
	e) EXPORT_PATH="$OPTARG" ;;
	s) SIZE="$OPTARG" ;;
	r) RESULTS_DIR="$OPTARG" ;;
	n) NFS_VER="$OPTARG" ;;
	j) FIO_JOBS_DIR="$OPTARG" ;;
	D) MODES="1" ;;
	p) PERF_LOCK=1 ;;
	d) DRY_RUN=1 ;;
	h) usage ;;
	*) usage ;;
	esac
done

# Auto-detect size: 2x total RAM
if [ -z "$SIZE" ]; then
	MEM_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
	MEM_GB=$(( MEM_KB / 1024 / 1024 ))
	SIZE="$(( MEM_GB * 2 ))G"
	echo "Auto-detected RAM: ${MEM_GB}G, using file size: ${SIZE}"
fi


log() {
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

run_cmd() {
	if [ "$DRY_RUN" -eq 1 ]; then
		echo "  [DRY RUN] $*"
	else
		"$@"
	fi
}

# Preflight checks
preflight() {
	log "=== Preflight checks ==="

	if ! command -v fio &>/dev/null; then
		echo "ERROR: fio not found in PATH"
		exit 1
	fi

	# Check fio has nfs ioengine
	if ! fio --enghelp=nfs &>/dev/null; then
		echo "ERROR: fio does not have the nfs ioengine (needs libnfs)"
		exit 1
	fi

	# Check debugfs knobs exist
	for knob in "$IO_CACHE_READ" "$IO_CACHE_WRITE" "$DISABLE_SPLICE"; do
		if [ ! -f "$knob" ]; then
			echo "ERROR: $knob not found. Is the kernel new enough?"
			exit 1
		fi
	done

	# Check NFS server is exporting
	if ! showmount -e localhost 2>/dev/null | grep -q "$EXPORT_PATH"; then
		echo "WARNING: $EXPORT_PATH not in showmount output, proceeding anyway"
	fi

	# Print system info
	echo "Kernel:     $(uname -r)"
	echo "RAM:        $(awk '/MemTotal/ {printf "%.1f GB", $2/1024/1024}' /proc/meminfo)"
	echo "Export:     $EXPORT_PATH"
	echo "NFS ver:    $NFS_VER"
	echo "File size:  $SIZE"
	echo "Results:    $RESULTS_DIR"
	echo ""
}

# Set server I/O mode via debugfs
set_io_mode() {
	local cache_write=$1
	local cache_read=$2
	local splice_off=$3

	log "Setting io_cache_write=$cache_write io_cache_read=$cache_read disable-splice-read=$splice_off"
	run_cmd bash -c "echo $cache_write > $IO_CACHE_WRITE"
	run_cmd bash -c "echo $cache_read  > $IO_CACHE_READ"
	run_cmd bash -c "echo $splice_off  > $DISABLE_SPLICE"
}

# Drop page cache on server
drop_caches() {
	log "Dropping page cache"
	run_cmd bash -c "sync && echo 3 > /proc/sys/vm/drop_caches"
	sleep 1
}

# Start background server monitoring
start_monitors() {
	local outdir=$1

	log "Starting server monitors in $outdir"
	run_cmd vmstat 1 > "${outdir}/vmstat.log" 2>&1 &
	VMSTAT_PID=$!

	run_cmd iostat -x 1 > "${outdir}/iostat.log" 2>&1 &
	IOSTAT_PID=$!

	# Sample /proc/meminfo every second
	(while true; do
		echo "=== $(date '+%s') ==="
		cat /proc/meminfo
		sleep 1
	done) > "${outdir}/meminfo.log" 2>&1 &
	MEMINFO_PID=$!
}

# Stop background monitors
stop_monitors() {
	log "Stopping monitors"
	kill "$VMSTAT_PID" "$IOSTAT_PID" "$MEMINFO_PID" 2>/dev/null || true
	wait "$VMSTAT_PID" "$IOSTAT_PID" "$MEMINFO_PID" 2>/dev/null || true
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

# Run a single fio benchmark.
# nfs_url is set in the job files; we pass --filename and --size on
# the command line to vary the target file and data volume per run.
# Pass "keep" as 5th arg to preserve the test file after the run.
run_fio() {
	local job_file=$1
	local outdir=$2
	local filename=$3
	local fio_size=${4:-$SIZE}
	local keep=${5:-}

	local job_name
	job_name=$(basename "$job_file" .fio)

	log "Running fio job: $job_name -> $outdir (file=$filename size=$fio_size)"
	mkdir -p "$outdir"

	drop_caches
	start_monitors "$outdir"
	# Skip perf lock profiling for precreate/setup runs
	[ "$keep" != "keep" ] && start_perf_lock "$outdir"

	run_cmd fio "$job_file" \
		--output-format=json \
		--output="${outdir}/${job_name}.json" \
		--filename="$filename" \
		--size="$fio_size"

	[ "$keep" != "keep" ] && stop_perf_lock "$outdir"
	stop_monitors

	log "Finished: $job_name"

	# Clean up test file to free disk space unless told to keep it
	if [ "$keep" != "keep" ]; then
		cleanup_test_files "$filename"
	fi
}

# Remove test files from the export to free disk space
cleanup_test_files() {
	local filename
	for filename in "$@"; do
		local filepath="${EXPORT_PATH}/${filename}"
		log "Cleaning up: $filepath"
		run_cmd rm -f "$filepath"
	done
}

# Ensure parent directories exist under the export for a given filename
ensure_export_dirs() {
	local filename
	for filename in "$@"; do
		local dirpath="${EXPORT_PATH}/$(dirname "$filename")"
		if [ "$dirpath" != "${EXPORT_PATH}/." ] && [ ! -d "$dirpath" ]; then
			log "Creating directory: $dirpath"
			run_cmd mkdir -p "$dirpath"
		fi
	done
}

# Mode name from numeric value
mode_name() {
	case $1 in
	0) echo "buffered" ;;
	1) echo "dontcache" ;;
	2) echo "direct" ;;
	esac
}

########################################################################
# Deliverable 1: Single-client fio benchmarks
########################################################################
run_deliverable1() {
	log "=========================================="
	log "Deliverable 1: Single-client fio benchmarks"
	log "=========================================="

	# Write test matrix:
	# mode 0 (buffered):    splice on  (default)
	# mode 1 (dontcache):   splice off (required)
	# mode 2 (direct):      splice off (required)

	# Sequential write
	for wmode in $MODES; do
		local mname
		mname=$(mode_name $wmode)
		local splice_off=0
		[ "$wmode" -ne 0 ] && splice_off=1

		drop_caches
		set_io_mode "$wmode" 0 "$splice_off"
		run_fio "${FIO_JOBS_DIR}/seq-write.fio" \
			"${RESULTS_DIR}/seq-write/${mname}" \
			"seq-write_testfile"
	done

	# Random write
	for wmode in $MODES; do
		local mname
		mname=$(mode_name $wmode)
		local splice_off=0
		[ "$wmode" -ne 0 ] && splice_off=1

		drop_caches
		set_io_mode "$wmode" 0 "$splice_off"
		run_fio "${FIO_JOBS_DIR}/rand-write.fio" \
			"${RESULTS_DIR}/rand-write/${mname}" \
			"rand-write_testfile"
	done

	# Sequential read — vary read mode, write stays buffered
	# Pre-create the file for reading
	log "Pre-creating sequential read test file"
	set_io_mode 0 0 0
	run_fio "${FIO_JOBS_DIR}/seq-write.fio" \
		"${RESULTS_DIR}/seq-read/precreate" \
		"seq-read_testfile" "$SIZE" "keep"

	# shellcheck disable=SC2086
	local last_mode
	last_mode=$(echo $MODES | awk '{print $NF}')

	for rmode in $MODES; do
		local mname
		mname=$(mode_name $rmode)
		local splice_off=0
		[ "$rmode" -ne 0 ] && splice_off=1
		# Keep file for subsequent modes; clean up after last
		local keep="keep"
		[ "$rmode" = "$last_mode" ] && keep=""

		drop_caches
		set_io_mode 0 "$rmode" "$splice_off"
		run_fio "${FIO_JOBS_DIR}/seq-read.fio" \
			"${RESULTS_DIR}/seq-read/${mname}" \
			"seq-read_testfile" "$SIZE" "$keep"
	done

	# Random read — vary read mode, write stays buffered
	# Pre-create the file for reading
	log "Pre-creating random read test file"
	set_io_mode 0 0 0
	run_fio "${FIO_JOBS_DIR}/seq-write.fio" \
		"${RESULTS_DIR}/rand-read/precreate" \
		"rand-read_testfile" "$SIZE" "keep"

	for rmode in $MODES; do
		local mname
		mname=$(mode_name $rmode)
		local splice_off=0
		[ "$rmode" -ne 0 ] && splice_off=1
		# Keep file for subsequent modes; clean up after last
		local keep="keep"
		[ "$rmode" = "$last_mode" ] && keep=""

		drop_caches
		set_io_mode 0 "$rmode" "$splice_off"
		run_fio "${FIO_JOBS_DIR}/rand-read.fio" \
			"${RESULTS_DIR}/rand-read/${mname}" \
			"rand-read_testfile" "$SIZE" "$keep"
	done
}

########################################################################
# Deliverable 2: Multi-client (simulated with multiple fio jobs)
########################################################################
run_deliverable2() {
	log "=========================================="
	log "Deliverable 2: Noisy-neighbor benchmarks"
	log "=========================================="

	local num_clients=4
	local client_size
	local mem_kb
	mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
	# Each client gets RAM/num_clients so total > RAM
	client_size="$(( mem_kb / 1024 / num_clients ))M"

	# Scenario A: Multiple writers
	for mode in $MODES; do
		local mname
		mname=$(mode_name $mode)
		local splice_off=0
		[ "$mode" -ne 0 ] && splice_off=1
		local outdir="${RESULTS_DIR}/multi-write/${mname}"
		mkdir -p "$outdir"

		set_io_mode "$mode" "$mode" "$splice_off"
		drop_caches

		# Ensure client directories exist on export
		for i in $(seq 1 $num_clients); do
			ensure_export_dirs "client${i}/testfile"
		done

		start_monitors "$outdir"
		start_perf_lock "$outdir"

		# Launch N parallel fio writers
		local pids=()
		for i in $(seq 1 $num_clients); do
			run_cmd fio "${FIO_JOBS_DIR}/multi-write.fio" \
				--output-format=json \
				--output="${outdir}/client${i}.json" \
				--filename="client${i}/testfile" \
				--size="$client_size" &
			pids+=($!)
		done

		# Wait for all
		local rc=0
		for pid in "${pids[@]}"; do
			wait "$pid" || rc=$?
		done

		stop_perf_lock "$outdir"
		stop_monitors
		[ $rc -ne 0 ] && log "WARNING: some fio jobs exited non-zero"

		# Clean up test files
		for i in $(seq 1 $num_clients); do
			cleanup_test_files "client${i}/testfile"
		done
	done

	# Scenario C: Noisy writer + latency-sensitive readers
	for mode in $MODES; do
		local mname
		mname=$(mode_name $mode)
		local splice_off=0
		[ "$mode" -ne 0 ] && splice_off=1
		local outdir="${RESULTS_DIR}/noisy-neighbor/${mname}"
		mkdir -p "$outdir"

		set_io_mode "$mode" "$mode" "$splice_off"
		drop_caches

		# Pre-create read files for latency readers
		for i in $(seq 1 $(( num_clients - 1 ))); do
			ensure_export_dirs "reader${i}/readfile"
			log "Pre-creating read file for reader $i"
			run_fio "${FIO_JOBS_DIR}/multi-write.fio" \
				"${outdir}/precreate_reader${i}" \
				"reader${i}/readfile" \
				"512M" "keep"
		done
		drop_caches
		ensure_export_dirs "bulk/testfile"
		start_monitors "$outdir"
		start_perf_lock "$outdir"

		# Noisy writer
		run_cmd fio "${FIO_JOBS_DIR}/noisy-writer.fio" \
			--output-format=json \
			--output="${outdir}/noisy_writer.json" \
			--filename="bulk/testfile" \
			--size="$SIZE" &
		local writer_pid=$!

		# Latency-sensitive readers
		local reader_pids=()
		for i in $(seq 1 $(( num_clients - 1 ))); do
			run_cmd fio "${FIO_JOBS_DIR}/lat-reader.fio" \
				--output-format=json \
				--output="${outdir}/reader${i}.json" \
				--filename="reader${i}/readfile" \
				--size="512M" &
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

		# Clean up test files
		cleanup_test_files "bulk/testfile"
		for i in $(seq 1 $(( num_clients - 1 ))); do
			cleanup_test_files "reader${i}/readfile"
		done
	done
	# Scenario D: Mixed-mode noisy neighbor
	# Test write/read mode combinations where the writer uses a
	# cache-friendly mode and readers use buffered reads to benefit
	# from warm cache.
	local mixed_modes=(
		# write_mode read_mode label
		"1 0 dontcache-w_buffered-r"
	)

	for combo in "${mixed_modes[@]}"; do
		local wmode rmode label
		read -r wmode rmode label <<< "$combo"
		local splice_off=0
		[ "$wmode" -ne 0 ] && splice_off=1
		local outdir="${RESULTS_DIR}/noisy-neighbor-mixed/${label}"
		mkdir -p "$outdir"

		set_io_mode "$wmode" "$rmode" "$splice_off"
		drop_caches

		# Pre-create read files for latency readers
		for i in $(seq 1 $(( num_clients - 1 ))); do
			ensure_export_dirs "reader${i}/readfile"
			log "Pre-creating read file for reader $i"
			run_fio "${FIO_JOBS_DIR}/multi-write.fio" \
				"${outdir}/precreate_reader${i}" \
				"reader${i}/readfile" \
				"512M" "keep"
		done
		drop_caches
		ensure_export_dirs "bulk/testfile"
		start_monitors "$outdir"
		start_perf_lock "$outdir"

		# Noisy writer
		run_cmd fio "${FIO_JOBS_DIR}/noisy-writer.fio" \
			--output-format=json \
			--output="${outdir}/noisy_writer.json" \
			--filename="bulk/testfile" \
			--size="$SIZE" &
		local writer_pid=$!

		# Latency-sensitive readers
		local reader_pids=()
		for i in $(seq 1 $(( num_clients - 1 ))); do
			run_cmd fio "${FIO_JOBS_DIR}/lat-reader.fio" \
				--output-format=json \
				--output="${outdir}/reader${i}.json" \
				--filename="reader${i}/readfile" \
				--size="512M" &
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

		# Clean up test files
		cleanup_test_files "bulk/testfile"
		for i in $(seq 1 $(( num_clients - 1 ))); do
			cleanup_test_files "reader${i}/readfile"
		done
	done
}

########################################################################
# Main
########################################################################
preflight

TIMESTAMP=$(date '+%Y%m%d-%H%M%S')
RESULTS_DIR="${RESULTS_DIR}/${TIMESTAMP}"
mkdir -p "$RESULTS_DIR"

# Save system info
{
	echo "Timestamp: $TIMESTAMP"
	echo "Kernel: $(uname -r)"
	echo "Hostname: $(hostname)"
	echo "NFS version: $NFS_VER"
	echo "File size: $SIZE"
	echo "Export: $EXPORT_PATH"
	cat /proc/meminfo
} > "${RESULTS_DIR}/sysinfo.txt"

log "Results will be saved to: $RESULTS_DIR"

run_deliverable1
run_deliverable2

# Reset to defaults
set_io_mode 0 0 0

log "=========================================="
log "All benchmarks complete."
log "Results in: $RESULTS_DIR"
log "Run: scripts/parse-results.sh $RESULTS_DIR"
log "=========================================="
