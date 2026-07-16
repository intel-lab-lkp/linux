#!/bin/bash
# perf metrics value validation
# SPDX-License-Identifier: GPL-2.0

shelldir=$(dirname "$0")
# shellcheck source=lib/setup_python.sh
. "${shelldir}"/lib/setup_python.sh

grep -q GenuineIntel /proc/cpuinfo || { echo Skipping non-Intel; exit 2; }

# Skip if no permission to record system-wide events
if ! perf stat -a -e instructions sleep 0.01 >/dev/null 2>&1; then
  echo "Skipping: no permission to record system-wide events (-a)"
  exit 2
fi


pythonvalidator=$(dirname $0)/lib/perf_metric_validation.py
rulefile=$(dirname $0)/lib/perf_metric_validation_rules.json
tmpdir=$(mktemp -d /tmp/__perf_test.program.XXXXX)
workload="perf bench futex hash -r 1 -s"

# Add -debug, save data file and full rule file
echo "Launch python validation script $pythonvalidator"
echo "Output will be stored in: $tmpdir"
for cputype in /sys/bus/event_source/devices/cpu_*; do
	cputype=$(basename "$cputype")
	echo "Testing metrics for: $cputype"
	mkdir -p "$tmpdir/legacy"
	$PYTHON $pythonvalidator -rule $rulefile -output_dir "$tmpdir/legacy" -wl "${workload}" \
		-cputype "${cputype}"
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "Metric validation return with errors. " \
			"Please check metrics reported with errors in: " \
			"$tmpdir/legacy"
		exit $ret
	fi

	echo "Testing metrics for: $cputype (New API)"
	mkdir -p "$tmpdir/new"
	$PYTHON $pythonvalidator -rule $rulefile -output_dir "$tmpdir/new" -wl "${workload}" \
		-cputype "${cputype}" -new
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "Metric validation return with errors (New API). " \
			"Please check metrics reported with errors in: " \
			"$tmpdir/new"
		exit $ret
	fi
done
rm -rf "$tmpdir"
exit $ret

