#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/rdma_common.sh"

require_root
require_cmd bash
require_cmd grep

trace_dir="$(tracefs_dir || true)"
if [[ -z "${trace_dir}" ]]; then
	log_warn "tracefs is unavailable"
	exit "${ksft_skip}"
fi

if [[ ! -d "${trace_dir}/events/ib_cma" ]]; then
	log_warn "ib_cma trace events are unavailable"
	exit "${ksft_skip}"
fi

workload_rc=0

cleanup_trace()
{
	local event

	for event in icm_send_req icm_send_rep icm_send_rtu icm_recv_unknown_attr; do
		[[ -f "${trace_dir}/events/ib_cma/${event}/enable" ]] && \
			echo 0 >"${trace_dir}/events/ib_cma/${event}/enable"
	done
	[[ -f "${trace_dir}/events/ib_cma/enable" ]] && echo 0 >"${trace_dir}/events/ib_cma/enable"
	echo 0 >"${trace_dir}/tracing_on"
}

trap cleanup_trace EXIT

echo 0 >"${trace_dir}/tracing_on"
echo >"${trace_dir}/trace"
echo 1 >"${trace_dir}/events/ib_cma/enable"

for event in icm_send_req icm_send_rep icm_send_rtu; do
	if [[ -f "${trace_dir}/events/ib_cma/${event}/enable" ]]; then
		echo 1 >"${trace_dir}/events/ib_cma/${event}/enable"
	fi
done

echo 1 >"${trace_dir}/tracing_on"
run_workload || workload_rc=$?
echo 0 >"${trace_dir}/tracing_on"

if [[ "${workload_rc}" -eq "${ksft_skip}" ]]; then
	exit "${ksft_skip}"
fi

trace_log="/tmp/rdma_cm_trace.$(date +%s).log"
cat "${trace_dir}/trace" >"${trace_log}"
log_info "captured trace at ${trace_log}"

if ! grep -Eq "icm_send_(req|rep|rtu)" "${trace_log}"; then
	log_err "missing CM send trace events (req/rep/rtu)"
	exit 1
fi

err_lines="$(grep "icm_.*_err" "${trace_log}" || true)"
if [[ -n "${err_lines}" ]]; then
	# DREP send failure while already in TIMEWAIT is a common teardown
	# race and is tolerated for this smoke-style validation script.
	untolerated_err_lines="$(
		printf '%s\n' "${err_lines}" | \
			grep -Ev "icm_send_drep_err: .*state=TIMEWAIT" || true
	)"
	if [[ -n "${untolerated_err_lines}" ]]; then
		log_err "error trace event detected in ib_cma path"
		printf '%s\n' "${untolerated_err_lines}" >&2
		exit 1
	fi
	log_warn "only tolerated TIMEWAIT drep errors observed"
fi

exit 0
