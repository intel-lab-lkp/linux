/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UNWIND_USER_EH_FRAME_H
#define _ASM_X86_UNWIND_USER_EH_FRAME_H

#include <linux/unwind_user_eh_frame_types.h>

#ifdef CONFIG_X86_64

#define EH_FRAME_REG_SP	7	/* designated stack pointer register */
#define EH_FRAME_REG_FP	6	/* designated frame pointer register */
#define EH_FRAME_REG_RA	16	/* (pseudo) return address register */

/* Instructions must be 1-byte aligned */
#define EH_FRAME_MAX_CODE_ALIGN 1

/* Stack grows towards lower addresses and SP must be 8-byte aligned */
#define EH_FRAME_MIN_DATA_ALIGN -8
#define EH_FRAME_MAX_DATA_ALIGN -1

#endif /* CONFIG_X86_64 */

static inline int memcmp_masked(const void *s1, const void *s2,
				const void *mask, size_t n)
{
	const unsigned char *p1 = s1, *p2 = s2, *m = mask;
	int res = 0;

	while (n--)
		if ((res = (*p1++ ^ *p2++) & *m++))
			break;

	return res;
}

static inline int eh_frame_do_def_cfa_expression(const char *expr,
						 int size,
						 unsigned long ip,
						 struct eh_frame_reg_state *reg_state)
{
	/*
	 * PLT CFA expression:
	 *
	 * DW_OP_breg<SP> + <SP_offset>	// 4 (ESP) + 4 or 7 (RSP) + 8
	 * DW_OP_breg<IP> + 0		// 8 (EIP) or 16 (RIP)
	 * DW_OP_lit15
	 * DW_OP_and
	 * DW_OP_lit<N>
	 * DW_OP_ge
	 * DW_OP_lit<shift>		// 2 or 3
	 * DW_OP_shl
	 * DW_OP_plus
	 *
	 * CFA = (SP + offset) + (((IP & 0xf) >= N) << shift)
	 */
	static const char plt_expr[] = {0x00,0x00,0x00,0x00,0x3f,0x1a,0x30,0x2a,0x30,0x24,0x22};
	static const char plt_mask[] = {0x00,0xf0,0x00,0xff,0xff,0xff,0xf0,0xff,0xf0,0xff,0xff};

	if (size == sizeof(plt_expr) &&
	    !memcmp_masked(expr, plt_expr, plt_mask, sizeof(plt_expr))) {
		unsigned char sp_op = expr[0];
		unsigned char sp_offset = expr[1] & 0x0f;
		unsigned char ip_op = expr[2];
		unsigned char n = DW_OP_lit_value(expr[6]);
		unsigned char shift = DW_OP_lit_value(expr[8]);
		unsigned char sp_reg, ip_reg;

		if (!DW_OP_is_breg(sp_op) || !DW_OP_is_breg(ip_op))
			return -EOPNOTSUPP;

		sp_reg = DW_OP_breg_register(sp_op);
		ip_reg = DW_OP_breg_register(ip_op);
		if (sp_reg != EH_FRAME_REG_SP || ip_reg != EH_FRAME_REG_RA)
			return -EOPNOTSUPP;

		/* CFA = (SP + SP_offset) + (((IP & 0xf) >= N) << shift) */
		reg_state->cfa_rule = CFA_REG_OFFSET;
		reg_state->cfa_regnum = EH_FRAME_REG_SP;
		reg_state->cfa_offset = sp_offset + (((ip & 15) >= n) << shift);
		return 0;
	}

	return -EOPNOTSUPP;
}
#define eh_frame_do_def_cfa_expression eh_frame_do_def_cfa_expression

#include <asm-generic/unwind_user_eh_frame.h>

#endif /* _ASM_X86_UNWIND_USER_EH_FRAME_H */
