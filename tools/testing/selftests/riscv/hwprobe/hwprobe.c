// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <uapi/linux/prctl.h>
#include <sys/auxv.h>
#include <sys/prctl.h>

#include "hwprobe.h"
#include "kselftest.h"

#define RVA23U64_HWCAP ( \
	(1 << ('I' - 'A')) | \
	(1 << ('M' - 'A')) | \
	(1 << ('A' - 'A')) | \
	(1 << ('F' - 'A')) | \
	(1 << ('D' - 'A')) | \
	(1 << ('C' - 'A')) | \
	(1 << ('B' - 'A')) | \
	(1 << ('V' - 'A')) \
)

#define RVA23U64_EXT0 ( \
	RISCV_HWPROBE_IMA_FD | \
	RISCV_HWPROBE_IMA_C | \
	RISCV_HWPROBE_IMA_V | \
	RISCV_HWPROBE_EXT_ZBA | \
	RISCV_HWPROBE_EXT_ZBB | \
	RISCV_HWPROBE_EXT_ZBS | \
	RISCV_HWPROBE_EXT_ZICBOZ | \
	RISCV_HWPROBE_EXT_ZKT | \
	RISCV_HWPROBE_EXT_ZVBB | \
	RISCV_HWPROBE_EXT_ZVKT | \
	RISCV_HWPROBE_EXT_ZFHMIN | \
	RISCV_HWPROBE_EXT_ZIHINTNTL | \
	RISCV_HWPROBE_EXT_ZVFHMIN | \
	RISCV_HWPROBE_EXT_ZFA | \
	RISCV_HWPROBE_EXT_ZICNTR | \
	RISCV_HWPROBE_EXT_ZICOND | \
	RISCV_HWPROBE_EXT_ZIHINTPAUSE | \
	RISCV_HWPROBE_EXT_ZIHPM | \
	RISCV_HWPROBE_EXT_ZIMOP | \
	RISCV_HWPROBE_EXT_ZCB | \
	RISCV_HWPROBE_EXT_ZCMOP | \
	RISCV_HWPROBE_EXT_ZAWRS | \
	RISCV_HWPROBE_EXT_SUPM | \
	RISCV_HWPROBE_EXT_ZICBOM | \
	RISCV_HWPROBE_EXT_ZICBOP | \
	RISCV_HWPROBE_EXT_ZICCLSM \
)

#define RVA23U64_EXT1 ( \
	RISCV_HWPROBE_EXT_ZICCAMOA | \
	RISCV_HWPROBE_EXT_ZICCIF | \
	RISCV_HWPROBE_EXT_ZICCRSE | \
	RISCV_HWPROBE_EXT_ZA64RS \
)

