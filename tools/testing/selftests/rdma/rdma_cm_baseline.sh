#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/rdma_common.sh"

require_root
require_cmd date
require_cmd uname

trace_dir="$(tracefs_dir || true)"
counter_root="$(find_cm_counter_root || true)"
out_dir="/tmp/rdma_cm_baseline.$(date +%s)"
dmesg_lines=400
dmesg_pattern="ib_cm|infiniband|rdma|roce|mlx|hns_roce|irdma|siw|rxe"

mkdir -p "${out_dir}"

log_info "writing baseline to ${out_dir}"

{
	echo "timestamp=$(date -u +%FT%TZ)"
	echo "kernel=$(uname -r)"
	echo "hostname=$(uname -n)"
	echo "dmesg_lines=${dmesg_lines}"
	echo "dmesg_pattern=${dmesg_pattern}"
} >"${out_dir}/env.txt"

if [[ -n "${trace_dir}" && -d "${trace_dir}/events/ib_cma" ]]; then
	find "${trace_dir}/events/ib_cma" -maxdepth 2 -name enable -print \
		>"${out_dir}/trace_events.list" 2>/dev/null || true
else
	log_warn "tracefs or ib_cma trace events are unavailable"
fi

if [[ -n "${counter_root}" ]]; then
	{
		echo "counter_root=${counter_root}"
		for group in "${RDMA_COUNTER_GROUPS[@]}"; do
			for attr in "${RDMA_COUNTER_ATTRS[@]}"; do
				value="$(read_cm_counter "${counter_root}" "${group}" "${attr}")"
				echo "${group}.${attr}=${value}"
			done
		done
	} >"${out_dir}/cm_counters.before"
else
	log_warn "cm counters are unavailable under /sys/class/infiniband"
fi

if command -v dmesg >/dev/null 2>&1; then
	dmesg | tail -n "${dmesg_lines}" | grep -E "${dmesg_pattern}" \
		>"${out_dir}/dmesg.rdma.tail" || true
fi

log_info "baseline collection completed"
exit 0
