#!/bin/bash
# perftool-testsuite_report (exclusive)
# SPDX-License-Identifier: GPL-2.0

test -d "$(dirname "$0")/base_report" || exit 2
cd "$(dirname "$0")/base_report" || exit 2
status=0

# On s390 the default timeout for addr2line is too short, disable warnings
if [ $(uname -m) = s390x ]
then
	perf_config_tmp=$(mktemp /tmp/.perfconfig_XXXXX)
	export PERF_CONFIG="${perf_config_tmp}"
	perf config 'core.addr2line-disable-warn = true'
else
	perf_config_tmp=""
fi

PERFSUITE_RUN_DIR=$(mktemp -d /tmp/"$(basename "$0" .sh)".XXX)
export PERFSUITE_RUN_DIR

for testcase in setup.sh test_*; do                  # skip setup.sh if not present or not executable
     test -x "$testcase" || continue
     ./"$testcase"
     (( status += $? ))
done

if ! [ "$PERFTEST_KEEP_LOGS" = "y" ]; then
	rm -rf "$PERFSUITE_RUN_DIR"
fi

[ -n "${perf_config_tmp}" ] && rm -f "${perf_config_tmp}"
test $status -ne 0 && exit 1
exit 0
