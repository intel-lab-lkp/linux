/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_EXTABLE_H
#define __VDSO_EXTABLE_H

/*
 * Inject exception fixup for vDSO code.  Unlike normal exception fixup,
 * vDSO uses a dedicated handler the addresses are relative to the overall
 * exception table, not each individual entry.
 */
#ifdef __ASSEMBLER__
#define _ASM_VDSO_EXTABLE_FIXUP_HANDLE(from, to)	\
	ASM_VDSO_EXTABLE_FIXUP_HANDLE from to

.macro ASM_VDSO_EXTABLE_FIXUP_HANDLE from:req to:req
	.pushsection __ex_table, "a"
	.long 0		/* type: fixup */
	.long (\from) - __ex_table
	.long (\to) - __ex_table
	.popsection
.endm
#else
#define _ASM_VDSO_EXTABLE_FIXUP_HANDLE(from, to)	\
	".pushsection __ex_table, \"a\"\n"      	\
	".long 0\n"	/* type: fixup */		\
	".long (" #from ") - __ex_table\n"      	\
	".long (" #to ") - __ex_table\n"        	\
	".popsection\n"

/*
 * Identify robust futex unlock critical section.
 */
#define _ASM_VDSO_EXTABLE_FUTEX_HANDLE(start, end)	\
	".pushsection __ex_table, \"a\"\n"      	\
	".long 1\n"	/* type: futex */		\
	".long (" #start ") - __ex_table\n"      	\
	".long (" #end ") - __ex_table\n"        	\
	".popsection\n"

/*
 * Identify robust PI futex unlock critical section.
 */
#define _ASM_VDSO_EXTABLE_PI_FUTEX_HANDLE(start, end)	\
	".pushsection __ex_table, \"a\"\n"      	\
	".long 2\n"	/* type: pi_futex */		\
	".long (" #start ") - __ex_table\n"      	\
	".long (" #end ") - __ex_table\n"        	\
	".popsection\n"

#endif

#endif /* __VDSO_EXTABLE_H */
