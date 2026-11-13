#!/bin/bash
# perf data type profiling tests for register variables
# SPDX-License-Identifier: GPL-2.0

set -e

perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test.perf.out.XXXXX)
prog=$(mktemp /tmp/__perf_test.regs.XXXXX)
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

if ! perf check feature -q dwarf; then
	echo "Skip: no DWARF (libdw) support"
	cleanup
	exit 2
fi

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
#include <stdlib.h>
#include <unistd.h>

struct node {
	long a;
	long b;
	long c;
};

static struct node n;
static struct node *volatile vp;
static volatile sig_atomic_t done;

static void stop(int sig)
{
	done = 1;
}

__attribute__((noinline)) static long touch(struct node *p)
{
	long sum = 0;

	sum += p->a;
	p->b = sum;
	sum += p->c;
	return sum;
}

int main(void)
{
	unsigned long sum = 0;

	vp = &n;
	signal(SIGALRM, stop);
	alarm(1);

	while (!done)
		sum += touch(vp);

	return sum != 0;
}
EOF

# -O2: the pointer stays in a register, so the accesses are resolved
# from its type state instead of from a stack slot.
if ! cc -O2 -g -fPIE -pie -o "$prog" "$src" 2>/dev/null; then
	echo "Skip: cannot build the workload"
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

for member in "+0 (a)" "+0x8 (b)" "+0x10 (c)"; do
	if ! grep -Fq "# data-type: struct node ${member}" "$perfout"; then
		echo "FAIL: member ${member} not resolved from the register"
		cleanup
		exit 1
	fi
done

echo "register held access resolved"

cleanup
