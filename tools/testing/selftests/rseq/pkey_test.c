// SPDX-License-Identifier: LGPL-2.1
/*
 * Ensure that rseq works when rseq data is inaccessible due to PKEYs.
 */

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "rseq.h"
#include "rseq-abi.h"

static int pkey;
static ucontext_t ucp0, ucp1;

static void coroutine(void)
{
	int i, orig_pk0, old_pk0, old_pk1, pk0, pk1;
	/*
	 * When we disable access to pkey 0, globals and TLS become
	 * inaccessible too, so we need to tread carefully.
	 * Pkey is global so we need to copy it onto the stack.
	 */
	int pk = RSEQ_READ_ONCE(pkey);
	struct timespec ts;

	orig_pk0 = pkey_get(0);
	if (pkey_set(0, PKEY_DISABLE_ACCESS))
		err(1, "pkey_set failed");
	old_pk0 = pkey_get(0);
	old_pk1 = pkey_get(pk);

	/*
	 * Prevent compiler from initializing it by loading a 16-global.
	 */
	RSEQ_WRITE_ONCE(ts.tv_sec, 0);
	RSEQ_WRITE_ONCE(ts.tv_nsec, 10 * 1000);
	/*
	 * If the kernel misbehaves, context switches in the following loop
	 * will terminate the process with SIGSEGV.
	 * Trigger preemption w/o accessing TLS.
	 * Note that glibc's usleep touches errno always.
	 */
	for (i = 0; i < 10; i++)
		syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, NULL);

	pk0 = pkey_get(0);
	pk1 = pkey_get(pk);
	if (pkey_set(0, orig_pk0))
		err(1, "pkey_set failed");

	/*
	 * Ensure that the kernel has restored the previous value of pkeys
	 * register after changing them.
	 */
	if (old_pk0 != pk0)
		errx(1, "pkey 0 changed %d->%d", old_pk0, pk0);
	if (old_pk1 != pk1)
		errx(1, "pkey 1 changed %d->%d", old_pk1, pk1);

	swapcontext(&ucp1, &ucp0);
	abort();
}

int main(int argc, char **argv)
{
	pkey = pkey_alloc(0, 0);
	if (pkey == -1) {
		printf("[SKIP]\tKernel does not support PKEYs: %s\n",
			strerror(errno));
		return 0;
	}

	if (rseq_register_current_thread())
		err(1, "rseq_register_current_thread failed");

	if (getcontext(&ucp1))
		err(1, "getcontext failed");
	ucp1.uc_stack.ss_size = getpagesize() * 4;
	ucp1.uc_stack.ss_sp = mmap(NULL, ucp1.uc_stack.ss_size,
		PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (ucp1.uc_stack.ss_sp == MAP_FAILED)
		err(1, "mmap failed");
	if (pkey_mprotect(ucp1.uc_stack.ss_sp, ucp1.uc_stack.ss_size,
			PROT_READ | PROT_WRITE, pkey))
		err(1, "pkey_mprotect failed");
	makecontext(&ucp1, coroutine, 0);
	if (swapcontext(&ucp0, &ucp1))
		err(1, "swapcontext failed");
	return 0;
}
