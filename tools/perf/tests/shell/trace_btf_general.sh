#!/bin/sh
# perf trace general tests
# SPDX-License-Identifier: GPL-2.0

err=0
set -e

. "$(dirname $0)"/lib/probe.sh
skip_if_no_perf_trace || exit 2

file1=$(mktemp /tmp/file1_XXXXX)
file2=$(echo $file1 | sed 's/file1/file2/g')

buffer="this is a buffer for testing"

trap cleanup EXIT TERM INT HUP

trace_test_string() {
  echo "Testing perf trace's string augmentation"
  if ! perf trace -e renameat* --max-events=1 -- mv ${file1} ${file2} 2>&1 | grep -q -E "renameat[2]*.*oldname: \"${file1}\".*newname: \"${file2}\".*"
  then
    echo "String augmentation test failed"
    err=1
  fi
}

trace_test_buffer() {
  echo "Testing perf trace's buffer augmentation"
  if ! perf trace -e write --max-events=1 -- echo "${buffer}" 2>&1 | grep -q -E ".*write.*buf: ${buffer}.*"
  then
    echo "Buffer augmentation test failed"
    err=1
  fi
}

trace_test_struct_btf() {
  echo "Testing perf trace's struct augmentation"
  if ! perf trace -e clock_nanosleep --force-btf --max-events=1 -- sleep 1 2>&1 | grep -q -E ".*clock_nanosleep\(rqtp: \{1,\}, rmtp: \{1,\}\).* = 0"
  then
    echo "BTF struct augmentation test failed"
    err=1
  fi
}

cleanup() {
	rm -rf ${file1} ${file2}
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}

trace_test_string

if [ $err = 0 ]; then
  trace_test_buffer
fi

if [ $err = 0 ]; then
  trace_test_struct_btf
fi

cleanup

exit $err
