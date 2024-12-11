#!/bin/sh
# perf record offcpu profiling tests (exclusive)
# SPDX-License-Identifier: GPL-2.0

set -e

err=0
perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
TEST_PROGRAM="perf test -w offcpu"

ts=$(printf "%u" $((~0 << 32))) # OFF_CPU_TIMESTAMP
dummy_timestamp=${ts%???} # remove the last 3 digits to match perf script

cleanup() {
  rm -f ${perfdata}
  rm -f ${perfdata}.old
  trap - EXIT TERM INT
}

trap_cleanup() {
  cleanup
  exit 1
}
trap trap_cleanup EXIT TERM INT

test_offcpu_priv() {
  echo "Checking off-cpu privilege"

  if [ "$(id -u)" != 0 ]
  then
    echo "off-cpu test [Skipped permission]"
    err=2
    return
  fi
  if perf version --build-options 2>&1 | grep HAVE_BPF_SKEL | grep -q OFF
  then
    echo "off-cpu test [Skipped missing BPF support]"
    err=2
    return
  fi
}

test_offcpu_basic() {
  echo "Basic off-cpu test"

  # We collect an off-cpu period __ONLY ONCE__, either in direct sample form, or in accumulated form
  # (in BPF stack trace map). Without the --off-cpu-thresh 200000 below, the sample will go into the
  # ring buffer instead of the BPF stack trace map. Additionally, when using -e dummy, the ring
  # buffer is not enabled, resulting in a lost sample.
  if ! perf record --off-cpu --off-cpu-thresh 2000000 -e dummy -o ${perfdata} sleep 1 2> /dev/null
  then
    echo "Basic off-cpu test [Failed record]"
    err=1
    return
  fi
  if ! perf evlist -i ${perfdata} | grep -q "offcpu-time"
  then
    echo "Basic off-cpu test [Failed no event]"
    err=1
    return
  fi
  if ! perf report -i ${perfdata} -q --percent-limit=90 | grep -E -q sleep
  then
    echo "Basic off-cpu test [Failed missing output]"
    err=1
    return
  fi
  echo "Basic off-cpu test [Success]"
}

test_offcpu_child() {
  echo "Child task off-cpu test"

  # perf bench sched messaging creates 400 processes
  if ! perf record --off-cpu -e dummy -o ${perfdata} -- \
    perf bench sched messaging -g 10 > /dev/null 2>&1
  then
    echo "Child task off-cpu test [Failed record]"
    err=1
    return
  fi
  if ! perf evlist -i ${perfdata} | grep -q "offcpu-time"
  then
    echo "Child task off-cpu test [Failed no event]"
    err=1
    return
  fi
  # each process waits at least for poll, so it should be more than 400 events
  if ! perf report -i ${perfdata} -s comm -q -n -t ';' --percent-limit=90 | \
    awk -F ";" '{ if (NF > 3 && int($3) < 400) exit 1; }'
  then
    echo "Child task off-cpu test [Failed invalid output]"
    err=1
    return
  fi
  echo "Child task off-cpu test [Success]"
}

test_offcpu_direct() {
  echo "Direct off-cpu test"

  # dump off-cpu samples for task blocked for more than 1.999999s
  # -D for initial delay, to enable evlist
  if ! perf record -e dummy -D 500 --off-cpu --off-cpu-thresh 1999999 -o ${perfdata} ${TEST_PROGRAM} 2> /dev/null
  then
    echo "Direct off-cpu test [Failed record]"
    err=1
    return
  fi
  # Direct sample's timestamp should be lower than the dummy_timestamp of the at-the-end sample.
  if ! perf script -i ${perfdata} -F time,period | sed "s/[\.:]//g" | \
       awk "{ if (\$1 < ${dummy_timestamp} && \$2 > 1999999999) exit 0; else exit 1; }"
  then
    echo "Direct off-cpu test [Failed missing direct sample]"
    err=1
    return
  fi
  echo "Direct off-cpu test [Success]"
}

test_offcpu_priv

if [ $err = 0 ]; then
  test_offcpu_basic
fi

if [ $err = 0 ]; then
  test_offcpu_child
fi

if [ $err = 0 ]; then
  test_offcpu_direct
fi

cleanup
exit $err
