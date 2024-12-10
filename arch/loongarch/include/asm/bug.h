/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_BUG_H
#define __ASM_BUG_H

#include <asm/break.h>
#include <linux/stringify.h>

#ifndef CONFIG_DEBUG_BUGVERBOSE
#define _BUGVERBOSE_LOCATION(file, line)
#else
#define __BUGVERBOSE_LOCATION(file, line)			\
		.pushsection .rodata.str, "aMS", @progbits, 1;	\
	10002:	.string file;					\
		.popsection;					\
								\
		.long 10002b - .;				\
		.short line;
#define _BUGVERBOSE_LOCATION(file, line) __BUGVERBOSE_LOCATION(file, line)
#endif

#ifndef CONFIG_GENERIC_BUG
#define __BUG_ENTRY(flags)
#else

#define __BUG_ENTRY_START					\
		.pushsection __bug_table, "aw";			\
		.align 2;					\
	10000:	.long 10001f - .;				\

#define __BUG_ENTRY_END						\
		.popsection;					\
	10001:

#define __BUG_ENTRY(flags)					\
		__BUG_ENTRY_START			\
		_BUGVERBOSE_LOCATION(__FILE__, __LINE__)	\
		.short flags;					\
		__BUG_ENTRY_END
#endif

#define ASM_BUG_FLAGS(flags)					\
	__BUG_ENTRY(flags)					\
	break		BRK_BUG

#define ASM_BUG()	ASM_BUG_FLAGS(0)

#define __BUG_FLAGS(flags)					\
	asm_inline volatile (__stringify(ASM_BUG_FLAGS(flags)));

#define __WARN_FLAGS(flags)					\
do {								\
	instrumentation_begin();				\
	__BUG_FLAGS(BUGFLAG_WARNING|(flags));			\
	annotate_reachable();					\
	instrumentation_end();					\
} while (0)

#define BUG()							\
do {								\
	instrumentation_begin();				\
	__BUG_FLAGS(0);						\
	unreachable();						\
} while (0)

#ifdef CONFIG_DEBUG_BUGVERBOSE
#define __BUG_LOCATION_STRING(file, line)		\
		".long " file "- .;"			\
		".short " line ";"
#else
#define __BUG_LOCATION_STRING(_file, _line)
#endif

#define __BUG_ENTRY_STRING(file, line, flags)		\
		__stringify(__BUG_ENTRY_START)		\
		__BUG_LOCATION_STRING(file, line)	\
		".short " flags ";"			\
		__stringify(__BUG_ENTRY_END)

#define ARCH_WARN_ASM(file, line, flags, size)		\
	__BUG_ENTRY_STRING(file, line, flags)		\
	__stringify(break BRK_BUG) ";"

#define HAVE_ARCH_BUG

#include <asm-generic/bug.h>

#endif /* __ASM_BUG_H */