static void check_rva23u64(unsigned long *cpus, size_t cpusetsize)
{
	struct riscv_hwprobe pairs[] = {
		{ .key = RISCV_HWPROBE_KEY_BASE_BEHAVIOR, },
		{ .key = RISCV_HWPROBE_KEY_IMA_EXT_0, },
		{ .key = RISCV_HWPROBE_KEY_IMA_EXT_1, },
		{ .key = RISCV_HWPROBE_KEY_ZICBOZ_BLOCK_SIZE, },
		{ .key = RISCV_HWPROBE_KEY_ZICBOM_BLOCK_SIZE, },
		{ .key = RISCV_HWPROBE_KEY_ZICBOP_BLOCK_SIZE, },
	};
	long ret, pmlen0, pmlen7;
	unsigned long hwcap;

	ret = riscv_hwprobe(pairs, ARRAY_SIZE(pairs), cpusetsize, cpus, 0);
	assert(ret == 0);
	assert(pairs[0].key == RISCV_HWPROBE_KEY_BASE_BEHAVIOR);
	assert(pairs[1].key == RISCV_HWPROBE_KEY_IMA_EXT_0);
	assert(pairs[3].key == RISCV_HWPROBE_KEY_ZICBOZ_BLOCK_SIZE);
	assert(pairs[4].key == RISCV_HWPROBE_KEY_ZICBOM_BLOCK_SIZE);
	assert(pairs[5].key == RISCV_HWPROBE_KEY_ZICBOP_BLOCK_SIZE);

	if (!(pairs[0].value & RISCV_HWPROBE_BASE_BEHAVIOR_RVA23U64)) {
		ksft_test_result_skip("rva23u64 check, no RVA23U64 base behavior\n");
		return;
	}

	assert(pairs[2].key == RISCV_HWPROBE_KEY_IMA_EXT_1);
	assert(pairs[0].value & RISCV_HWPROBE_BASE_BEHAVIOR_IMA);

	if (prctl(PR_SET_TAGGED_ADDR_CTRL, 0 << PR_PMLEN_SHIFT, 0, 0, 0)) {
		ksft_test_result_skip("rva23u64 check, failed PR_SET_TAGGED_ADDR_CTRL\n");
		return;
	}
	ret = prctl(PR_GET_TAGGED_ADDR_CTRL, 0, 0, 0, 0);
	if (ret < 0) {
		ksft_test_result_skip("rva23u64 check, failed PR_GET_TAGGED_ADDR_CTRL\n");
		return;
	}
	pmlen0 = (ret & PR_PMLEN_MASK) >> PR_PMLEN_SHIFT;

	ret = prctl(PR_SET_TAGGED_ADDR_CTRL, 7 << PR_PMLEN_SHIFT, 0, 0, 0);
	assert(ret == 0);
	ret = prctl(PR_GET_TAGGED_ADDR_CTRL, 0, 0, 0, 0);
	assert(ret >= 0);
	pmlen7 = (ret & PR_PMLEN_MASK) >> PR_PMLEN_SHIFT;

	hwcap = getauxval(AT_HWCAP);

	ksft_test_result((pairs[1].value & RVA23U64_EXT0) == RVA23U64_EXT0 &&
			 (pairs[2].value & RVA23U64_EXT1) == RVA23U64_EXT1 &&
			 pairs[3].value == 64 && pairs[4].value == 64 && pairs[5].value == 64 &&
			 pmlen0 == 0 && pmlen7 == 7 &&
			 (hwcap & RVA23U64_HWCAP) == RVA23U64_HWCAP,
			 "rva23u64 is consistent\n");
}

int main(int argc, char **argv)
{
	struct riscv_hwprobe pairs[8];
	unsigned long cpus;
	long out;

	ksft_print_header();
	ksft_set_plan(6);

	/* Fake the CPU_SET ops. */
	cpus = -1;

	/*
	 * Just run a basic test: pass enough pairs to get up to the base
	 * behavior, and then check to make sure it's sane.
	 */
	for (long i = 0; i < 8; i++)
		pairs[i].key = i;

	out = riscv_hwprobe(pairs, 8, 1, &cpus, 0);
	if (out != 0)
		ksft_exit_fail_msg("hwprobe() failed with %ld\n", out);

	for (long i = 0; i < 4; ++i) {
		/* Fail if the kernel claims not to recognize a base key. */
		if ((i < 4) && (pairs[i].key != i))
			ksft_exit_fail_msg("Failed to recognize base key: key != i, "
					   "key=%lld, i=%ld\n", pairs[i].key, i);

		if (pairs[i].key != RISCV_HWPROBE_KEY_BASE_BEHAVIOR)
			continue;

		if (pairs[i].value & RISCV_HWPROBE_BASE_BEHAVIOR_IMA)
			continue;

		ksft_exit_fail_msg("Unexpected pair: (%lld, %llu)\n", pairs[i].key, pairs[i].value);
	}

	out = riscv_hwprobe(pairs, 8, 0, 0, 0);
	ksft_test_result(out == 0, "NULL CPU set\n");

	out = riscv_hwprobe(pairs, 8, 0, &cpus, 0);
	ksft_test_result(out != 0, "Bad CPU set\n");

	out = riscv_hwprobe(pairs, 8, 1, 0, 0);
	ksft_test_result(out != 0, "NULL CPU set with non-zero size\n");

	pairs[0].key = RISCV_HWPROBE_KEY_BASE_BEHAVIOR;
	out = riscv_hwprobe(pairs, 1, 1, &cpus, 0);
	ksft_test_result(out == 0 && pairs[0].key == RISCV_HWPROBE_KEY_BASE_BEHAVIOR,
			 "Existing key is maintained\n");

	pairs[0].key = 0x5555;
	pairs[1].key = 1;
	pairs[1].value = 0xAAAA;
	out = riscv_hwprobe(pairs, 2, 0, 0, 0);
	ksft_test_result(out == 0 && pairs[0].key == -1 &&
			 pairs[1].key == 1 && pairs[1].value != 0xAAAA,
			 "Unknown key overwritten with -1 and doesn't block other elements\n");

	check_rva23u64(&cpus, 1);

	ksft_finished();
}
