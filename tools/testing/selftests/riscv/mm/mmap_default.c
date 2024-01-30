// SPDX-License-Identifier: GPL-2.0-only
#include <sys/mman.h>
#include <mmap_test.h>

#include "../../kselftest_harness.h"

TEST(default_rlimit)
{
// Only works on 64 bit
#if __riscv_xlen == 64
	EXPECT_EQ(TOP_DOWN, memory_layout());

	TEST_MMAPS;
#endif
}

TEST_HARNESS_MAIN
