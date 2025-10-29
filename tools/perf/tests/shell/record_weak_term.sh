#!/bin/bash
# record weak terms
# SPDX-License-Identifier: GPL-2.0
# Test that command line options override weak terms from sysfs or inbuilt json.
set -e

shelldir=$(dirname "$0")
# shellcheck source=lib/setup_python.sh
. "${shelldir}"/lib/setup_python.sh


event=$(perf list --json | $PYTHON -c "import json,sys; next((print(e['EventName']) for e in json.load(sys.stdin) if e.get('Encoding') and 'period=' in e.get('Encoding')))")
if [[ "$?" != "0" ]]
then
  echo "No sysfs/json events with inbuilt period."
  exit 2
fi

if ! perf record -c 1000 -vv -e "$event" -o /dev/null true 2>&1 | \
  grep -q -F '{ sample_period, sample_freq }   1000'
then
  echo "Unexpected verbose output and sample period"
  exit 1
fi
exit 0
