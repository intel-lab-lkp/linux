// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Ventana Micro Systems Inc.
 * Copyright (c) 2025 PLCT Lab, ISCAS
 *
 * Based on tools/testing/selftests/riscv/hwprobe/cbo.c with modifications
 * for Zicbop prefetch testing.
 *
 * Run with 'taskset -c <cpu-list> prefetch' to only execute hwprobe on a
 * subset of cpus, as well as only executing the tests on those cpus.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <assert.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <asm/ucontext.h>

#include "hwprobe.h"
#include "../../kselftest.h"

#define MK_PREFETCH(fn) \
	le32_bswap(0 << 25 | (uint32_t)(fn) << 20 | 10 << 15 | 6 << 12 | 0 << 7 | 19)

static char mem[4096] __aligned(4096) = { [0 ... 4095] = 0xa5 };

static bool illegal;

static void sigill_handler(int sig, siginfo_t *info, void *context)
{
	unsigned long *regs = (unsigned long *)&((ucontext_t *)context)->uc_mcontext;
	uint32_t insn = *(uint32_t *)regs[0];

	assert(insn == MK_PREFETCH(regs[11]));

	illegal = true;
	regs[0] += 4;
}

#define prefetch_insn(base, fn)							\
({										\
	asm volatile(								\
	"mv	a0, %0\n"							\
	"li	a1, %1\n"							\
	".4byte	%2\n"								\
	: : "r" (base), "i" (fn), "i" (MK_PREFETCH(fn)) : "a0", "a1", "memory");\
})

static void prefetch_i(char *base) { prefetch_insn(base, 0); }

static void prefetch_r(char *base) { prefetch_insn(base, 1); }

static void prefetch_w(char *base) { prefetch_insn(base, 3); }

static bool is_power_of_2(__u64 n)
{
	return n != 0 && (n & (n - 1)) == 0;
}

static void test_no_zicbop(void *arg)
{
	// Zicbop prefetch.* are HINT instructions.
	ksft_print_msg("Testing Zicbop instructions\n");

	illegal = false;
	prefetch_i(&mem[0]);
	ksft_test_result(!illegal, "No prefetch.i\n");

	illegal = false;
	prefetch_r(&mem[0]);
	ksft_test_result(!illegal, "No prefetch.r\n");

	illegal = false;
	prefetch_w(&mem[0]);
	ksft_test_result(!illegal, "No prefetch.w\n");
}

static void test_zicbop(void *arg)
{
	struct riscv_hwprobe pair = {
		.key = RISCV_HWPROBE_KEY_ZICBOP_BLOCK_SIZE,
	};
	cpu_set_t *cpus = (cpu_set_t *)arg;
	__u64 block_size;
	long rc;

	rc = riscv_hwprobe(&pair, 1, sizeof(cpu_set_t), (unsigned long *)cpus, 0);
	block_size = pair.value;
	ksft_test_result(rc == 0 && pair.key == RISCV_HWPROBE_KEY_ZICBOP_BLOCK_SIZE &&
			 is_power_of_2(block_size), "Zicbop block size\n");
	ksft_print_msg("Zicbop block size: %llu\n", block_size);

	illegal = false;
	prefetch_i(&mem[0]);
	prefetch_r(&mem[0]);
	prefetch_w(&mem[0]);
	ksft_test_result(!illegal, "Zicbop prefetch.* on valid address\n");

	illegal = false;
	prefetch_i(NULL);
	prefetch_r(NULL);
	prefetch_w(NULL);
	ksft_test_result(!illegal, "Zicbop prefetch.* on NULL\n");
}

static void check_no_zicbop_cpus(cpu_set_t *cpus)
{
	struct riscv_hwprobe pair = {
		.key = RISCV_HWPROBE_KEY_IMA_EXT_0,
	};
	cpu_set_t one_cpu;
	int i = 0, c = 0;
	long rc;

	while (i++ < CPU_COUNT(cpus)) {
		while (!CPU_ISSET(c, cpus))
			++c;

		CPU_ZERO(&one_cpu);
		CPU_SET(c, &one_cpu);

		rc = riscv_hwprobe(&pair, 1, sizeof(cpu_set_t), (unsigned long *)&one_cpu, 0);
		assert(rc == 0 && pair.key == RISCV_HWPROBE_KEY_IMA_EXT_0);

		if (pair.value & RISCV_HWPROBE_EXT_ZICBOP)
			ksft_exit_fail_msg("zicbop is only present on a subset of harts.\n"
					   "Use taskset to select a set of harts where zicbop\n"
					   "presence (present or not) is consistent for each hart\n");
		++c;
	}
}

enum {
	TEST_ZICBOP,
	TEST_NO_ZICBOP,
};

enum {
	HANDLER_SIGILL,
	HANDLER_SIGSEGV,
	HANDLER_SIGBUS,
};

static struct test_info {
	bool enabled;
	unsigned int nr_tests;
	void (*test_fn)(void *arg);
} tests[] = {
	[TEST_ZICBOP]		= { .nr_tests = 3, test_zicbop },
	[TEST_NO_ZICBOP]	= { .nr_tests = 3, test_no_zicbop },
};

static struct sighandler_info {
	const char *flag;
	int sig;
} handlers[] = {
	[HANDLER_SIGILL] = { .flag = "--sigill", .sig = SIGILL },
	[HANDLER_SIGSEGV] = { .flag = "--sigsegv", .sig = SIGSEGV },
	[HANDLER_SIGBUS] = { .flag = "--sigbus", .sig = SIGBUS },
};

static bool search_flag(int argc, char **argv, const char *flag)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], flag))
			return true;
	}
	return false;
}

static void install_sigaction(int argc, char **argv)
{
	int i, rc;
	struct sigaction act = {
		.sa_sigaction = &sigill_handler,
		.sa_flags = SA_SIGINFO,
	};

	for (i = 0; i < ARRAY_SIZE(handlers); ++i) {
		if (search_flag(argc, argv, handlers[i].flag)) {
			rc = sigaction(handlers[i].sig, &act, NULL);
			assert(rc == 0);
		}
	}

	if (search_flag(argc, argv, handlers[HANDLER_SIGILL].flag))
		tests[TEST_NO_ZICBOP].enabled = true;
}

int main(int argc, char **argv)
{
	struct riscv_hwprobe pair;
	unsigned int plan = 0;
	cpu_set_t cpus;
	long rc;
	int i;

	install_sigaction(argc, argv);

	rc = sched_getaffinity(0, sizeof(cpu_set_t), &cpus);
	assert(rc == 0);

	ksft_print_header();

	pair.key = RISCV_HWPROBE_KEY_IMA_EXT_0;
	rc = riscv_hwprobe(&pair, 1, sizeof(cpu_set_t), (unsigned long *)&cpus, 0);
	if (rc < 0)
		ksft_exit_fail_msg("hwprobe() failed with %ld\n", rc);
	assert(rc == 0 && pair.key == RISCV_HWPROBE_KEY_IMA_EXT_0);

	if (pair.value & RISCV_HWPROBE_EXT_ZICBOP)
		tests[TEST_ZICBOP].enabled = true;
	else
		check_no_zicbop_cpus(&cpus);

	for (i = 0; i < ARRAY_SIZE(tests); ++i)
		plan += tests[i].enabled ? tests[i].nr_tests : 0;

	if (plan == 0)
		ksft_print_msg("No tests enabled.\n");
	else
		ksft_set_plan(plan);

	for (i = 0; i < ARRAY_SIZE(tests); ++i) {
		if (tests[i].enabled)
			tests[i].test_fn(&cpus);
	}

	ksft_finished();
}
