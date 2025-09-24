/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UNWIND_USER_H
#define _ASM_X86_UNWIND_USER_H

#define ARCH_INIT_USER_FP_FRAME				\
	.cfa_off	=  2,				\
	.ra_off		= -1,				\
	.fp_off		= -2,				\
	.use_fp		= true,

#endif /* _ASM_X86_UNWIND_USER_H */
