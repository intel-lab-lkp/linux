#!/bin/sh
# perf trace enum augmentation tests
# SPDX-License-Identifier: GPL-2.0

err=0
set -e

syscall="landlock_add_rule"
non_syscall="timer:hrtimer_init,timer:hrtimer_start"

landlock_script=$(mktemp /tmp/landlock-XXXXX.c)
landlock_ex=$(echo $landlock_script | sed -E 's/(.*).c$/\1/g')

landlock_fd=24
landlock_flags=25

. "$(dirname $0)"/lib/probe.sh
skip_if_no_perf_trace || exit 2

enum_aug_prereq() {
  echo "Checking perf trace enum augmentation prerequisites"
  if ! ls /sys/kernel/btf/vmlinux 1>/dev/null 2>&1
  then
    echo "trace+enum test [Skipped missing vmlinux BTF support]"
    err=2
    return
  fi
}

prepare_landlock_script() {
  echo "Preparing script for ${syscall} syscall"

  cat > $landlock_script << EOF
#define _GNU_SOURCE
#include <unistd.h>
#include <linux/landlock.h>
#include <sys/syscall.h>

int main()
{
	int fd = ${landlock_fd};
	int flags = ${landlock_flags};
	struct landlock_path_beneath_attr path_beneath_attr = {
	    .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE,
	};
	struct landlock_net_port_attr net_port_attr = {
	    .allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP,
	    .port = 443,
	};

	syscall(SYS_landlock_add_rule, fd, LANDLOCK_RULE_PATH_BENEATH,
		&path_beneath_attr, flags);

	syscall(SYS_landlock_add_rule, fd, LANDLOCK_RULE_NET_PORT,
		&net_port_attr, flags);

	return 0;
}
EOF

  gcc $landlock_script -o $landlock_ex
}

trace_landlock() {
  echo "Tracing syscall ${syscall}"
  if perf trace -e $syscall $landlock_ex 2>&1 | \
     grep -q -E ".*landlock_add_rule\(ruleset_fd: ${landlock_fd}, rule_type: (LANDLOCK_RULE_PATH_BENEATH|LANDLOCK_RULE_NET_PORT), rule_attr: 0x[a-f0-9]+, flags: ${landlock_flags}\) = -1.*"
  then
    err=0
  else
    err=1
  fi
}

trace_non_syscall() {
  echo "Tracing non-syscall tracepoint ${non-syscall}"
  if perf trace -e $non_syscall --max-events=1 2>&1 | \
     grep -q -E '.*timer:hrtimer_.*\(.*mode: HRTIMER_MODE_.*\)$'
  then
    err=0
  else
    err=1
  fi
}

cleanup() {
  rm -f $landlock_script $landlock_ex
}

enum_aug_prereq

prepare_landlock_script

if [ $err = 0 ]; then
  trace_landlock
fi

if [ $err = 0 ]; then
  trace_non_syscall
fi

cleanup

exit $err
