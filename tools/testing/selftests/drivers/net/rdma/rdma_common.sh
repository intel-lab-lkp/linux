#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
RET=0

RDMA_COUNTER_GROUPS=(
	cm_tx_msgs
	cm_tx_retries
	cm_rx_msgs
	cm_rx_duplicates
)

RDMA_COUNTER_ATTRS=(
	req
	mra
	rej
	rep
	rtu
	dreq
	drep
	sidr_req
	sidr_rep
	lap
	apr
)

log_info()
{
	echo "INFO: $*"
}

log_warn()
{
	echo "WARN: $*" >&2
}

log_err()
{
	echo "ERROR: $*" >&2
}

require_root()
{
	if [[ "$(id -u)" -ne 0 ]]; then
		log_warn "this test requires root privileges"
		exit "${ksft_skip}"
	fi
}

require_cmd()
{
	local cmd="$1"

	command -v "${cmd}" >/dev/null 2>&1 || {
		log_warn "missing required command: ${cmd}"
		exit "${ksft_skip}"
	}
}

tracefs_dir()
{
	if [[ -d /sys/kernel/tracing ]]; then
		echo /sys/kernel/tracing
	elif [[ -d /sys/kernel/debug/tracing ]]; then
		echo /sys/kernel/debug/tracing
	else
		return 1
	fi
}

find_cm_counter_root()
{
	local base
	local port
	local candidate

	for base in /sys/class/infiniband/*; do
		[[ -d "${base}" ]] || continue

		for port in "${base}"/ports/*; do
			[[ -d "${port}" ]] || continue
			# RoCE / newer sysfs: cm_* groups live directly under ports/<N>/
			if [[ -d "${port}/cm_tx_msgs" ]]; then
				echo "${port}"
				return 0
			fi
			# Legacy layout: under counters/ or hw_counters/
			for candidate in "${port}/counters" "${port}/hw_counters"; do
				[[ -d "${candidate}/cm_tx_msgs" ]] || continue
				echo "${candidate}"
				return 0
			done
		done
	done

	return 1
}

read_cm_counter()
{
	local root="$1"
	local group="$2"
	local attr="$3"
	local path="${root}/${group}/${attr}"

	if [[ -f "${path}" ]]; then
		cat "${path}" 2>/dev/null
	else
		echo 0
	fi
}

run_workload()
{
	local cmd="${CM_WORKLOAD_CMD:-}"

	if [[ -z "${cmd}" ]]; then
		log_warn "CM_WORKLOAD_CMD is not set"
		return "${ksft_skip}"
	fi

	log_info "running workload: ${cmd}"
	bash -c "${cmd}"
}

