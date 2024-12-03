// SPDX-License-Identifier: GPL-2.0
#include <sys/ucontext.h>
#define __FRAME_OFFSETS
#include <asm/ptrace.h>
#include <sysdep/ptrace.h>
#include <sysdep/mcontext.h>
#include <sysdep/syscalls.h>

void get_regs_from_mc(struct uml_pt_regs *regs, mcontext_t *mc)
{
#ifdef __i386__
#define COPY2(X,Y) regs->gp[X] = mc->gregs[REG_##Y]
#define COPY(X) regs->gp[X] = mc->gregs[REG_##X]
#define COPY_SEG(X) regs->gp[X] = mc->gregs[REG_##X] & 0xffff;
#define COPY_SEG_CPL3(X) regs->gp[X] = (mc->gregs[REG_##X] & 0xffff) | 3;
	COPY_SEG(GS); COPY_SEG(FS); COPY_SEG(ES); COPY_SEG(DS);
	COPY(EDI); COPY(ESI); COPY(EBP);
	COPY2(UESP, ESP); /* sic */
	COPY(EBX); COPY(EDX); COPY(ECX); COPY(EAX);
	COPY(EIP); COPY_SEG_CPL3(CS); COPY(EFL); COPY_SEG_CPL3(SS);
#else
#define COPY2(X,Y) regs->gp[X/sizeof(unsigned long)] = mc->gregs[REG_##Y]
#define COPY(X) regs->gp[X/sizeof(unsigned long)] = mc->gregs[REG_##X]
	COPY(R8); COPY(R9); COPY(R10); COPY(R11);
	COPY(R12); COPY(R13); COPY(R14); COPY(R15);
	COPY(RDI); COPY(RSI); COPY(RBP); COPY(RBX);
	COPY(RDX); COPY(RAX); COPY(RCX); COPY(RSP);
	COPY(RIP);
	COPY2(EFLAGS, EFL);
	COPY2(CS, CSGSFS);
	regs->gp[CS / sizeof(unsigned long)] &= 0xffff;
	regs->gp[CS / sizeof(unsigned long)] |= 3;
#endif
}

#ifndef CONFIG_MMU
static void userspace_sigreturn(void)
{
	__asm__ volatile("movq $15, %rax");
	__asm__ volatile("call *%0" : : "r"(__kernel_vsyscall) :);
}

void mc_set_regs_ip_relay(mcontext_t *mc)
{
	mc->gregs[REG_RIP] = (unsigned long) userspace_sigreturn;
}

void mc_set_sigsys_hook(mcontext_t *mc)
{
	mc->gregs[REG_RSP] -= sizeof(unsigned long);
	*((unsigned long *) (mc->gregs[REG_RSP])) = mc->gregs[REG_RIP];
	mc->gregs[REG_RCX] = mc->gregs[REG_RIP];
	mc->gregs[REG_RIP] = (unsigned long) __kernel_vsyscall;
}
#endif
