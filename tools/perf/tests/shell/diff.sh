#!/bin/bash
# perf diff tests
# SPDX-License-Identifier: GPL-2.0

set -e

err=0
perfdata1=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfdata2=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfdata3=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
diffout=$(mktemp /tmp/__perf_test.perf.diff.out.XXXXX)
testprog="perf test -w thloop"
testprog2="perf test -w noploop"

shelldir=$(dirname "$0")
. "${shelldir}"/lib/perf_has_symbol.sh

testsym="test_loop"
testsym2="noploop"

skip_test_missing_symbol ${testsym}
skip_test_missing_symbol ${testsym2}

cleanup() {
  rm -rf "${perfdata1}"
  rm -rf "${perfdata1}".old
  rm -rf "${perfdata2}"
  rm -rf "${perfdata2}".old
  rm -rf "${perfdata3}"
  rm -rf "${perfdata3}".old
  rm -f "${diffout}"

  trap - EXIT TERM INT
}

trap_cleanup() {
  cleanup
  exit 1
}
trap trap_cleanup EXIT TERM INT

make_data() {
  file="$1"
  prog="$2"
  sym="$3"
  if ! perf record -o "${file}" ${prog} 2> /dev/null
  then
    echo "Workload record [Failed record]" >&2
    echo 1
    return
  fi
  if ! perf report -i "${file}" -q | grep -q "${sym}"
  then
    echo "Workload record [Failed missing output]" >&2
    echo 1
    return
  fi
  echo 0
}

test_two_files() {
  echo "Basic two file diff test"
  err=$(make_data "${perfdata1}" "${testprog}" "${testsym}")
  if [ "$err" != 0 ]
  then
    return
  fi
  err=$(make_data "${perfdata2}" "${testprog}" "${testsym}")
  if [ "$err" != 0 ]
  then
    return
  fi

  if ! perf diff "${perfdata1}" "${perfdata2}" | grep -q "${testsym}"
  then
    echo "Basic two file diff test [Failed diff]"
    err=1
    return
  fi
  echo "Basic two file diff test [Success]"
}

test_three_files() {
  echo "Basic three file diff test"
  err=$(make_data "${perfdata1}" "${testprog}" "${testsym}")
  if [ "$err" != 0 ]
  then
    return
  fi
  err=$(make_data "${perfdata2}" "${testprog}" "${testsym}")
  if [ "$err" != 0 ]
  then
    return
  fi
  # The third file is recorded with another workload, so that the
  # baseline-only and order option checks below can tell which entries
  # are shown and in what order.
  err=$(make_data "${perfdata3}" "${testprog2}" "${testsym2}")
  if [ $err != 0 ]
  then
    return
  fi

  if ! perf diff "${perfdata1}" "${perfdata2}" "${perfdata3}" | grep -q "${testsym}"
  then
    echo "Basic three file diff test [Failed diff]"
    err=1
    return
  fi
  echo "Basic three file diff test [Success]"
}

