// SPDX-License-Identifier: GPL-2.0
/*
 * umip_leak_seg.c - Test for user space retrieving a DPL=0 segment base
 * address via UMIP instruction decoding/emulation.
 *
 * User space executing opcode SGDT on a UMIP-enabled CPU results in
 * #GP(0). In an effort to support legacy applications, #GP handler calls
 * fixup_umip_exception() to patch up the exception and return a dummy
 * value.
 *
 * SGDT is emulated by decoding the instruction and copying dummy data to
 * the memory address specified by the operand:
 *
 *	uaddr = insn_get_addr_ref(&insn, regs);
 *	if ((unsigned long)uaddr == -1L)
 *		return false;
 *
 *	nr_copied = copy_to_user(uaddr, dummy_data, dummy_data_size);
 *	if (nr_copied  > 0) {
 *		/ *
 *		 * If copy fails, send a signal and tell caller that
 *		 * fault was fixed up.
 *		 * /
 *		force_sig_info_umip_fault(uaddr, regs);
 *		return true;
 *	}
 *
 * Decoder handles segmentation, so for "sgdt %ss:(%rax)" the value of
 * `uaddr` will be correctly (in compatibility mode) shifted by the base
 * address of the segment. There's a catch though: decoder does not check
 * segment's DPL, nor its type.
 *
 * ptrace() can be used to prepare a RPL=3 selector for a S=0/DPL=0
 * segment, potentially one with a kernel space base address. On return to
 * user space, CPU will reject such selector load; exception will be
 * raised. But because the #GP handler sees no distinction between
 * SGDT-induced #GP(0) and IRET-induced #GP(selector), emulator will kick
 * in and process the malformed @regs->ss.
 *
 * If the first 4 GiB of user space are unmapped or non-writable,
 * copy_to_user() will fail, and signal to user will reveal `uaddr` -- i.e.
 * the (part of) kernel address. On x86_64, addresses of GDT_ENTRY_TSS (for
 * each CPU) and GDT_ENTRY_LDT (when in use) can be fully leaked in a few
 * steps.
 *
 * This selftest makes sure that selectors belonging to kernel are not
 * passed to UMIP emulation logic on #GP. CPU#0's TSS and the current
 * task's LDT are tried for that. Code is compiled with
 * -Ttext-segment=0x100000000 to reserve the initial 4 GiB, so that
 * SIGSEGV's siginfo_t::si_addr can be reliably caught. As an alternative
 * to ptrace(), sigreturn() is used for setting illegal selector value.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <assert.h>
#include <sched.h>
#include <errno.h>
#include <err.h>

#include <sys/syscall.h>
#include <sys/user.h>
#include <asm/ldt.h>

/* grep arch/x86/include/asm/segment.h */
#define GDT_ENTRY_DEFAULT_USER32_CS	4
#define GDT_ENTRY_TSS			8
#define GDT_ENTRY_LDT			10

#define USER_RPL	3

static int umip_leak_seg;
static int umip_leak_ax;
static long umip_leak_kmem;
static sigjmp_buf buf;

static __attribute__((naked)) void sgdt(void)
{
	asm volatile ("sgdt %ss:(%rax)");
}

/*
 * Make sure the first 4 GiB are not mapped.
 */
static void check_vm(void)
{
	unsigned long start;
	FILE *f;

	f = fopen("/proc/self/maps", "r");
	if (!f)
		err(1, "fopen");

	if (fscanf(f, "%lx-", &start) != 1 || start < (1UL << 32))
		errx(1, "First 4 GiB need to be unmapped");

	fclose(f);
}

/*
 * arch/x86/kernel/ldt.c:
 *	if (alloc_size > PAGE_SIZE)
 *		new_ldt->entries = __vmalloc(alloc_size, GFP_KERNEL_ACCOUNT | __GFP_ZERO);
 *	else
 *		new_ldt->entries = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);
 */
static void force_ldt_vmalloc(void)
{
	struct user_desc desc = { .entry_number = LDT_ENTRIES - 1 };
	int ret;

	ret = syscall(SYS_modify_ldt, 1, &desc, sizeof(desc));
	if (ret) {
		errno = -ret;
		err(1, "modify_ldt");
	}
}

static void set_handler(int sig, void (*handler)(int, siginfo_t *, void *))
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));

	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);

	if (sigaction(sig, &sa, 0))
		err(1, "sigaction");
}

static void sigusr1(int sig, siginfo_t *info, void *ctx_void)
{
	ucontext_t *ctx = (ucontext_t *)ctx_void;
	struct selectors {
		unsigned short cs, gs, fs, ss;
	} *sel = (void *)&ctx->uc_mcontext.gregs[REG_CSGSFS];

	sel->cs = (GDT_ENTRY_DEFAULT_USER32_CS << 3) | USER_RPL;
	sel->ss = (umip_leak_seg << 3) | USER_RPL;
	ctx->uc_mcontext.gregs[REG_RAX] = umip_leak_ax;
	ctx->uc_mcontext.gregs[REG_RIP] = (greg_t)&sgdt;
}

static void sigsegv(int sig, siginfo_t *info, void *ctx_void)
{
	assert((intptr_t)info->si_addr >> 32 == 0);

	if (info->si_code & SI_KERNEL)
		umip_leak_kmem = -1L;
	else
		umip_leak_kmem = (intptr_t)info->si_addr;

	siglongjmp(buf, 1);
}

static long leak(int seg, int ax)
{
	umip_leak_seg = seg;
	umip_leak_ax = ax;

	if (sigsetjmp(buf, 1))
		return umip_leak_kmem;

	raise(SIGUSR1);
	assert(!"unreachable");
}

static long find_limit(int seg)
{
	int limit = 0xffff;

	/* Assuming desc::g and desc::limit1 are zero. */
	while (limit >= 0 && leak(seg, limit) < 0)
		limit--;

	return limit;
}

static int fetch_base(char *seg_name, int seg)
{
	long base = leak(seg, 0);

	if (base != -1) {
		/*
		 * We're aiming here at a long mode segment descriptor that's
		 * taking two legacy-mode-sized entries in the GDT.
		 * Base Address[63:32] of n-th entry will be leaked in two
		 * steps: from Segment Limit[15:0] and Base Address[15:0] of
		 * (n+1)-th entry.
		 */
		base |= find_limit(seg + 1) << 32;
		base |= leak(seg + 1, 0) << 48;

		printf("[FAIL]\t%s base leaked: %#zx\n", seg_name, base);
		return 1;
	}

	printf("[OK]\t%s base: no leaks\n", seg_name);
	return 0;
}

static int dump_TSS(void)
{
	cpu_set_t old, new;
	int ret;

	if (sched_getaffinity(0, sizeof(old), &old))
		err(1, "sched_getaffinity");

	CPU_ZERO(&new);
	CPU_SET(0, &new);

	if (sched_setaffinity(0, sizeof(new), &new))
		err(1, "sched_setaffinity");

	ret = fetch_base("CPU#0 TSS", GDT_ENTRY_TSS);

	if (sched_setaffinity(0, sizeof(old), &old))
		err(1, "sched_setaffinity restore");

	return ret;
}

static int dump_LDT(void)
{
	force_ldt_vmalloc();
	return fetch_base("LDT", GDT_ENTRY_LDT);
}

int main(void)
{
	int status;

	check_vm();

	set_handler(SIGUSR1, sigusr1);
	set_handler(SIGSEGV, sigsegv);

	status  = dump_TSS();
	status |= dump_LDT();

	return status;
}
