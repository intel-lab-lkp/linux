#!/bin/bash
# perf sched tests
# SPDX-License-Identifier: GPL-2.0

set -e

if [ "$(id -u)" != 0 ]; then
  echo "[Skip] No root permission"
  exit 2
fi

err=0
perfdata=$(mktemp /tmp/__perf_test_sched.perf.data.XXXXX)
statsdata=$(mktemp /tmp/__perf_test_sched.stats.XXXXX)
schedout=$(mktemp /tmp/__perf_test_sched.out.XXXXX)
PID1=0
PID2=0

cleanup() {
  rm -f "${perfdata}"
  rm -f "${perfdata}".old
  rm -f "${statsdata}"
  rm -f "${statsdata}".old
  rm -f "${schedout}"

  trap - EXIT TERM INT
}

trap_cleanup() {
  echo "Unexpected signal in ${FUNCNAME[1]}"
  cleanup
  exit 1
}
trap trap_cleanup EXIT TERM INT

start_noploops() {
  # Start two noploop workloads on CPU0 to trigger scheduling.
  perf test -w noploop 10 &
  PID1=$!
  taskset -pc 0 $PID1
  perf test -w noploop 10 &
  PID2=$!
  taskset -pc 0 $PID2

  if ! grep -q 'Cpus_allowed_list:\s*0$' "/proc/$PID1/status"
  then
    echo "Sched [Error taskset did not work for the 1st noploop ($PID1)]"
    grep Cpus_allowed /proc/$PID1/status
    err=1
  fi

  if ! grep -q 'Cpus_allowed_list:\s*0$' "/proc/$PID2/status"
  then
    echo "Sched [Error taskset did not work for the 2nd noploop ($PID2)]"
    grep Cpus_allowed /proc/$PID2/status
    err=1
  fi
}

cleanup_noploops() {
  kill "$PID1" "$PID2" || true
}

run_sched() {
  local desc="$1" expect="$2"

  shift 2
  echo "Sched ${desc}"
  if ! perf sched "$@" > "${schedout}" 2>&1
  then
    echo "Sched ${desc} [Failed: perf sched failed]"
    cat "${schedout}"
    err=1
    return
  fi
  if [ -n "${expect}" ] && ! grep -q "${expect}" "${schedout}"
  then
    echo "Sched ${desc} [Failed: missing '${expect}']"
    cat "${schedout}"
    err=1
    return
  fi
  echo "Sched ${desc} [Success]"
}

test_sched_record() {
  echo "Sched record"

  start_noploops

  if ! perf sched record -o "${perfdata}" sleep 1
  then
    echo "Sched record [Failed: perf sched record failed]"
    err=1
  fi

  cleanup_noploops
}

test_sched_latency() {
  run_sched "latency" "perf-noploop" latency -i "${perfdata}"
}

test_sched_latency_histogram() {
  run_sched "latency histogram" "Latency Distribution Histogram" \
    latency -H -i "${perfdata}"
  run_sched "latency linear histogram" "Latency Distribution Histogram" \
    latency --histogram --hist-mode linear -i "${perfdata}"
}

test_sched_latency_options() {
  run_sched "latency sort" "perf-noploop" latency -s max -i "${perfdata}"
  run_sched "latency per pid" "perf-noploop" latency -p -i "${perfdata}"
  run_sched "latency cpu filter" "" latency -C 0 -i "${perfdata}"
  run_sched "latency time filter" "perf-noploop" latency --time 0, -i "${perfdata}"
}

test_sched_script() {
  run_sched "script" "perf-noploop" script -i "${perfdata}"
}

test_sched_map() {
  run_sched "map" "perf-noploop" map -i "${perfdata}"
}

test_sched_map_options() {
  run_sched "map compact" "" map --compact -i "${perfdata}"
  run_sched "map color" "" map --color-pids "${PID1}" --color-cpus 0 \
    --cpus 0 -i "${perfdata}"
  run_sched "map task name" "" map --task-name perf-noploop -i "${perfdata}"
  run_sched "map fuzzy name" "" map --task-name noploop --fuzzy-name \
    -i "${perfdata}"
}

test_sched_timehist() {
  run_sched "timehist" "perf-noploop" timehist -i "${perfdata}"
}

test_sched_timehist_options() {
  run_sched "timehist summary" "Runtime summary" timehist --summary -i "${perfdata}"
  run_sched "timehist wakeups" "awakened:" timehist -w -i "${perfdata}"
  run_sched "timehist migrations" "migrated:" timehist -M -i "${perfdata}"
  run_sched "timehist idle" "Idle-time summary" timehist -I --summary -i "${perfdata}"
  run_sched "timehist state" "sch delay" \
    timehist --state --show-prio -V -P -i "${perfdata}"
  run_sched "timehist call graph" "" timehist -g --max-stack 3 -i "${perfdata}"
  run_sched "timehist pid filter" "" timehist -p "${PID1}" -i "${perfdata}"
  run_sched "timehist prio filter" "" timehist --show-prio --prio 0 -i "${perfdata}"
  run_sched "timehist with summary" "perf-noploop" timehist -S -i "${perfdata}"
}

test_sched_stats_live() {
  echo "Sched stats live"

  ret=0
  timeout -s INT -k 5 3 perf sched stats > "${schedout}" 2>&1 || ret=$?
  if [ "${ret}" != 0 ] && [ "${ret}" != 124 ]
  then
    echo "Sched stats live [Failed: perf sched stats failed]"
    cat "${schedout}"
    err=1
    return
  fi

  if ! grep -q "Description" "${schedout}"
  then
    echo "Sched stats live [Failed missing output]"
    err=1
    return
  fi
  echo "Sched stats live [Success]"
}

test_sched_stats_record() {
  if ! perf sched stats record -o "${statsdata}" sleep 1
  then
    echo "Sched stats record [Failed: perf sched stats record failed]"
    err=1
    return
  fi
  if [ ! -s "${statsdata}" ]
  then
    echo "Sched stats record [Failed: no data file]"
    err=1
    return
  fi
  echo "Sched stats record [Success]"
}

test_sched_stats_report() {
  run_sched "stats report" "Description" stats report -i "${statsdata}" -C 0 -v
}

test_sched_stats_diff() {
  run_sched "stats diff" "" stats diff "${statsdata}" "${statsdata}"
}

test_sched_record
test_sched_latency
test_sched_latency_histogram
test_sched_latency_options
test_sched_script
test_sched_map
test_sched_map_options
test_sched_timehist
test_sched_timehist_options
test_sched_stats_live
test_sched_stats_record
test_sched_stats_report
test_sched_stats_diff

cleanup
exit $err
