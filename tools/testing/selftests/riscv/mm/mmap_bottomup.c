// SPDX-License-Identifier: GPL-2.0-only
#include <sys/mman.h>
#include <mmap_test.h>

#include "../../kselftest_harness.h"

TEST(infinite_rlimit)
{
// Only works on 64 bit
#if __riscv_xlen == 64
	EXPECT_EQ(BOTTOM_UP, memory_layout());

	TEST_MMAPS;
#endif
}

TEST_HARNESS_MAIN
