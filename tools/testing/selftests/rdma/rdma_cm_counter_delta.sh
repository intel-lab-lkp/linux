#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/rdma_common.sh"

require_root
counter_root="$(find_cm_counter_root || true)"
counter_wait_sec=2

if [[ -z "${counter_root}" ]]; then
	log_warn "cm counters are unavailable under /sys/class/infiniband"
	exit "${ksft_skip}"
fi

declare -A before after

for group in "${RDMA_COUNTER_GROUPS[@]}"; do
	for attr in "${RDMA_COUNTER_ATTRS[@]}"; do
		key="${group}.${attr}"
		before["${key}"]="$(read_cm_counter "${counter_root}" "${group}" "${attr}")"
	done
done

if [[ "${counter_wait_sec}" != "0" ]]; then
	log_info "waiting ${counter_wait_sec}s before workload"
	sleep "${counter_wait_sec}"
fi

workload_rc=0
run_workload || workload_rc=$?
if [[ "${workload_rc}" -eq "${ksft_skip}" ]]; then
	exit "${ksft_skip}"
fi
if [[ "${workload_rc}" -ne 0 ]]; then
	log_err "workload failed with rc=${workload_rc}"
	exit "${workload_rc}"
fi

for group in "${RDMA_COUNTER_GROUPS[@]}"; do
	for attr in "${RDMA_COUNTER_ATTRS[@]}"; do
		key="${group}.${attr}"
		after["${key}"]="$(read_cm_counter "${counter_root}" "${group}" "${attr}")"
		delta=$((after["${key}"] - before["${key}"]))
		echo "${key}.delta=${delta}"
		if ((delta < 0)); then
			log_err "counter regressed: ${key}"
			exit 1
		fi
	done
done

dup_limit=10
retry_limit=10

for attr in "${RDMA_COUNTER_ATTRS[@]}"; do
	dup_delta=$((after["cm_rx_duplicates.${attr}"] - before["cm_rx_duplicates.${attr}"]))
	retry_delta=$((after["cm_tx_retries.${attr}"] - before["cm_tx_retries.${attr}"]))

	if ((dup_delta > dup_limit)); then
		log_err "duplicate counter exceeds limit: ${attr}=${dup_delta}"
		exit 1
	fi
	if ((retry_delta > retry_limit)); then
		log_err "retry counter exceeds limit: ${attr}=${retry_delta}"
		exit 1
	fi
done

exit 0
