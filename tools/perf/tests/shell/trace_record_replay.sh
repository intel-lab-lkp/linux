#!/bin/bash
# perf trace record and replay
# SPDX-License-Identifier: GPL-2.0

# Check that perf trace works with record and replay

# shellcheck source=lib/probe.sh
. "$(dirname $0)"/lib/probe.sh
# shellcheck source=lib/perf_record.sh
. "$(dirname $0)"/lib/perf_record.sh

skip_if_no_perf_trace || exit 2
[ "$(id -u)" = 0 ] || exit 2

file=$(mktemp /tmp/temporary_file.XXXXX)

check_nanosleep() {
  perf trace -i "${file}" 2>&1 | grep -q nanosleep
}

PERF_RECORD_CMD="perf trace record" perf_record_with_retry "${file}" "check_nanosleep" "sleep"
err=$?

perf_record_cleanup
rm -f ${file}

if [ $err -ne 0 ]; then
	echo "Failed: cannot find *nanosleep syscall"
	exit 1
fi
exit 0
