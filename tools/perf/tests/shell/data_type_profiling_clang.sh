#!/bin/bash
# perf data type profiling tests with clang DWARF 5
# SPDX-License-Identifier: GPL-2.0

set -e

# clang describes the location of globals with DW_OP_addrx where gcc
# uses DW_OP_addr, so the workload has to be built here to get that
# encoding.

command -v clang > /dev/null || exit 2

perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test.perf.out.XXXXX)
prog=$(mktemp /tmp/__perf_test.clang.XXXXX)
src="${prog}.c"

cleanup()
{
	rm -f "$perfdata" "$perfout" "$prog" "$src"
	trap - EXIT TERM INT
}

trap_cleanup()
{
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

# No point in recording if the PMU cannot do memory events.
if perf mem record -o /dev/null -- true 2>&1 | \
		grep -q "failed: no PMU supports the memory events"; then
	cleanup
	exit 2
fi
if ! perf mem record -o /dev/null -- true 2>/dev/null; then
	echo "Skip: cannot record memory events on this PMU"
	cleanup
	exit 2
fi

cat > "$src" <<'EOF'
#include <signal.h>
#include <unistd.h>

struct conn {
	int saddr;
	int daddr;
	long bytes;
};

static volatile struct conn conn;
static volatile sig_atomic_t done;

static void stop(int sig)
{
	done = 1;
}

int main(void)
{
	unsigned long sum = 0;
	int i = 0;

	signal(SIGALRM, stop);
	alarm(1);

	while (!done) {
		conn.saddr = i;
		conn.daddr = i + 1;
		sum += conn.bytes;
		i++;
	}
	return sum != 0;
}
EOF

# -fPIE keeps the accesses PC-relative on x86_64, the operand form the
# data type resolver handles.
if ! clang -O0 -gdwarf-5 -fPIE -pie -o "$prog" "$src"; then
	echo "Skip: cannot build the workload with clang"
	cleanup
	exit 2
fi

if ! perf mem record -o "$perfdata" -- "$prog" > /dev/null 2>&1; then
	echo "FAIL: perf mem record"
	cleanup
	exit 1
fi

if ! perf annotate --code-with-type -i "$perfdata" --stdio \
		--percent-limit 1 > "$perfout" 2>/dev/null; then
	echo "FAIL: perf annotate"
	cleanup
	exit 1
fi

if ! grep -q "# data-type: struct conn" "$perfout"; then
	echo "FAIL: struct conn not resolved, DW_OP_addrx not handled?"
	cleanup
	exit 1
fi

echo "DW_OP_addrx global resolved"
cleanup
