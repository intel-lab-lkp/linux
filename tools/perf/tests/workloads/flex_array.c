// SPDX-License-Identifier: GPL-2.0
/*
 * Flexible array member workload for data type profiling: the accesses to
 * the array are past the end of the struct, where the resolver has to find
 * the flexible array member.
 *
 * See tests/shell/data_type_flex_array.sh.
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/compiler.h>
#include "../tests.h"

struct flex {
	int len;
	char data[];
};

static struct flex *gp;
static volatile unsigned long flex_sink;
static volatile sig_atomic_t done;

static void sighandler(int sig __maybe_unused)
{
	done = 1;
}

static int flex_array(int argc, const char **argv)
{
	unsigned long sum = 0;
	int sec = 1;

	if (argc > 0)
		sec = atoi(argv[0]);

	gp = calloc(1, sizeof(*gp) + 4096);
	if (gp == NULL)
		return 1;
	gp->len = 4096;

	signal(SIGINT, sighandler);
	signal(SIGALRM, sighandler);
	alarm(sec);

	while (!done) {
		sum += gp->data[3];
		gp->data[5] = 1;
	}
	flex_sink = sum;
	free(gp);
	return 0;
}

DEFINE_WORKLOAD(flex_array);
