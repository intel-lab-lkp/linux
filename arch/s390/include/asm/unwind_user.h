/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_S390_UNWIND_USER_H
#define _ASM_S390_UNWIND_USER_H

#include <linux/sched/task_stack.h>
#include <linux/types.h>
#include <asm/fpu.h>

#ifdef CONFIG_UNWIND_USER

static inline int unwind_user_word_size(struct pt_regs *regs)
{
	return 8;
}

static inline int unwind_user_get_ra_reg(unsigned long *val)
{
	struct pt_regs *regs = task_pt_regs(current);
	*val = regs->gprs[14];
	return 0;
}
#define unwind_user_get_ra_reg unwind_user_get_ra_reg

static inline unsigned long __s390_dwarf_fpr_to_fpr(unsigned int regnum)
{
	unsigned int fpr;

	/*
	 * Convert from s390 DWARF floating-point register number (16..31)
	 * to floating-point register number (0..15): left rotate the least
	 * significant three bits and then return the least significant four
	 * bits.
	 */
	fpr  = (regnum & 3) << 1;
	fpr |= (regnum & 4) >> 2;
	fpr |= (regnum & 8);
	return fpr;
}

static inline unsigned long __s390_get_dwarf_fpr(unsigned int regnum)
{
	struct fpu *fpu = &current->thread.ufpu;

	save_user_fpu_regs();
	return fpu->vxrs[__s390_dwarf_fpr_to_fpr(regnum)].high;
}

static inline int unwind_user_get_reg(unsigned long *val, unsigned int regnum)
{
	if (regnum <= 15) {
		/* DWARF register numbers 0..15 */
		struct pt_regs *regs = task_pt_regs(current);
		*val = regs->gprs[regnum];
		return 0;
	} else if (regnum <= 31) {
		/* DWARF register numbers 16..31 */
		*val = __s390_get_dwarf_fpr(regnum);
		return 0;
	}

	pr_debug("%s (%d): %s(%u): unsupported register number\n",
		 current->comm, current->pid, __func__, regnum);
	return -EINVAL;
}
#define unwind_user_get_reg unwind_user_get_reg

#endif /* CONFIG_UNWIND_USER */

#include <asm-generic/unwind_user.h>

#endif /* _ASM_S390_UNWIND_USER_H */
