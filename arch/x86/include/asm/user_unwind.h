/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_USER_UNWIND_H
#define _ASM_X86_USER_UNWIND_H

#define ARCH_INIT_USER_FP_FRAME						\
	.ra_off		= sizeof(long) * -1,				\
	.cfa_off	= sizeof(long) * 2,				\
	.fp_off		= sizeof(long) * -2,				\
	.use_fp		= true,

#endif /* _ASM_X86_USER_UNWIND_H */
