/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RUNTIME_CONST_ACCESSORS_H
#define _ASM_RUNTIME_CONST_ACCESSORS_H

#ifdef MODULE
#error "this functionality is not available for modules"
#endif

#ifdef __ASSEMBLY__

.macro RUNTIME_CONST_PTR sym reg
	movq	$0x0123456789abcdef, %\reg
	1:
	.pushsection runtime_ptr_\sym, "a"
	.long	1b - 8 - .
	.popsection
.endm

#else /* __ASSEMBLY__ */

#define runtime_const_ptr(sym) ({				\
	typeof(sym) __ret;					\
	asm_inline("mov %1,%0\n1:\n"				\
		".pushsection runtime_ptr_" #sym ",\"a\"\n\t"	\
		".long 1b - %c2 - .\n"				\
		".popsection"					\
		:"=r" (__ret)					\
		:"i" ((unsigned long)0x0123456789abcdefull),	\
		 "i" (sizeof(long)));				\
	__ret; })

// The 'typeof' will create at _least_ a 32-bit type, but
// will happily also take a bigger type and the 'shrl' will
// clear the upper bits
#define runtime_const_shift_right_32(val, sym) ({		\
	typeof(0u+(val)) __ret = (val);				\
	asm_inline("shrl $12,%k0\n1:\n"				\
		".pushsection runtime_shift_" #sym ",\"a\"\n\t"	\
		".long 1b - 1 - .\n"				\
		".popsection"					\
		:"+r" (__ret));					\
	__ret; })

#endif /* __ASSEMBLY__ */
#endif
