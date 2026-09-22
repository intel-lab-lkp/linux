// SPDX-License-Identifier: GPL-2.0-only
/*
 * Check that PTRACE_SYSEMU_SINGLESTEP does not report a syscall-exit-stop.
 *
 * ptrace(2), "Syscall-stops":
 *
 *	If the tracee is continued using any other method (including
 *	PTRACE_SYSEMU), no syscall-exit-stop occurs. Note that all mentions of
 *	PTRACE_SYSEMU apply equally to PTRACE_SYSEMU_SINGLESTEP.
 *
 * PTRACE_SYSEMU_SINGLESTEP sets SYSCALL_EMU and enables single-stepping, and
 * ptrace_resume() clears SYSCALL_TRACE for it. An architecture that gates its
 * syscall-exit report on "single-stepping is enabled" rather than on "the
 * tracer asked for syscall stops" reports an extra stop here.
 *
 * Detecting that extra stop takes some care. The report is emitted with
 * step=1, and ptrace_report_syscall_exit() then calls
 * user_single_step_report() rather than ptrace_report_syscall(), so it arrives
 * as a plain SIGTRAP: PTRACE_O_TRACESYSGOOD and PTRACE_GET_SYSCALL_INFO cannot
 * tell it from the legitimate single-step trap. What distinguishes it is the
 * PC, which has not yet moved past the syscall instruction. So this asserts
 * that the first stop after the syscall-entry stop is at a different PC.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <linux/elf.h>
#include <linux/ptrace.h>
#include "../kselftest_harness.h"

/*
 * Not every libc declares these, even where the kernel implements them (an
 * AArch32 tracer on an arm64 kernel, for instance). The values are ABI.
 */
#ifndef PTRACE_SYSEMU
#define PTRACE_SYSEMU			31
#endif
#ifndef PTRACE_SYSEMU_SINGLESTEP
#define PTRACE_SYSEMU_SINGLESTEP	32
#endif

/*
 * Reading the PC portably would need one regset layout per architecture. Cover
 * the ones that implement PTRACE_SYSEMU_SINGLESTEP and can be checked here;
 * elsewhere the test skips rather than failing the build or the run.
 */
#if defined(__aarch64__)
#include <asm/ptrace.h>
static int tracee_pc(pid_t pid, unsigned long long *pc)
{
	struct user_pt_regs regs;
	struct iovec iov = { &regs, sizeof(regs) };

	if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov))
		return -1;
	*pc = regs.pc;
	return 0;
}
#elif defined(__arm__)
static int tracee_pc(pid_t pid, unsigned long long *pc)
{
	unsigned long uregs[18];	/* struct user_regs; uregs[15] is PC */
	struct iovec iov = { uregs, sizeof(uregs) };

	if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov))
		return -1;
	*pc = uregs[15];
	return 0;
}
#elif defined(__x86_64__) || defined(__i386__)
#include <sys/user.h>
static int tracee_pc(pid_t pid, unsigned long long *pc)
{
	struct user_regs_struct regs;
	struct iovec iov = { &regs, sizeof(regs) };

	if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov))
		return -1;
#if defined(__x86_64__)
	*pc = regs.rip;
#else
	*pc = regs.eip;
#endif
	return 0;
}
#else
static int tracee_pc(pid_t pid, unsigned long long *pc)
{
	return -1;
}
#define NO_PC_ACCESSOR	1
#endif

FIXTURE(sysemu)
{
	pid_t tracee;
};

FIXTURE_SETUP(sysemu)
{
	self->tracee = -1;
}

FIXTURE_TEARDOWN(sysemu)
{
	/* Reap the tracee however the test ended, including on assert. */
	if (self->tracee > 0) {
		kill(self->tracee, SIGKILL);
		waitpid(self->tracee, NULL, 0);
	}
}

TEST_F(sysemu, no_syscall_exit_stop)
{
	unsigned long long entry_pc = 0, next_pc = 0;
	int status;

#ifdef NO_PC_ACCESSOR
	SKIP(return, "no PC accessor for this architecture");
#endif

	self->tracee = fork();
	ASSERT_GE(self->tracee, 0);
	if (self->tracee == 0) {
		if (ptrace(PTRACE_TRACEME, 0, 0, 0))
			_exit(1);
		raise(SIGSTOP);
		/* A raw syscall: a libc wrapper may not issue one. */
		syscall(__NR_getpid);
		_exit(0);
	}

	ASSERT_EQ(waitpid(self->tracee, &status, 0), self->tracee);
	ASSERT_TRUE(WIFSTOPPED(status));
	ASSERT_EQ(ptrace(PTRACE_SETOPTIONS, self->tracee, 0,
			 (void *)PTRACE_O_TRACESYSGOOD), 0);

	/*
	 * PTRACE_SYSEMU reaches the syscall-entry stop in one resume. Getting
	 * there by single-stepping would cost one stop per instruction.
	 */
	if (ptrace(PTRACE_SYSEMU, self->tracee, 0, 0)) {
		if (errno == EIO || errno == EINVAL)
			SKIP(return, "PTRACE_SYSEMU not supported");
		ASSERT_EQ(errno, 0) TH_LOG("PTRACE_SYSEMU: %m");
	}
	ASSERT_EQ(waitpid(self->tracee, &status, 0), self->tracee);
	ASSERT_TRUE(WIFSTOPPED(status));
	ASSERT_TRUE(WSTOPSIG(status) & 0x80)
		TH_LOG("expected a syscall-entry stop, got signal %d",
		       WSTOPSIG(status));
	ASSERT_EQ(tracee_pc(self->tracee, &entry_pc), 0);

	/* The resume under test. */
	if (ptrace(PTRACE_SYSEMU_SINGLESTEP, self->tracee, 0, 0)) {
		if (errno == EIO || errno == EINVAL)
			SKIP(return, "PTRACE_SYSEMU_SINGLESTEP not supported");
		ASSERT_EQ(errno, 0) TH_LOG("PTRACE_SYSEMU_SINGLESTEP: %m");
	}
	ASSERT_EQ(waitpid(self->tracee, &status, 0), self->tracee);
	ASSERT_TRUE(WIFSTOPPED(status));
	ASSERT_EQ(tracee_pc(self->tracee, &next_pc), 0);

	TH_LOG("entry stop pc=%#llx, next stop sig=%d pc=%#llx",
	       entry_pc, WSTOPSIG(status), next_pc);

	EXPECT_NE(next_pc, entry_pc)
		TH_LOG("redundant syscall-exit-stop reported at the syscall PC");
}

TEST_HARNESS_MAIN
