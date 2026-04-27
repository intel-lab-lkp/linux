/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_STRING_H
#define _ASM_X86_STRING_H

#ifndef _SETUP
#ifdef CONFIG_X86_32
# include <asm/string_32.h>
#else
# include <asm/string_64.h>
#endif
#endif

static __always_inline void *__inline_memcpy(void *to, const void *from, size_t len)
{
	void *ret = to;

	asm volatile("rep movsb"
		     : "+D" (to), "+S" (from), "+c" (len)
		     : : "memory");
	return ret;
}

static __always_inline void *__inline_memset(void *s, int v, size_t n)
{
	void *ret = s;

	asm volatile("rep stosb"
		     : "+D" (s), "+c" (n)
		     : "a" ((uint8_t)v)
		     : "memory");
	return ret;
}

/* Note: this memcmp() only returns 0/1, not -1/0/1 as regular memcmp(). */
static __always_inline int __inline_memcmp(const void *s1, const void *s2, size_t len)
{
	bool diff;

	asm("repe cmpsb"
	    : "=@ccnz" (diff), "+D" (s1), "+S" (s2), "+c" (len));
	return diff;
}

#endif /* _ASM_X86_STRING_H */