test_options() {
  echo "Diff options test"

  # delta-abs is the default compute, so a plain diff prints the same
  # header: pin it here, the option run below then only checks that
  # --compute delta-abs is accepted and still prints its column
  if ! perf diff "${perfdata1}" "${perfdata2}" | \
      grep -qE 'Baseline[[:space:]]+Delta Abs[[:space:]]+Shared Object'
  then
    echo "Diff options test [Failed: default compute is not delta-abs]"
    err=1
    return
  fi

  # Every compute formula prints a column of its own
  for compute in delta delta-abs ratio wdiff:1,2
  do
    case "${compute}" in
      delta) expect='Delta[[:space:]]+Shared Object' ;;
      delta-abs) expect='Delta Abs' ;;
      ratio) expect='Ratio' ;;
      wdiff:1,2) expect='Weighted diff' ;;
    esac
    if ! perf diff --compute "${compute}" "${perfdata1}" "${perfdata2}" | \
        grep -qE "${expect}"
    then
      echo "Diff options test [Failed: --compute ${compute}]"
      err=1
      return
    fi
  done

  if ! perf diff -F "${perfdata1}" "${perfdata2}" | grep -q "Formula"
  then
    echo "Diff options test [Failed: --formula]"
    err=1
    return
  fi

  if ! perf diff -p "${perfdata1}" "${perfdata2}" | grep -q "Base period"
  then
    echo "Diff options test [Failed: --period]"
    err=1
    return
  fi

  # -b keeps the entries matched in the baseline: with two files of
  # the same workload the loop entry stays
  if ! perf diff -b "${perfdata1}" "${perfdata2}" | grep -q "${testsym}"
  then
    echo "Diff options test [Failed: --baseline-only drops common entries]"
    err=1
    return
  fi

  # The noploop workload is only in the third file
  if ! perf diff "${perfdata1}" "${perfdata3}" > "${diffout}" 2> /dev/null
  then
    echo "Diff options test [Failed: diff of different workloads]"
    err=1
    return
  fi
  if ! grep -q "${testsym2}" "${diffout}"
  then
    echo "Diff options test [Failed: ${testsym2} missing from the diff]"
    err=1
    return
  fi

  # and with a second file recorded with another workload its entries
  # have no match in the baseline and disappear
  if ! perf diff -b "${perfdata1}" "${perfdata3}" > "${diffout}" 2> /dev/null
  then
    echo "Diff options test [Failed: --baseline-only]"
    err=1
    return
  fi
  if grep -q "${testsym2}" "${diffout}"
  then
    echo "Diff options test [Failed: --baseline-only shows non-baseline entries]"
    err=1
    return
  fi

  if ! perf diff -t, "${perfdata1}" "${perfdata2}" | grep -q "Baseline,"
  then
    echo "Diff options test [Failed: --field-separator]"
    err=1
    return
  fi

  # -o 0 puts the entry with the biggest baseline first, the loop in
  # the first file, while the default starts with the loop in the
  # second file
  if ! perf diff -o 0 "${perfdata1}" "${perfdata3}" > "${diffout}" 2> /dev/null
  then
    echo "Diff options test [Failed: --order]"
    err=1
    return
  fi
  if ! grep -v '^#' "${diffout}" | grep -v '^[[:space:]]*$' | head -1 | \
      grep -q "${testsym}"
  then
    echo "Diff options test [Failed: --order 0 does not sort by the baseline]"
    err=1
    return
  fi

  # A window starting at the beginning of the data shows it all, a
  # window starting after the last sample shows nothing
  if ! perf diff --time 0, "${perfdata1}" "${perfdata2}" | grep -q "${testsym}"
  then
    echo "Diff options test [Failed: --time]"
    err=1
    return
  fi
  if perf diff --time 9000000000000000000, "${perfdata1}" "${perfdata2}" | \
      grep -q "${testsym}"
  then
    echo "Diff options test [Failed: --time shows samples outside the window]"
    err=1
    return
  fi

  # --percentage only shows up when entries are filtered in the TUI
  # and --verbose only adds debug output, check the table is printed
  if ! perf diff --percentage relative "${perfdata1}" "${perfdata2}" | grep -q "${testsym}"
  then
    echo "Diff options test [Failed: --percentage relative]"
    err=1
    return
  fi

  if ! perf diff --percentage absolute "${perfdata1}" "${perfdata2}" | grep -q "${testsym}"
  then
    echo "Diff options test [Failed: --percentage absolute]"
    err=1
    return
  fi

  if ! perf diff -s comm "${perfdata1}" "${perfdata2}" | grep -q "Command"
  then
    echo "Diff options test [Failed: --sort comm]"
    err=1
    return
  fi

  if ! perf diff -v "${perfdata1}" "${perfdata2}" 2> /dev/null | \
      grep -q "${testsym}"
  then
    echo "Diff options test [Failed: --verbose]"
    err=1
    return
  fi

  if ! perf diff --dump-raw-trace "${perfdata1}" "${perfdata2}" 2>&1 | \
          grep -q "PERF_RECORD_SAMPLE"
  then
    echo "Diff options test [Failed: --dump-raw-trace]"
    err=1
    return
  fi

  echo "Diff options test [Success]"
}

test_two_files
test_three_files
test_options

cleanup
exit $err
