/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_S390_UNWIND_USER_EH_FRAME_H
#define _ASM_S390_UNWIND_USER_EH_FRAME_H

#define EH_FRAME_REG_SP	15	/* designated stack pointer register */
#define EH_FRAME_REG_FP	11	/* "preferred" frame pointer register */
#define EH_FRAME_REG_RA	14	/* designaged return address register */

/* Instructions must be 2-byte aligned */
#define EH_FRAME_MAX_CODE_ALIGN 2

/* CFA is defined as SP at call site + 160, so that SP = CFA - 160 */
#define EH_FRAME_SP_VAL_OFFSET -160

/* SP may be saved on the stack or in a register */
static inline bool eh_frame_reject_sp_rule(void)
{
	return false;
}
#define eh_frame_reject_sp_rule eh_frame_reject_sp_rule

#include <asm-generic/unwind_user_eh_frame.h>

#endif /* _ASM_S390_UNWIND_USER_EH_FRAME_H */
