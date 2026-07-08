/* SPDX-License-Identifier: GPL-2.0 */

/* Various wrappers to make the kernel .S file build in user-space: */

// memcpy_orig is being defined as SYM_L_LOCAL but we need it
#define SYM_FUNC_START_LOCAL(name)                      \
        SYM_START(name, SYM_L_GLOBAL, SYM_A_ALIGN)
#define memcpy MEMCPY /* don't hide glibc's memcpy() */
#define altinstr_replacement text
#define globl p2align 4; .globl
#define _ASM_EXTABLE_FAULT(x, y)
#define _ASM_EXTABLE(x, y)

#include "../../arch/x86/lib/memcpy_64.S"
/*
 * We need to provide note.GNU-stack section, saying that we want
 * NOT executable stack. Otherwise the final linking will assume that
 * the ELF stack should not be restricted at all and set it RWX.
 */
.section .note.GNU-stack,"",@progbits

/*
 * We need to provide .note.gnu.property section with IBT and SHSTK,
 * saying that this object supports the same CET properties as C code
 * built with -fcf-protection. Otherwise static linking drops CET from
 * the final perf binary when other objects are CET-enabled.
 */
	.pushsection .note.gnu.property, "a"
	.p2align 3
	.long 1f - 0f
	.long 4f - 1f
	.long 5
0:
	.asciz "GNU"
1:
	.p2align 3
	.long 0xc0000002
	.long 3f - 2f
2:
	.long 3
3:
	.p2align 3
4:
	.popsection
