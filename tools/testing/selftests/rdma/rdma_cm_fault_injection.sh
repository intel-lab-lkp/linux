#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/rdma_common.sh"

require_root

debugfs_fail="/sys/kernel/debug/failslab"
recovery_wait_sec=2
if [[ ! -d "${debugfs_fail}" ]]; then
	log_warn "failslab is unavailable: ${debugfs_fail}"
	exit "${ksft_skip}"
fi

for knob in probability interval times task-filter; do
	if [[ ! -f "${debugfs_fail}/${knob}" ]]; then
		log_warn "failslab knob missing: ${knob}"
		exit "${ksft_skip}"
	fi
done

orig_probability="$(cat "${debugfs_fail}/probability")"
orig_interval="$(cat "${debugfs_fail}/interval")"
orig_times="$(cat "${debugfs_fail}/times")"
orig_task_filter="$(cat "${debugfs_fail}/task-filter")"

restore_knobs()
{
	echo "${orig_probability}" >"${debugfs_fail}/probability" || true
	echo "${orig_interval}" >"${debugfs_fail}/interval" || true
	echo "${orig_times}" >"${debugfs_fail}/times" || true
	echo "${orig_task_filter}" >"${debugfs_fail}/task-filter" || true
}

trap restore_knobs EXIT

log_failslab_state()
{
	local state="$1"
	local task_filter probability interval times

	task_filter="$(cat "${debugfs_fail}/task-filter")"
	probability="$(cat "${debugfs_fail}/probability")"
	interval="$(cat "${debugfs_fail}/interval")"
	times="$(cat "${debugfs_fail}/times")"

	log_info "failslab ${state}: task-filter=${task_filter} probability=${probability}"
	log_info "failslab ${state}: interval=${interval} times=${times}"
}

echo 1 >"${debugfs_fail}/task-filter"
echo 1 >"${debugfs_fail}/probability"
echo 100 >"${debugfs_fail}/interval"
echo 1 >"${debugfs_fail}/times"
log_failslab_state "enabled"

if [[ -z "${CM_WORKLOAD_CMD:-}" && -n "${CM_VALIDATE_RECOVERY_CMD:-}" ]]; then
	CM_WORKLOAD_CMD="${CM_VALIDATE_RECOVERY_CMD}"
	log_warn "CM_WORKLOAD_CMD is not set; fallback to CM_VALIDATE_RECOVERY_CMD"
fi

injected_rc=0
run_workload || injected_rc=$?
if [[ "${injected_rc}" -eq "${ksft_skip}" ]]; then
	exit "${ksft_skip}"
fi
log_info "workload rc under injection=${injected_rc}"

echo 0 >"${debugfs_fail}/probability"
echo 0 >"${debugfs_fail}/times"
echo 0 >"${debugfs_fail}/task-filter"
log_failslab_state "disabled"

recovery_cmd="${CM_VALIDATE_RECOVERY_CMD:-${CM_WORKLOAD_CMD:-}}"
if [[ -z "${recovery_cmd}" ]]; then
	log_warn "CM_VALIDATE_RECOVERY_CMD and CM_WORKLOAD_CMD are both unset"
	exit "${ksft_skip}"
fi

if [[ "${recovery_wait_sec}" != "0" ]]; then
	log_info "waiting ${recovery_wait_sec}s before recovery workload"
	sleep "${recovery_wait_sec}"
fi

log_info "running recovery workload: ${recovery_cmd}"
if ! bash -c "${recovery_cmd}"; then
	log_err "recovery workload failed after disabling fault injection"
	log_err "hint: ensure remote server is restarted and listening for a second connection"
	exit 1
fi

exit 0
