// SPDX-License-Identifier: GPL-2.0
/*
 * Test the ADDR_LIMIT_47BIT personality flag.
 */
#include <sys/syscall.h>
#include <sys/mman.h>
#include <errno.h>
#include "../kselftest.h"

#define LENGTH (100000000)

#define ADDR_LIMIT_47BIT	0x10000000
#define BIT47			1UL << 47

/*
 * Map memory with ADDR_LIMIT_47BIT until no memory left. Ensure that all returned
 * addresses are below 47 bits.
 */
int main(int argc, char **argv)
{
	void *addr;

	syscall(__NR_personality, ADDR_LIMIT_47BIT);

	do {
		addr = mmap(0, LENGTH, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	} while (addr != MAP_FAILED && (unsigned long)addr < BIT47);

	if (errno == ENOMEM)
		ksft_test_result_pass("ADDR_LIMIT_47BIT works\n");
	else
		ksft_test_result_fail("mmap returned address above 47 bits with ADDR_LIMIT_47BIT with addr: %p and err: %s\n",
				      addr, strerror(errno));
}
