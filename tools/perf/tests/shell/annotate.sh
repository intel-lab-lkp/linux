#!/bin/bash
# perf annotate basic tests
# SPDX-License-Identifier: GPL-2.0

set -e

export PERF_CONFIG=/dev/null

shelldir=$(dirname "$0")

# shellcheck source=lib/perf_has_symbol.sh
. "${shelldir}"/lib/perf_has_symbol.sh

testsym="noploop"

skip_test_missing_symbol ${testsym}

err=0
perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test.perf.out.XXXXX)
bpftmp=""
testprog="perf test -w noploop"
# disassembly format: "percent : offset: instruction (operands ...)"
disasm_regex="[0-9]*\.[0-9]* *: *\w*: *\w*"

cleanup() {
  rm -rf "${perfdata}" "${perfout}"
  rm -rf "${perfdata}".old
  if [ -n "${bpftmp}" ]; then
    rm -rf "${bpftmp}"
  fi

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

test_disassembler() {
  disassembler=$1
  feature=$2
  local ret=0

  if [ -n "${feature}" ]
  then
    if ! perf check feature "${feature}" > /dev/null 2>&1
    then
      echo "Skip test for ${disassembler} (feature ${feature} not supported)"
      return 0
    fi
  fi

  echo "Test annotate with disassembler: ${disassembler}"

  perf annotate --no-demangle -i "${perfdata}" --stdio \
    --percent-limit 10 --disassembler "${disassembler}" \
    2> /dev/null > "${perfout}" || ret=$?

  if [ "${ret}" -ne 0 ]
  then
    echo "annotate with ${disassembler} [Failed: perf annotate error]"
    err=1
    return 0
  fi

  # check if it has the target symbol
  if ! grep -q "${testsym}" "${perfout}"
  then
    echo "annotate with ${disassembler} [Failed: missing target symbol]"
    err=1
    return 0
  fi

  # check if it has the disassembly lines
  if ! grep -q "${disasm_regex}" "${perfout}"
  then
    echo "annotate with ${disassembler} [Failed: missing disasm output]"
    err=1
    return 0
  fi

  echo "annotate with ${disassembler} [Success]"
  return 0
}

test_basic Basic
test_basic Pipe

# Restore perfdata to a regular format for disassembler tests
perf record -o "${perfdata}" ${testprog} > /dev/null 2>&1

if [ "${err}" -eq 0 ]
then
  test_disassembler "objdump" ""
  test_disassembler "llvm" "libLLVM"
  test_disassembler "capstone" "libcapstone"
  test_disassembler "libasm" "libasm"
fi

test_bpf_disassembler() {
  disassembler=$1
  feature=$2
  bpf_perfdata_file=$3

  if [ -n "${feature}" ]
  then
    if ! perf check feature "${feature}" > /dev/null 2>&1
    then
      echo "Skip BPF JIT test for ${disassembler} (feature ${feature} not supported)"
      return 0
    fi
  fi

  echo "Test BPF JIT annotate with disassembler: ${disassembler}"

  if ! perf annotate --no-demangle -i "${bpf_perfdata_file}" --stdio "${bpf_sym}" \
      --disassembler "${disassembler}" 2> /dev/null > "${perfout}"
  then
    echo "BPF JIT annotate with ${disassembler} [Failed: perf annotate error]"
    err=1
    return 0
  fi

  if ! grep -q "${disasm_regex}" "${perfout}"
  then
    echo "BPF JIT annotate with ${disassembler} [Failed: missing disasm output]"
    err=1
    return 0
  fi

  echo "BPF JIT annotate with ${disassembler} [Success]"
  return 0
}

test_bpf() {
  echo "Test annotate with BPF JIT output"

  if ! perf check -q feature libbpf-strings ; then
    echo "BPF annotation test [Skipped - libbpf-strings not supported]"
    return 0
  fi

  bpftmp=$(mktemp -d /tmp/__perf_test.bpf.XXXXXX)
  bpf_perfdata="${bpftmp}/perf.data"

  if ! perf record -a -e cycles -F 4000 -o "${bpf_perfdata}" -- sleep 1 2> /dev/null
  then
    echo "BPF annotation test [Skipped - perf record -a failed, probably no privileges]"
    rm -rf "${bpftmp}"
    bpftmp=""
    return 0
  fi

  bpf_symbols=$(perf report --stdio -i "${bpf_perfdata}" 2>/dev/null | \
      grep -E -o 'bpf_prog_[0-9a-f]{16}_[0-9A-Za-z_]*' | sort -u)

  bpf_sym=""
  for sym in ${bpf_symbols}
  do
    # Check if we can annotate it (meaning JIT code is available)
    if perf annotate -i "${bpf_perfdata}" --stdio "${sym}" > /dev/null 2>&1
    then
      bpf_sym="${sym}"
      break
    fi
  done

  if [ -z "${bpf_sym}" ]; then
    echo "BPF annotation test [Skipped - no JITted BPF symbols with code found (race?)]"
    rm -rf "${bpftmp}"
    bpftmp=""
    return 0
  fi

  test_bpf_disassembler "objdump" "" "${bpf_perfdata}"
  test_bpf_disassembler "llvm" "libLLVM" "${bpf_perfdata}"
  test_bpf_disassembler "capstone" "libcapstone" "${bpf_perfdata}"
  test_bpf_disassembler "libasm" "libasm" "${bpf_perfdata}"

  rm -rf "${bpftmp}"
  bpftmp=""
}

if [ "${err}" -eq 0 ]
then
  test_bpf
fi

cleanup
exit $err
