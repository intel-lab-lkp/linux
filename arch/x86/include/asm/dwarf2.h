/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_DWARF2_H
#define _ASM_X86_DWARF2_H

#ifndef __ASSEMBLER__
#warning "asm/dwarf2.h should be only included in pure assembly files"
#endif

#ifdef BUILD_VDSO

	/*
	 * For the vDSO, emit both runtime unwind information and debug
	 * symbols for the .dbg file.
	 */

	.cfi_sections .eh_frame, .debug_frame

#define CFI_STARTPROC		.cfi_startproc
#define CFI_ENDPROC		.cfi_endproc
#define CFI_DEF_CFA		.cfi_def_cfa
#define CFI_DEF_CFA_REGISTER	.cfi_def_cfa_register
#define CFI_DEF_CFA_OFFSET	.cfi_def_cfa_offset
#define CFI_ADJUST_CFA_OFFSET	.cfi_adjust_cfa_offset
#define CFI_OFFSET		.cfi_offset
#define CFI_REL_OFFSET		.cfi_rel_offset
#define CFI_REGISTER		.cfi_register
#define CFI_RESTORE		.cfi_restore
#define CFI_REMEMBER_STATE	.cfi_remember_state
#define CFI_RESTORE_STATE	.cfi_restore_state
#define CFI_UNDEFINED		.cfi_undefined
#define CFI_ESCAPE		.cfi_escape

#else /* !BUILD_VDSO */

/*
 * On x86, these macros aren't used outside VDSO.  As well they shouldn't be:
 * they're fragile and very difficult to maintain.
 */

.macro nocfi args:vararg
.endm

#define CFI_STARTPROC		nocfi
#define CFI_ENDPROC		nocfi
#define CFI_DEF_CFA		nocfi
#define CFI_DEF_CFA_REGISTER	nocfi
#define CFI_DEF_CFA_OFFSET	nocfi
#define CFI_ADJUST_CFA_OFFSET	nocfi
#define CFI_OFFSET		nocfi
#define CFI_REL_OFFSET		nocfi
#define CFI_REGISTER		nocfi
#define CFI_RESTORE		nocfi
#define CFI_REMEMBER_STATE	nocfi
#define CFI_RESTORE_STATE	nocfi
#define CFI_UNDEFINED		nocfi
#define CFI_ESCAPE		nocfi

#endif /* !BUILD_VDSO */

#endif /* _ASM_X86_DWARF2_H */
