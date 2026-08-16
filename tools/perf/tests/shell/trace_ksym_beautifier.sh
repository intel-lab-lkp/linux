#!/bin/bash
# perf trace kernel symbol beautifier tests
# SPDX-License-Identifier: GPL-2.0

err=0
OUTPUT=$(mktemp /tmp/perf_trace_test.XXXXX)

# shellcheck source=lib/probe.sh
. "$(dirname $0)"/lib/probe.sh
skip_if_no_perf_trace || exit 2
[ "$(id -u)" = 0 ] || exit 2

cleanup() {
  rm -f ${OUTPUT}
}

trap cleanup EXIT TERM INT HUP

test_ksym_kallsyms() {
  echo "Testing perf trace kernel symbol beautifier (default kallsyms)"
  perf trace -e kmem:kmalloc --max-events=1 > ${OUTPUT} 2>&1
  if ! grep -q -E "call_site: [a-zA-Z0-9_]+" ${OUTPUT}
  then
    printf "Default kallsyms function symbolization failed, output:\n$(cat ${OUTPUT})\n"
    err=1
  fi
}

test_ksym_btf() {
  echo "Testing perf trace kernel symbol beautifier (BTF)"
  if [ ! -f /sys/kernel/btf/vmlinux ]; then
    echo "Skipping BTF test due to missing vmlinux BTF"
    return
  fi

  perf trace -e kmem:kmalloc --force-btf --max-events=1 > ${OUTPUT} 2>&1
  if ! grep -q -E "call_site: [a-zA-Z0-9_]+" ${OUTPUT}
  then
    printf "BTF function symbolization failed, output:\n$(cat ${OUTPUT})\n"
    err=1
  fi
}

test_ksym_kallsyms

if [ $err = 0 ]; then
  test_ksym_btf
fi

cleanup

exit $err
