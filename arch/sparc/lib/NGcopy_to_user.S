/* SPDX-License-Identifier: GPL-2.0 */
/* NGcopy_to_user.S: Niagara optimized copy to userspace.
 *
 * Copyright (C) 2006, 2007 David S. Miller (davem@davemloft.net)
 */

#define EX_ST(x,y)		\
98:	x;			\
	.section __ex_table,"a";\
	.align 4;		\
	.word 98b, y;		\
	.text;			\
	.align 4;

#ifndef ASI_AIUS
#define ASI_AIUS	0x11
#endif

#define FUNC_NAME		NGcopy_to_user
#define STORE(type,src,addr)	type##a src, [addr] ASI_AIUS
#define STORE_ASI		ASI_BLK_INIT_QUAD_LDD_AIUS
#define EX_RETVAL(x)		%g0

#ifdef __KERNEL__
	/* Writing to %asi is _expensive_ so we hardcode it.
	 * Reading %asi to check for KERNEL_DS is comparatively
	 * cheap.
	 */
#define PREAMBLE					\
	rd		%asi, %g1;			\
	cmp		%g1, ASI_AIUS;			\
	be,pt		%icc, 1f;			\
	 nop;					\
	set		raw_copy_in_user, %g1;		\
	jmpl		%g1, %g0;			\
	 nop;					\
1:
#endif

#include "NGmemcpy.S"
