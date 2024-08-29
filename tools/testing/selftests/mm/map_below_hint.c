// SPDX-License-Identifier: GPL-2.0
/*
 * Test the MAP_BELOW_HINT mmap flag.
 */
#include <sys/mman.h>
#include <errno.h>
#include "../kselftest.h"

#define ADDR (1 << 20)
#define LENGTH (ADDR / 10000)

#define MAP_BELOW_HINT	  0x8000000	/* Not defined in all libc */

/*
 * Map memory with MAP_BELOW_HINT until no memory left. Ensure that all returned
 * addresses are below the hint.
 */
int main(int argc, char **argv)
{
	void *addr;

	do {
		addr = mmap((void *)ADDR, LENGTH, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_BELOW_HINT, -1, 0);
	} while (addr != MAP_FAILED && (unsigned long)addr <= ADDR);

	if (errno == ENOMEM)
		ksft_test_result_pass("MAP_BELOW_HINT works\n");
	else
		ksft_test_result_fail("mmap returned address above hint with MAP_BELOW_HINT with error: %s\n",
				      strerror(errno));
}
