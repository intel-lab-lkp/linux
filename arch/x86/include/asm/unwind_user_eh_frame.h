/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UNWIND_USER_EH_FRAME_H
#define _ASM_X86_UNWIND_USER_EH_FRAME_H

#ifdef CONFIG_X86_64

#define EH_FRAME_REG_SP	7	/* designated stack pointer register */
#define EH_FRAME_REG_FP	6	/* designated frame pointer register */
#define EH_FRAME_REG_RA	16	/* (pseudo) return address register */

/* Instructions must be 1-byte aligned */
#define EH_FRAME_MAX_CODE_ALIGN 1

/* Stack grows towards lower addresses and SP must be 8-byte aligned */
#define EH_FRAME_MIN_DATA_ALIGN -8
#define EH_FRAME_MAX_DATA_ALIGN -1

#endif

#include <asm-generic/unwind_user_eh_frame.h>

#endif /* _ASM_X86_UNWIND_USER_EH_FRAME_H */
