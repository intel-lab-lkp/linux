#!/bin/bash
# perf trace kernel symbol beautifier tests
# SPDX-License-Identifier: GPL-2.0

err=0

# shellcheck source=lib/probe.sh
. "$(dirname "$0")"/lib/probe.sh
skip_if_no_perf_trace || exit 2
[ "$(id -u)" = 0 ] || exit 2

test_ksym_kallsyms() {
  echo "Testing perf trace kernel symbol beautifier (default kallsyms)"
  output="$(perf trace -e kmem:kmalloc --max-events=1 2>&1)"
  if ! echo "$output" | grep -q -E "call_site: [a-zA-Z_][a-zA-Z0-9_]*" || echo "$output" | grep -q -E "call_site: 0x[0-9a-fA-F]+"
  then
    printf "Default kallsyms function symbolization failed, output:\n%s\n" "$output"
    err=1
  fi
}

test_ksym_btf() {
  echo "Testing perf trace kernel symbol beautifier (BTF)"
  if [ ! -f /sys/kernel/btf/vmlinux ]; then
    echo "Skipping BTF test due to missing vmlinux BTF"
    return
  fi

  output="$(perf trace -e csd:csd_function_entry --force-btf --max-events=1 2>&1)"
  if ! echo "$output" | grep -q -E "func: [a-zA-Z_][a-zA-Z0-9_]*" || echo "$output" | grep -q -E "func: 0x[0-9a-fA-F]+"
  then
    printf "BTF function symbolization failed, output:\n%s\n" "$output"
    err=1
  fi
}

test_ksym_kallsyms

if [ $err = 0 ]; then
  test_ksym_btf
fi

exit $err
