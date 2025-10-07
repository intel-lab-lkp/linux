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

TEST(ptrace_rvv_not_enabled)
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

TEST(ptrace_rvv_invalid_vtype)
{
	static volatile unsigned long vtype;
	unsigned long vlenb;
	unsigned long reg;
	pid_t pid;

	if (!is_vector_supported())
		SKIP(return, "Vector not supported");

	asm volatile("csrr %[vlenb], vlenb" : [vlenb] "=r"(vlenb));

	if (vlenb > 16)
		SKIP(return, "This test does not support VLEN > 128");

	chld_lock = 1;

	pid = fork();

	ASSERT_LE(0, pid)
		TH_LOG("fork: %m");

	if (pid == 0) {
		while (chld_lock == 1)
			asm volatile("" : : "g"(chld_lock) : "memory");

		asm(".option arch, +v\n");
		asm(".option arch, +c\n");
		asm volatile("vsetvli x0, x0, e8, m8, tu, mu\n");

		while (1) {
			asm volatile ("c.ebreak");
			asm volatile("csrr %[vtype], vtype" : [vtype] "=r"(vtype) : :);
			asm volatile ("c.ebreak");
		}
	} else {
		struct __riscv_v_regset_state *regset_data;
		struct user_regs_struct regs;
		size_t regset_size;
		struct iovec iov;
		int status;

		/* attach */

		ASSERT_EQ(0, ptrace(PTRACE_ATTACH, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* unlock */

		ASSERT_EQ(0, ptrace(PTRACE_POKEDATA, pid, &chld_lock, 0));

		/* resume and wait for the 1st c.ebreak */

		ASSERT_EQ(0, ptrace(PTRACE_CONT, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* read tracee vector csr regs using ptrace GETREGSET */

		regset_size = sizeof(*regset_data) + vlenb * 32;
		regset_data = calloc(1, regset_size);

		iov.iov_base = regset_data;
		iov.iov_len = regset_size;

		ASSERT_EQ(0, ptrace(PTRACE_GETREGSET, pid, NT_RISCV_VECTOR, &iov));

		/* set invalid vtype 0x1d = (5 | 3 << 3):
		 * - LMUL: 1/8
		 * - SEW:  64
		 *   - invalid configuration for VLENB <= 128
		 */
		regset_data->vtype = 0x1d;
		ASSERT_EQ(0, ptrace(PTRACE_SETREGSET, pid, NT_RISCV_VECTOR, &iov));

		/* skip 1st c.ebreak, then resume and wait for the 2nd c.ebreak */

		iov.iov_base = &regs;
		iov.iov_len = sizeof(regs);

		ASSERT_EQ(0, ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov));
		regs.pc += 2;
		ASSERT_EQ(0, ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov));

		ASSERT_EQ(0, ptrace(PTRACE_CONT, pid, NULL, NULL));
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFSTOPPED(status));

		/* read tracee vtype using ptrace GETREGSET */

		iov.iov_base = regset_data;
		iov.iov_len = regset_size;
		ASSERT_EQ(0, ptrace(PTRACE_GETREGSET, pid, NT_RISCV_VECTOR, &iov));

		/* read tracee vtype ptrace PEEKDATA */

		errno = 0;
		reg = ptrace(PTRACE_PEEKDATA, pid, &vtype, NULL);
		ASSERT_FALSE((errno != 0) && (reg == -1));

		/* verify that V state is illegal */

		EXPECT_EQ(reg, regset_data->vtype);
		EXPECT_EQ(1UL, (regset_data->vtype >> (__riscv_xlen - 1)));

		/* cleanup */

		ASSERT_EQ(0, kill(pid, SIGKILL));
	}
}

TEST_HARNESS_MAIN
