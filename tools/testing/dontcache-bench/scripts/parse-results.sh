#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Parse fio JSON output and generate comparison tables.
#
# Usage: ./parse-results.sh <results-dir>

set -euo pipefail

if [ $# -lt 1 ]; then
	echo "Usage: $0 <results-dir>"
	exit 1
fi

RESULTS_DIR="$1"

if ! command -v jq &>/dev/null; then
	echo "ERROR: jq is required"
	exit 1
fi

# Extract metrics from a single fio JSON result
extract_metrics() {
	local json_file=$1
	local rw_type=$2  # read or write

	if [ ! -f "$json_file" ]; then
		echo "N/A N/A N/A N/A N/A N/A"
		return
	fi

	jq -r --arg rw "$rw_type" '
		.jobs[0][$rw] as $d |
		[
			(($d.bw // 0) / 1024 | . * 10 | round / 10),    # MB/s
			($d.iops // 0),                                    # IOPS
			((($d.clat_ns.mean // 0) / 1000) | . * 10 | round / 10), # avg lat us
			(($d.clat_ns.percentile["50.000000"] // 0) / 1000), # p50 us
			(($d.clat_ns.percentile["99.000000"] // 0) / 1000), # p99 us
			(($d.clat_ns.percentile["99.900000"] // 0) / 1000)  # p99.9 us
		] | @tsv
	' "$json_file" 2>/dev/null || echo "N/A N/A N/A N/A N/A N/A"
}

# Extract server CPU from vmstat log (average sys%)
extract_cpu() {
	local vmstat_log=$1
	if [ ! -f "$vmstat_log" ]; then
		echo "N/A"
		return
	fi
	# vmstat columns: us sy id wa st — skip header lines
	awk 'NR>2 {sum+=$14; n++} END {if(n>0) printf "%.1f", sum/n; else print "N/A"}' \
		"$vmstat_log" 2>/dev/null || echo "N/A"
}

# Extract peak dirty pages from meminfo log
extract_peak_dirty() {
	local meminfo_log=$1
	if [ ! -f "$meminfo_log" ]; then
		echo "N/A"
		return
	fi
	grep "^Dirty:" "$meminfo_log" | awk '{print $2}' | sort -n | tail -1 || echo "N/A"
}

# Extract peak cached from meminfo log
extract_peak_cached() {
	local meminfo_log=$1
	if [ ! -f "$meminfo_log" ]; then
		echo "N/A"
		return
	fi
	grep "^Cached:" "$meminfo_log" | awk '{print $2}' | sort -n | tail -1 || echo "N/A"
}

print_separator() {
	printf '%*s\n' 120 '' | tr ' ' '-'
}

########################################################################
# Deliverable 1: Single-client results
########################################################################
echo ""
echo "=================================================================="
echo "  Deliverable 1: Single-Client fio Benchmarks"
echo "=================================================================="
echo ""

for workload in seq-write rand-write seq-read rand-read; do
	case $workload in
	seq-write|rand-write) rw_type="write" ;;
	seq-read|rand-read)   rw_type="read" ;;
	esac

	echo "--- $workload ---"
	printf "%-16s %10s %10s %10s %10s %10s %10s %10s %12s %12s\n" \
		"Mode" "MB/s" "IOPS" "Avg(us)" "p50(us)" "p99(us)" "p99.9(us)" "Sys CPU%" "PeakDirty(kB)" "PeakCache(kB)"
	print_separator

	for mode in buffered dontcache direct; do
		dir="${RESULTS_DIR}/${workload}/${mode}"
		json_file=$(find "$dir" -name '*.json' -not -name 'client*' 2>/dev/null | head -1 || true)
		if [ -z "$json_file" ]; then
			printf "%-16s %10s\n" "$mode" "(no data)"
			continue
		fi

		read -r mbps iops avg_lat p50 p99 p999 <<< \
			"$(extract_metrics "$json_file" "$rw_type")"
		cpu=$(extract_cpu "${dir}/vmstat.log")
		dirty=$(extract_peak_dirty "${dir}/meminfo.log")
		cached=$(extract_peak_cached "${dir}/meminfo.log")

		printf "%-16s %10s %10s %10s %10s %10s %10s %10s %12s %12s\n" \
			"$mode" "$mbps" "$iops" "$avg_lat" "$p50" "$p99" "$p999" \
			"$cpu" "${dirty:-N/A}" "${cached:-N/A}"
	done
	echo ""
done

########################################################################
# Deliverable 2: Multi-client results
########################################################################
echo "=================================================================="
echo "  Deliverable 2: Noisy-Neighbor Benchmarks"
echo "=================================================================="
echo ""

# Scenario A: Multiple writers
echo "--- Scenario A: Multiple Writers ---"
for mode in buffered dontcache direct; do
	dir="${RESULTS_DIR}/multi-write/${mode}"
	if [ ! -d "$dir" ]; then
		continue
	fi

	json_file=$(find "$dir" -name '*.json' 2>/dev/null | head -1 || true)
	if [ -z "$json_file" ] || [ ! -f "$json_file" ]; then
		echo "  Mode: $mode (no data)"
		continue
	fi

	echo "  Mode: $mode"
	printf "  %-10s %10s %10s %10s %10s %10s %10s\n" \
		"Job" "MB/s" "IOPS" "Avg(us)" "p50(us)" "p99(us)" "p99.9(us)"

	# Parse per-job stats from the single fio JSON output
	jq -r '.jobs[] |
		[
			.jobname,
			((.write.bw // 0) / 1024 | . * 10 | round / 10),
			(.write.iops // 0),
			(((.write.clat_ns.mean // 0) / 1000) | . * 10 | round / 10),
			((.write.clat_ns.percentile["50.000000"] // 0) / 1000),
			((.write.clat_ns.percentile["99.000000"] // 0) / 1000),
			((.write.clat_ns.percentile["99.900000"] // 0) / 1000)
		] | @tsv
	' "$json_file" 2>/dev/null | while IFS=$'\t' read -r name mbps iops avg_lat p50 p99 p999; do
		printf "  %-10s %10s %10s %10s %10s %10s %10s\n" \
			"$name" "$mbps" "$iops" "$avg_lat" "$p50" "$p99" "$p999"
	done

	# Aggregate bandwidth
	total_bw=$(jq '[.jobs[].write.bw // 0] | add / 1024 | . * 10 | round / 10' \
		"$json_file" 2>/dev/null || echo "N/A")
	cpu=$(extract_cpu "${dir}/vmstat.log")
	dirty=$(extract_peak_dirty "${dir}/meminfo.log")
	printf "  Aggregate BW: %s MB/s | Sys CPU: %s%% | Peak Dirty: %s kB\n" \
		"$total_bw" "$cpu" "${dirty:-N/A}"
	echo ""
done

# Scenario C: Noisy neighbor
echo "--- Scenario C: Noisy Writer + Latency-Sensitive Readers ---"
for mode in buffered dontcache direct; do
	dir="${RESULTS_DIR}/noisy-neighbor/${mode}"
	if [ ! -d "$dir" ]; then
		continue
	fi

	echo "  Mode: $mode"
	printf "  %-14s %10s %10s %10s %10s %10s %10s\n" \
		"Job" "MB/s" "IOPS" "Avg(us)" "p50(us)" "p99(us)" "p99.9(us)"

	# Writer
	if [ -f "${dir}/noisy_writer.json" ]; then
		read -r mbps iops avg_lat p50 p99 p999 <<< \
			"$(extract_metrics "${dir}/noisy_writer.json" "write")"
		printf "  %-14s %10s %10s %10s %10s %10s %10s\n" \
			"Bulk writer" "$mbps" "$iops" "$avg_lat" "$p50" "$p99" "$p999"
	fi

	# Readers
	for json_file in "${dir}"/reader*.json; do
		[ -f "$json_file" ] || continue
		reader=$(basename "$json_file" .json)
		read -r mbps iops avg_lat p50 p99 p999 <<< \
			"$(extract_metrics "$json_file" "read")"
		printf "  %-14s %10s %10s %10s %10s %10s %10s\n" \
			"$reader" "$mbps" "$iops" "$avg_lat" "$p50" "$p99" "$p999"
	done

	cpu=$(extract_cpu "${dir}/vmstat.log")
	dirty=$(extract_peak_dirty "${dir}/meminfo.log")
	printf "  Sys CPU: %s%% | Peak Dirty: %s kB\n" "$cpu" "${dirty:-N/A}"
	echo ""
done

# Scenario D: Mixed-mode noisy neighbor
echo "--- Scenario D: Mixed-Mode Noisy Writer + Readers ---"
for dir in "${RESULTS_DIR}"/noisy-neighbor-mixed/*/; do
	[ -d "$dir" ] || continue
	label=$(basename "$dir")

	echo "  Mode: $label"
	printf "  %-14s %10s %10s %10s %10s %10s %10s\n" \
		"Job" "MB/s" "IOPS" "Avg(us)" "p50(us)" "p99(us)" "p99.9(us)"

	# Writer
	if [ -f "${dir}/noisy_writer.json" ]; then
		read -r mbps iops avg_lat p50 p99 p999 <<< \
			"$(extract_metrics "${dir}/noisy_writer.json" "write")"
		printf "  %-14s %10s %10s %10s %10s %10s %10s\n" \
			"Bulk writer" "$mbps" "$iops" "$avg_lat" "$p50" "$p99" "$p999"
	fi

	# Readers
	for json_file in "${dir}"/reader*.json; do
		[ -f "$json_file" ] || continue
		reader=$(basename "$json_file" .json)
		read -r mbps iops avg_lat p50 p99 p999 <<< \
			"$(extract_metrics "$json_file" "read")"
		printf "  %-14s %10s %10s %10s %10s %10s %10s\n" \
			"$reader" "$mbps" "$iops" "$avg_lat" "$p50" "$p99" "$p999"
	done

	cpu=$(extract_cpu "${dir}/vmstat.log")
	dirty=$(extract_peak_dirty "${dir}/meminfo.log")
	printf "  Sys CPU: %s%% | Peak Dirty: %s kB\n" "$cpu" "${dirty:-N/A}"
	echo ""
done

# Scenario E: Competing writers
echo "--- Scenario E: Competing Writers (Separate Files) ---"
for dir in "${RESULTS_DIR}"/competing-writers/*/; do
	[ -d "$dir" ] || continue
	label=$(basename "$dir")

	echo "  Mode: $label"
	printf "  %-20s %10s %10s %10s %10s %10s %10s\n" \
		"Writer" "MB/s" "IOPS" "Avg(us)" "p50(us)" "p99(us)" "p99.9(us)"

	total_bw=0
	for json_file in "${dir}"/writer*.json; do
		[ -f "$json_file" ] || continue
		writer=$(basename "$json_file" .json)
		read -r mbps iops avg_lat p50 p99 p999 <<< \
			"$(extract_metrics "$json_file" "write")"
		printf "  %-20s %10s %10s %10s %10s %10s %10s\n" \
			"$writer" "$mbps" "$iops" "$avg_lat" "$p50" "$p99" "$p999"
		total_bw=$(echo "$total_bw + ${mbps:-0}" | bc 2>/dev/null || echo "$total_bw")
	done

	cpu=$(extract_cpu "${dir}/vmstat.log")
	dirty=$(extract_peak_dirty "${dir}/meminfo.log")
	printf "  Aggregate BW: %s MB/s | Sys CPU: %s%% | Peak Dirty: %s kB\n" \
		"$total_bw" "$cpu" "${dirty:-N/A}"
	echo ""
done

########################################################################
# Deliverable 3: Axboe 32-file write benchmark
########################################################################
echo "=================================================================="
echo "  Deliverable 3: 32-File Write (Axboe Test)"
echo "=================================================================="
echo ""

for mode in buffered dontcache direct; do
	dir="${RESULTS_DIR}/axboe-write/${mode}"
	if [ ! -d "$dir" ]; then
		continue
	fi

	json_file=$(find "$dir" -name '*.json' 2>/dev/null | head -1 || true)
	if [ -z "$json_file" ] || [ ! -f "$json_file" ]; then
		echo "--- $mode: (no data) ---"
		continue
	fi

	echo "--- $mode ---"

	# Aggregate stats across all 32 jobs
	agg_bw=$(jq '[.jobs[].write.bw // 0] | add / 1024 | . * 10 | round / 10' \
		"$json_file" 2>/dev/null || echo "N/A")
	agg_iops=$(jq '[.jobs[].write.iops // 0] | add | round' \
		"$json_file" 2>/dev/null || echo "N/A")

	# Average latency across jobs
	avg_lat=$(jq '[.jobs[].write.clat_ns.mean // 0] | (add / length / 1000) |
		. * 10 | round / 10' "$json_file" 2>/dev/null || echo "N/A")
	avg_p50=$(jq '[.jobs[].write.clat_ns.percentile["50.000000"] // 0] |
		(add / length / 1000) | round' "$json_file" 2>/dev/null || echo "N/A")
	avg_p99=$(jq '[.jobs[].write.clat_ns.percentile["99.000000"] // 0] |
		(add / length / 1000) | round' "$json_file" 2>/dev/null || echo "N/A")
	avg_p999=$(jq '[.jobs[].write.clat_ns.percentile["99.900000"] // 0] |
		(add / length / 1000) | round' "$json_file" 2>/dev/null || echo "N/A")

	printf "  Aggregate BW: %s MB/s | IOPS: %s\n" "$agg_bw" "$agg_iops"
	printf "  Avg Latency: %s us | p50: %s us | p99: %s us | p99.9: %s us\n" \
		"$avg_lat" "$avg_p50" "$avg_p99" "$avg_p999"

	cpu=$(extract_cpu "${dir}/vmstat.log")
	dirty=$(extract_peak_dirty "${dir}/meminfo.log")
	cached=$(extract_peak_cached "${dir}/meminfo.log")
	printf "  Sys CPU: %s%% | Peak Dirty: %s kB | Peak Cached: %s kB\n" \
		"$cpu" "${dirty:-N/A}" "${cached:-N/A}"

	# Per-second bandwidth from fio bw log (shows the page-cache cliff)
	bw_log=$(find "$dir" -name '*_bw.*.log' 2>/dev/null | head -1 || true)
	if [ -n "$bw_log" ] && [ -f "$bw_log" ]; then
		echo "  Per-second aggregate BW (MB/s):"
		# fio bw logs: msec, bw_kB, rw, bs — aggregate across all job logs
		for logfile in "${dir}"/*_bw.*.log; do
			[ -f "$logfile" ] || continue
			cat "$logfile"
		done | awk -F',' '{
			sec = int($1 / 1000) + 1
			bw[sec] += $2
		} END {
			n = asorti(bw, sorted, "@ind_num_asc")
			for (i = 1; i <= n; i++)
				printf "    %2ds: %.0f MB/s\n", sorted[i], bw[sorted[i]] / 1024
		}'
	fi
	echo ""
done

echo "=================================================================="
echo "  System Info"
echo "=================================================================="
if [ -f "${RESULTS_DIR}/sysinfo.txt" ]; then
	head -6 "${RESULTS_DIR}/sysinfo.txt"
fi
echo ""
