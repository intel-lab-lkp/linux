/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARCH_S390_ENTRY_PERCPU_H
#define ARCH_S390_ENTRY_PERCPU_H

#include <linux/percpu.h>
#include <asm/lowcore.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>

static __always_inline void percpu_entry(struct pt_regs *regs)
{
	struct lowcore *lc = get_lowcore();

	if (user_mode(regs))
		return;
	regs->cpu = lc->cpu_nr;
	regs->percpu_register = lc->percpu_register;
	lc->percpu_register = 0;
}

static __always_inline void percpu_exit(struct pt_regs *regs)
{
	struct lowcore *lc = get_lowcore();
	unsigned int insn, disp;
	unsigned char reg;

	if (user_mode(regs))
		return;
	reg = regs->percpu_register;
	if (!reg)
		return;
	lc->percpu_register = reg;
	if (regs->cpu == lc->cpu_nr)
		return;
	/*
	 * Within a percpu code section and process has been migrated to
	 * a different CPU. Check if the percpu base register needs to be
	 * updated. This is the case if the PSW does not point to the ADD
	 * instruction within the section
	 * - AG %rx,percpu_offset_in_lowcore(%r0,%r0)
	 * which adds the percpu offset to the percpu base register.
	 */
	if ((*(u16 *)psw_bits(regs->psw).ia & 0xff0f) != 0xe300)
		goto fixup;
	disp = offsetof(struct lowcore, percpu_offset);
	if (machine_has_relocated_lowcore())
		disp += LOWCORE_ALT_ADDRESS;
	insn = (disp & 0xff000) >> 4 | (disp & 0x00fff) << 16 | 0x8;
	if (*(u32 *)(psw_bits(regs->psw).ia + 2) != insn)
		return;
fixup:
	/* Fixup percpu base register */
	regs->gprs[reg] -= __per_cpu_offset[regs->cpu];
	regs->gprs[reg] += lc->percpu_offset;
}

#endif
