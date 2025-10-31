/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RUNTIME_CONST_H
#define _ASM_RUNTIME_CONST_H

#include <asm/runtime-const-accessors.h>

#ifndef __ASSEMBLY__
#define runtime_const_init(type, sym) do {		\
	extern s32 __start_runtime_##type##_##sym[];	\
	extern s32 __stop_runtime_##type##_##sym[];	\
	runtime_const_fixup(__runtime_fixup_##type,	\
		(unsigned long)(sym), 			\
		__start_runtime_##type##_##sym,		\
		__stop_runtime_##type##_##sym);		\
} while (0)

/*
 * The text patching is trivial - you can only do this at init time,
 * when the text section hasn't been marked RO, and before the text
 * has ever been executed.
 */
static inline void __runtime_fixup_ptr(void *where, unsigned long val)
{
	*(unsigned long *)where = val;
}

static inline void __runtime_fixup_shift(void *where, unsigned long val)
{
	*(unsigned char *)where = val;
}

static inline void runtime_const_fixup(void (*fn)(void *, unsigned long),
	unsigned long val, s32 *start, s32 *end)
{
	while (start < end) {
		fn(*start + (void *)start, val);
		start++;
	}
}

#endif /* !__ASSEMBLY__ */
#endif
