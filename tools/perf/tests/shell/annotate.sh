#!/bin/bash
# perf annotate basic tests
# SPDX-License-Identifier: GPL-2.0

set -e

shelldir=$(dirname "$0")

. "${shelldir}"/lib/perf_has_symbol.sh

testsym="noploop"

skip_test_missing_symbol ${testsym}

err=0
perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test.perf.out.XXXXX)
testprog="perf test -w noploop"
# disassembly format: "percent : offset: instruction (operands ...)"
disasm_regex="[0-9]*\.[0-9]* *: *\w*: *\w*"

cleanup() {
  rm -rf "${perfdata}" "${perfout}"
  rm -rf "${perfdata}".old

  trap - EXIT TERM INT
}

trap_cleanup() {
  echo "Unexpected signal in ${FUNCNAME[1]}"
  cleanup
  exit 1
}
trap trap_cleanup EXIT TERM INT

test_basic() {
  mode=$1
  echo "${mode} perf annotate test"
  if [ "x${mode}" == "xBasic" ]
  then
    perf record -o "${perfdata}" ${testprog} 2> /dev/null
  else
    perf record -o - ${testprog} 2> /dev/null > "${perfdata}"
  fi
  if [ "x$?" != "x0" ]
  then
    echo "${mode} annotate [Failed: perf record]"
    err=1
    return
  fi

  # Generate the annotated output file
  if [ "x${mode}" == "xBasic" ]
  then
    perf annotate --no-demangle -i "${perfdata}" --stdio --percent-limit 10 2> /dev/null > "${perfout}"
  else
    perf annotate --no-demangle -i - --stdio 2> /dev/null --percent-limit 10 < "${perfdata}" > "${perfout}"
  fi

  # check if it has the target symbol
  if ! grep -q "${testsym}" "${perfout}"
  then
    echo "${mode} annotate [Failed: missing target symbol]"
    cat "${perfout}"
    err=1
    return
  fi

  # check if it has the disassembly lines
  if ! grep -q "${disasm_regex}" "${perfout}"
  then
    echo "${mode} annotate [Failed: missing disasm output from default disassembler]"
    err=1
    return
  fi

  # check again with a target symbol name
  if [ "x${mode}" == "xBasic" ]
  then
    perf annotate --no-demangle -i "${perfdata}" "${testsym}" 2> /dev/null > "${perfout}"
  else
    perf annotate --no-demangle -i - "${testsym}" 2> /dev/null < "${perfdata}" > "${perfout}"
  fi

  if ! head -250 "${perfout}"| grep -q -m 3 "${disasm_regex}"
  then
    echo "${mode} annotate [Failed: missing disasm output when specifying the target symbol]"
    err=1
    return
  fi

  # check one more with external objdump tool (forced by --objdump option)
  if [ "x${mode}" == "xBasic" ]
  then
    perf annotate --no-demangle -i "${perfdata}" --percent-limit 10 --objdump=objdump 2> /dev/null > "${perfout}"
  else
    perf annotate --no-demangle -i - "${testsym}" --percent-limit 10 --objdump=objdump 2> /dev/null < "${perfdata}" > "${perfout}"
  fi
  if ! grep -q -m 3 "${disasm_regex}" "${perfout}"
  then
    echo "${mode} annotate [Failed: missing disasm output from non default disassembler (using --objdump)]"
    err=1
    return
  fi
  echo "${mode} annotate test [Success]"
}

test_options() {
  echo "Options annotate test"
  perf record -o "${perfdata}" ${testprog} 2> /dev/null

  # --stdio2 prints each instruction with the symbol header
  if ! perf annotate -i "${perfdata}" --stdio2 -s "${testsym}" 2> /dev/null | grep -q "${testsym}()"
  then
    echo "Options annotate [Failed: missing --stdio2 output]"
    err=1
    return
  fi

  # -l/--print-line prints a summary of the annotated file with source lines
  if ! perf annotate -i "${perfdata}" --stdio --print-line -s "${testsym}" 2> /dev/null | grep -q "Sorted summary for file"
  then
    echo "Options annotate [Failed: missing --print-line output]"
    err=1
    return
  fi

  # --show-total-period adds a period column; --asm-raw is the default
  if ! perf annotate -i "${perfdata}" --stdio --asm-raw --show-total-period -s "${testsym}" 2> /dev/null | grep -q "Period"
  then
    echo "Options annotate [Failed: missing --show-total-period output]"
    err=1
    return
  fi

  # --show-nr-samples adds a samples column
  if ! perf annotate -i "${perfdata}" --stdio -n -s "${testsym}" 2> /dev/null | grep -q "Samples"
  then
    echo "Options annotate [Failed: missing --show-nr-samples output]"
    err=1
    return
  fi

  # --percent-type selects the percentage type shown in the header
  if ! perf annotate -i "${perfdata}" --stdio --percent-type global-period -s "${testsym}" 2> /dev/null | grep -q "percent: global period"
  then
    echo "Options annotate [Failed: missing --percent-type output]"
    err=1
    return
  fi

  # -d limits the annotation to the given DSO: perf has the symbol, a
  # DSO absent from the data produces no annotation
  if ! perf annotate -i "${perfdata}" --stdio -s "${testsym}" -d perf 2> /dev/null | grep -q "Disassembly of"
  then
    echo "Options annotate [Failed: missing -d output]"
    err=1
    return
  fi

  if perf annotate -i "${perfdata}" --stdio -s "${testsym}" -d nonexistent-dso 2> /dev/null | grep -q "Disassembly of"
  then
    echo "Options annotate [Failed: -d did not filter the DSO]"
    err=1
    return
  fi

  echo "Options annotate test [Success]"
}

test_basic Basic
test_basic Pipe
test_options

cleanup
exit $err
