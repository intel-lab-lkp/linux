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
#include <unistd.h>

#include "rseq.h"
#include "rseq-abi.h"

int main(int argc, char **argv)
{
	struct rseq_abi_cs *cs;
	__u32 *sig;
	unsigned long page_size;
	int pkey, i;

	pkey = pkey_alloc(0, 0);
	if (pkey == -1) {
		printf("[SKIP]\tKernel does not support PKEYs: %s\n",
			strerror(errno));
		return 0;
	}

	if (rseq_register_current_thread())
		err(1, "rseq_register_current_thread failed");

	page_size = getpagesize();
	cs = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE, -1, 0);
	if (cs == MAP_FAILED)
		err(1, "mmap failed");
	/* Create valid rseq_cs. */
	sig = (__u32 *)(cs + 1);
	*sig = RSEQ_SIG;
	cs->abort_ip = (__u64)(sig + 1);
	if (pkey_mprotect(cs, page_size, PROT_READ | PROT_WRITE, pkey))
		err(1, "pkey_mprotect failed");
	if (pkey_set(pkey, PKEY_DISABLE_ACCESS))
		err(1, "pkey_set failed");

	/* Install pkey-protected rseq_cs. */
	rseq_get_abi()->rseq_cs.ptr64 = (__u64)cs;

	/*
	 * If the kernel misbehaves, context switches in the following loop
	 * will terminate the process with SIGSEGV.
	 */
	for (i = 0; i < 10; i++)
		usleep(100);

	/*
	 * Ensure that the kernel has restored the previous value of pkeys
	 * register after changing it.
	 */
	if (pkey_get(pkey) != PKEY_DISABLE_ACCESS)
		errx(1, "pkey protection has changed");
	return 0;
}
