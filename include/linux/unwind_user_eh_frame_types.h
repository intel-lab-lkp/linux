/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_EH_FRAME_TYPES_H
#define _LINUX_UNWIND_USER_EH_FRAME_TYPES_H

enum eh_frame_cfa_rule {
	CFA_UNDEFINED,		/* unrecoverable */
	CFA_REG_OFFSET,		/* CFA = reg + offset */
};

enum eh_frame_reg_rule {
	REG_UNDEFINED_IMPLICIT,	/* reg = reg */
	REG_UNDEFINED_EXPLICIT,	/* unrecoverable; RA: outermost frame */
	REG_SAME_VALUE,		/* reg = reg; TODO: reset to CIE initial CFI */
	REG_OFFSET,		/* reg = *(CFA + offset) */
	REG_VAL_OFFSET,		/* reg = CFA + offset */
	REG_REGISTER,		/* reg = other_reg */
};

enum eh_frame_reg_index {
	FP_IDX,			/* frame pointer (FP) */
	RA_IDX,			/* return address (RA) */
	NR_REGS
};

struct eh_frame_reg_state {
	/* CFA recovery rule */
	enum eh_frame_cfa_rule cfa_rule;
	unsigned long cfa_regnum;
	long cfa_offset;

	/* FP and RA recovery rules (SP uses implicit recovery) */
	enum eh_frame_reg_rule reg_rule[NR_REGS];
	unsigned long reg_regnum[NR_REGS];
	long reg_offset[NR_REGS];
};

#endif /* _LINUX_UNWIND_USER_EH_FRAME_TYPES_H */
