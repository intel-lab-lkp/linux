// SPDX-License-Identifier: GPL-2.0
/*
 * hugepage-shm:
 *
 * Example of using huge page memory in a user application using Sys V shared
 * memory system calls.  In this example the app is requesting 256MB of
 * memory that is backed by huge pages.  The application uses the flag
 * SHM_HUGETLB in the shmget system call to inform the kernel that it is
 * requesting huge pages.
 *
 * For the ia64 architecture, the Linux kernel reserves Region number 4 for
 * huge pages.  That means that if one requires a fixed address, a huge page
 * aligned address starting with 0x800000... will be required.  If a fixed
 * address is not required, the kernel will select an address in the proper
 * range.
 * Other architectures, such as ppc64, i386 or x86_64 are not so constrained.
 *
 * Note: The default shared memory limit is quite low on many kernels,
 * you may need to increase it via:
 *
 * echo 268435456 > /proc/sys/kernel/shmmax
 *
 * This will increase the maximum size per shared memory segment to 256MB.
 * The other limit that you will hit eventually is shmall which is the
 * total amount of shared memory in pages. To set it to 16GB on a system
 * with a 4kB pagesize do:
 *
 * echo 4194304 > /proc/sys/kernel/shmall
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include "../kselftest.h"

#define LENGTH (256UL*1024*1024)

/* Only ia64 requires this */
#ifdef __ia64__
#define ADDR (void *)(0x8000000000000000UL)
#define SHMAT_FLAGS (SHM_RND)
#else
#define ADDR (void *)(0x0UL)
#define SHMAT_FLAGS (0)
#endif

int main(void)
{
	int shmid;
	unsigned long i;
	char *shmaddr;

	ksft_print_header();
	ksft_set_plan(1);

	shmid = shmget(2, LENGTH, SHM_HUGETLB | IPC_CREAT | SHM_R | SHM_W);
	if (shmid < 0)
		ksft_exit_fail_msg("shmget: %s\n", strerror(errno));

	ksft_print_msg("shmid: 0x%x\n", shmid);

	shmaddr = shmat(shmid, ADDR, SHMAT_FLAGS);
	if (shmaddr == (char *)-1) {
		shmctl(shmid, IPC_RMID, NULL);
		ksft_exit_fail_msg("Shared memory attach failure: %s\n", strerror(errno));
	}

	ksft_print_msg("shmaddr: %p\n", shmaddr);

	ksft_print_msg("Starting the writes:");
	for (i = 0; i < LENGTH; i++)
		shmaddr[i] = (char)(i);
	ksft_print_msg("Done.\n");

	ksft_print_msg("Starting the Check...");
	for (i = 0; i < LENGTH; i++)
		if (shmaddr[i] != (char)i)
			ksft_exit_fail_msg("\nIndex %lu mismatched\n", i);
	ksft_print_msg("Done.\n");

	if (shmdt((const void *)shmaddr) != 0) {
		shmctl(shmid, IPC_RMID, NULL);
		ksft_exit_fail_msg("Detach failure: %s\n", strerror(errno));
	}

	shmctl(shmid, IPC_RMID, NULL);

	ksft_test_result_pass("Completed test\n");

	ksft_finished();
}
