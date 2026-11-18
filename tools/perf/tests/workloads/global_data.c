// SPDX-License-Identifier: GPL-2.0
/*
 * A zero initialized global struct, so it lands in .bss, past the file
 * backed end of the last segment: at run time it sits in an anonymous
 * mapping that the thread maps do not name, so only the DWARF address
 * lookup finds it.  clang describes its address with DW_OP_addrx in
 * DWARF 5, gcc with DW_OP_addr.
 *
 * 'perf mem record' of this workload followed by 'perf annotate
 * --code-with-type' (see tests/shell/data_type_global.sh) resolves the
 * accesses to struct conn.
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/compiler.h>
#include "../tests.h"

struct conn {
	int saddr;
	int daddr;
	long bytes;
};

static volatile struct conn conn;
static volatile unsigned long conn_sink;
static volatile sig_atomic_t done;

static void sighandler(int sig __maybe_unused)
{
	done = 1;
}

static int global_data(int argc, const char **argv)
{
	unsigned long sum = 0;
	unsigned int i = 0;
	int sec = 1;

	if (argc > 0)
		sec = atoi(argv[0]);

	signal(SIGINT, sighandler);
	signal(SIGALRM, sighandler);
	alarm(sec);

	while (!done) {
		conn.saddr = i;
		conn.daddr = i + 1;
		sum += conn.bytes;
		i++;
	}
	conn_sink = sum;
	return 0;
}

DEFINE_WORKLOAD(global_data);
