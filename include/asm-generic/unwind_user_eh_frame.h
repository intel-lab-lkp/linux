/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_UNWIND_USER_EH_FRAME_H
#define _ASM_GENERIC_UNWIND_USER_EH_FRAME_H

#ifndef EH_FRAME_MAX_CIE_LENGTH
#define EH_FRAME_MAX_CIE_LENGTH 128
#endif

#ifndef EH_FRAME_MAX_FDE_LENGTH
#define EH_FRAME_MAX_FDE_LENGTH 32768
#endif

#ifndef EH_FRAME_MAX_AUGSTR_LENGTH
#define EH_FRAME_MAX_AUGSTR_LENGTH 16
#endif

#ifndef EH_FRAME_MAX_STATE_STACK
#define EH_FRAME_MAX_STATE_STACK 1
#endif

#ifndef EH_FRAME_MAX_CODE_ALIGN
#define EH_FRAME_MAX_CODE_ALIGN 8
#endif

#ifndef EH_FRAME_MIN_DATA_ALIGN
#define EH_FRAME_MIN_DATA_ALIGN -8
#endif

#ifndef EH_FRAME_MAX_DATA_ALIGN
#define EH_FRAME_MAX_DATA_ALIGN -1
#endif

#ifndef EH_FRAME_CFI_INSN_LIMIT
#define EH_FRAME_CFI_INSN_LIMIT 16384
#endif

#ifndef EH_FRAME_SP_VAL_OFFSET
/* Most archs/ABIs define CFA as SP at call site, so that SP = CFA + 0 */
#define EH_FRAME_SP_VAL_OFFSET 0
#endif

#ifndef eh_frame_reject_sp_rule
static inline bool eh_frame_reject_sp_rule(void)
{
	return true;
}
#define eh_frame_reject_sp_rule eh_frame_reject_sp_rule
#endif

#endif /* _ASM_GENERIC_UNWIND_USER_EH_FRAME_H */

