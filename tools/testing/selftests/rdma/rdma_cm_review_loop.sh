#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}"

declare -A rc

run_step()
{
	local name="$1"
	local cmd="$2"

	echo "==== ${name} ===="
	if bash -c "${cmd}"; then
		rc["${name}"]=0
	else
		rc["${name}"]=$?
	fi
	echo "==== ${name} rc=${rc["${name}"]} ===="
}

run_step baseline "./rdma_cm_baseline.sh"
run_step trace "./rdma_cm_trace_sequence.sh"
run_step counters "./rdma_cm_counter_delta.sh"
run_step fault_injection "./rdma_cm_fault_injection.sh"

echo "==== summary ===="
for name in baseline trace counters fault_injection; do
	echo "${name}=${rc["${name}"]}"
done

exit 0
