// SPDX-License-Identifier: GPL-2.0
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <linux/futex.h>
#include <sys/syscall.h>

#include "kselftest_harness.h"

static int sys_pkey_alloc(unsigned long flags, unsigned long init_val)
{
	return syscall(__NR_pkey_alloc, flags, init_val);
}

static int sys_pkey_mprotect(void *ptr, size_t size, int prot, int pkey)
{
	return syscall(__NR_pkey_mprotect, ptr, size, prot, pkey);
}

static int futex_wake_op(uint32_t *uaddr, uint32_t val, uint32_t val2,
			 uint32_t *uaddr2, uint32_t val3)
{
	return syscall(SYS_futex, uaddr, FUTEX_WAKE_OP, val, val2,
		       uaddr2, val3);
}

/*
 * Trigger some atomic uaccess on a page mapped with a non-default pkey.
 *
 * This ensures that such access is not mistakenly checked against the
 * kernel's POR_EL1 register.
 */
TEST(poe_futex)
{
	int ret, pkey;
	void *ptr;
	size_t size = getpagesize();

	pkey = sys_pkey_alloc(0, 0);

	if (pkey == -1 && errno == ENOSPC)
		SKIP(return, "pkeys are not supported");

	ASSERT_GT(pkey, 0);

	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	ret = sys_pkey_mprotect(ptr, size, PROT_READ | PROT_WRITE, pkey);
	ASSERT_EQ(ret, 0);

	/*
	 * There is no one to wake up so this syscall boils down to *(ptr+4) = 0
	 * (arch_futex_atomic_op_inuser() called with FUTEX_OP_SET and op_arg=0).
	 */
	ret = futex_wake_op(ptr, 1, 1, ptr + sizeof(uint32_t), 0);
	ASSERT_EQ(ret, 0);
}

TEST_HARNESS_MAIN
