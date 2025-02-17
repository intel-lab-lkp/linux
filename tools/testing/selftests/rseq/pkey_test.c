// SPDX-License-Identifier: LGPL-2.1
/*
 * Ensure that rseq works when rseq data is protected with PKEYs.
 */

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	void *rseq;
	unsigned long page_size;
	int pkey, i;

	pkey = pkey_alloc(0, 0);
	if (pkey == -1) {
		printf("[SKIP]\tKernel does not support PKEYs: %s\n",
			strerror(errno));
		return 0;
	}

	/*
	 * Prevent glibc from registering own struct rseq.
	 * We need to know the rseq address to protect it, but also we need
	 * it to be placed on own page that does not contain other data
	 * (e.g. errno).
	 */
	if (!getenv("RSEQ_TEST_REEXECED")) {
		setenv("RSEQ_TEST_REEXECED", "1", 1);
		setenv("GLIBC_TUNABLES", "glibc.pthread.rseq=0", 1);
		if (execvpe(argv[0], argv, environ))
			err(1, "execvpe failed");
	}

	page_size = getpagesize();
	rseq = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE, -1, 0);
	if (rseq == MAP_FAILED)
		err(1, "mmap failed");
	if (pkey_mprotect(rseq, page_size, PROT_READ | PROT_WRITE, pkey))
		err(1, "pkey_mprotect failed");
	if (syscall(__NR_rseq, rseq, 32, 0, 0))
		err(1, "rseq failed");
	if (pkey_set(pkey, PKEY_DISABLE_ACCESS))
		err(1, "pkey_set failed");

	/*
	 * If the kernel misbehaves, context switches in the following loop
	 * will kill the process with SIGSEGV.
	 */
	for (i = 0; i < 10; i++)
		usleep(100);
	return 0;
}
