#!/bin/bash
# perf data type profiling tests for flexible array members
# SPDX-License-Identifier: GPL-2.0

set -e

perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)
perfout=$(mktemp /tmp/__perf_test.perf.out.XXXXX)
prog=$(mktemp /tmp/__perf_test.flex.XXXXX)
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
#include <stdlib.h>
#include <unistd.h>

struct flex {
	int len;
	char data[];
};

static struct flex *gp;
static volatile sig_atomic_t done;

static void stop(int sig)
{
	done = 1;
}

int main(void)
{
	unsigned long sum = 0;

	gp = calloc(1, sizeof(*gp) + 4096);
	if (gp == NULL)
		return 1;
	gp->len = 4096;

	signal(SIGALRM, stop);
	alarm(1);

	while (!done) {
		sum += gp->data[3];
		gp->data[5] = 1;
	}
	return sum != 0;
}
EOF

# -fPIE keeps the global pointer load PC-relative on x86_64, so the
# resolver can tell the register holds a struct flex pointer.
if ! cc -O0 -g -fPIE -pie -o "$prog" "$src" 2>/dev/null; then
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

if ! grep -Fq "# data-type: struct flex +0x4 (data)" "$perfout"; then
	echo "FAIL: flexible array member not resolved"
	cleanup
	exit 1
fi

echo "flexible array member resolved"
cleanup
