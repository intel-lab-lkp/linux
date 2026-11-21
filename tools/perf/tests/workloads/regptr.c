// SPDX-License-Identifier: GPL-2.0
/*
 * A struct pointer that -O2 keeps in a register, so the member accesses
 * have to be resolved from the register's type state and not from a
 * stack slot.
 *
 * See tests/shell/data_type_register.sh.
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/compiler.h>
#include "../tests.h"

struct node {
	long long a;
	long long b;
	long long c;
};

static struct node node;
static struct node *volatile vp;
static volatile unsigned long long reg_sink;
static volatile sig_atomic_t done;

static void sighandler(int sig __maybe_unused)
{
	done = 1;
}

static noinline long long touch(struct node *p)
{
	long long sum = 0;

	sum += p->a;
	p->b += sum;
	sum += p->c;
	return sum;
}

static int regptr(int argc, const char **argv)
{
	unsigned long long sum = 0;
	int sec = 1;

	if (argc > 0)
		sec = atoi(argv[0]);

	vp = &node;
	signal(SIGINT, sighandler);
	signal(SIGALRM, sighandler);
	alarm(sec);

	while (!done)
		sum += touch(vp);
	reg_sink = sum;
	return 0;
}

DEFINE_WORKLOAD(regptr);
