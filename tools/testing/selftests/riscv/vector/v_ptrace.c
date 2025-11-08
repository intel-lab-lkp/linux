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

volatile unsigned long chld_lock;

TEST(ptrace_v_not_enabled)
{
	pid_t pid;

	if (!is_vector_supported())
		SKIP(return, "Vector not supported");

	chld_lock = 1;
	pid = fork();
	ASSERT_LE(0, pid)
		TH_LOG("fork: %m");

	if (pid == 0) {
		while (chld_lock == 1)
			asm volatile("" : : "g"(chld_lock) : "memory");

		asm volatile ("ebreak" : : : );
	} else {
		struct __riscv_v_regset_state *regset_data;
		unsigned long vlenb;
		size_t regset_size;
		struct iovec iov;
		int status;
		int ret;

		asm volatile("csrr %[vlenb], vlenb" : [vlenb] "=r"(vlenb));

		ASSERT_GT(vlenb, 0)
			TH_LOG("vlenb is not valid: %lu\n", vlenb);

		/* attach */

		ASSERT_EQ(0, ptrace(PTRACE_ATTACH, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* unlock */

		ASSERT_EQ(0, ptrace(PTRACE_POKEDATA, pid, &chld_lock, 0));

		/* resume and wait for ebreak */

		ASSERT_EQ(0, ptrace(PTRACE_CONT, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* try to read vector registers from the tracee */

		regset_size = sizeof(*regset_data) + vlenb * 32;
		regset_data = calloc(1, regset_size);

		iov.iov_base = regset_data;
		iov.iov_len = regset_size;

		/* V extension is available, but not yet enabled for the tracee */

		errno = 0;
		ret = ptrace(PTRACE_GETREGSET, pid, NT_RISCV_VECTOR, &iov);
		ASSERT_EQ(ENODATA, errno);
		ASSERT_EQ(-1, ret);

		/* cleanup */

		ASSERT_EQ(0, kill(pid, SIGKILL));
	}
}

TEST(ptrace_v_early_debug)
{
	static volatile unsigned long vstart;
	static volatile unsigned long vtype;
	static volatile unsigned long vlenb;
	static volatile unsigned long vcsr;
	static volatile unsigned long vl;
	pid_t pid;

	if (!is_vector_supported())
		SKIP(return, "Vector not supported");

	chld_lock = 1;
	pid = fork();
	ASSERT_LE(0, pid)
		TH_LOG("fork: %m");

	if (pid == 0) {
		while (chld_lock == 1)
			asm volatile("" : : "g"(chld_lock) : "memory");

		asm volatile("csrr %[vstart], vstart" : [vstart] "=r"(vstart));
		asm volatile("csrr %[vl], vl" : [vl] "=r"(vl));
		asm volatile("csrr %[vtype], vtype" : [vtype] "=r"(vtype));
		asm volatile("csrr %[vcsr], vcsr" : [vcsr] "=r"(vcsr));
		asm volatile("csrr %[vlenb], vlenb" : [vlenb] "=r"(vlenb));

		asm volatile ("ebreak" : : : );
	} else {
		struct __riscv_v_regset_state *regset_data;
		unsigned long vstart_csr;
		unsigned long vlenb_csr;
		unsigned long vtype_csr;
		unsigned long vcsr_csr;
		unsigned long vl_csr;
		size_t regset_size;
		struct iovec iov;
		int status;

		/* attach */

		ASSERT_EQ(0, ptrace(PTRACE_ATTACH, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* unlock */

		ASSERT_EQ(0, ptrace(PTRACE_POKEDATA, pid, &chld_lock, 0));

		/* resume and wait for ebreak */

		ASSERT_EQ(0, ptrace(PTRACE_CONT, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* read tracee vector csr regs using ptrace PEEKDATA */

		errno = 0;
		vstart_csr = ptrace(PTRACE_PEEKDATA, pid, &vstart, NULL);
		ASSERT_FALSE((errno != 0) && (vstart_csr == -1));

		errno = 0;
		vl_csr = ptrace(PTRACE_PEEKDATA, pid, &vl, NULL);
		ASSERT_FALSE((errno != 0) && (vl_csr == -1));

		errno = 0;
		vtype_csr = ptrace(PTRACE_PEEKDATA, pid, &vtype, NULL);
		ASSERT_FALSE((errno != 0) && (vtype_csr == -1));

		errno = 0;
		vcsr_csr = ptrace(PTRACE_PEEKDATA, pid, &vcsr, NULL);
		ASSERT_FALSE((errno != 0) && (vcsr_csr == -1));

		errno = 0;
		vlenb_csr = ptrace(PTRACE_PEEKDATA, pid, &vlenb, NULL);
		ASSERT_FALSE((errno != 0) && (vlenb_csr == -1));

		/* read tracee csr regs using ptrace GETREGSET */

		regset_size = sizeof(*regset_data) + vlenb_csr * 32;
		regset_data = calloc(1, regset_size);

		iov.iov_base = regset_data;
		iov.iov_len = regset_size;

		ASSERT_EQ(0, ptrace(PTRACE_GETREGSET, pid, NT_RISCV_VECTOR, &iov));

		/* compare */

		EXPECT_EQ(vstart_csr, regset_data->vstart);
		EXPECT_EQ(vtype_csr, regset_data->vtype);
		EXPECT_EQ(vlenb_csr, regset_data->vlenb);
		EXPECT_EQ(vcsr_csr, regset_data->vcsr);
		EXPECT_EQ(vl_csr, regset_data->vl);

		/* cleanup */

		ASSERT_EQ(0, kill(pid, SIGKILL));
	}
}

TEST_HARNESS_MAIN
