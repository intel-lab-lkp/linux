#!/bin/sh
# perf all metricgroups test
# SPDX-License-Identifier: GPL-2.0

for m in $(perf list --raw-dump metricgroups); do
  echo "Testing $m"
  result=$(perf stat -M "$m" -a true 2>&1)
  rc=$?
  # Skip if there is no access to perf_events monitoring
  # Otherwise exit based on the return code of perf comamnd.
  if echo "$result" | grep -q "Access to performance monitoring and observability operations is limited";
  then
      continue
  else
      [ $rc -ne 0 ] && exit $rc
  fi

done

exit 0
