// SPDX-License-Identifier: GPL-2.0-only
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>

#include <linux/ptrace.h>
#include <linux/elf.h>

#include "../../kselftest_harness.h"
#include "v_helpers.h"

volatile unsigned long data = 0;
volatile unsigned long lock = 0;

TEST(ptrace_vlenb)
{
	pid_t pid;

	if (!is_vector_supported() && !is_xtheadvector_supported())
		SKIP(return, "Vector not supported");

	pid = fork();

	ASSERT_LE(0, pid) {
		TH_LOG("fork: %m");
	}

	if (pid == 0) {
		while (lock == 0)
			asm volatile("" : : "g"(lock) : "memory");

		asm volatile("csrr %[data], vlenb" : [data] "=r"(data));
		asm volatile ("ebreak" : : : );
	} else {
		struct __riscv_v_regset_state *regset_data;
		size_t regset_size;
		struct iovec iov;
		unsigned long vlenb_csr;
		int status;

		/* attach */

		ASSERT_EQ(0, ptrace(PTRACE_ATTACH, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* unlock */

		ASSERT_EQ(0, ptrace(PTRACE_POKEDATA, pid, &lock, 1));

		/* resume and wait ebreak */

		ASSERT_EQ(0, ptrace(PTRACE_CONT, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* read tracee vlenb via ptrace peek */

		errno = 0;
		vlenb_csr = ptrace(PTRACE_PEEKDATA, pid, &data, NULL);
		ASSERT_FALSE((errno != 0) && (vlenb_csr == -1));

		/* read tracee vlenb via ptrace regs */

		regset_size = sizeof(struct __riscv_v_regset_state) +
			vlenb_csr * 8 * 32;
		regset_data = calloc(1, regset_size);

		iov.iov_base = regset_data;
		iov.iov_len = regset_size;

		ASSERT_EQ(0, ptrace(PTRACE_GETREGSET, pid, NT_RISCV_VECTOR, &iov));

		/* compare */

		EXPECT_EQ(vlenb_csr, regset_data->vlenb);
	}
}

TEST_HARNESS_MAIN
