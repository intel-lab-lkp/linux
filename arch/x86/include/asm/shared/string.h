/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_STRING_H
#define _ASM_X86_SHARED_STRING_H

/* Note: this memcmp() returns 0/1, not -1/0/1 as regular memcmp(). */
static __always_inline int __inline_memcmp(const void *s1, const void *s2, size_t len)
{
	bool diff;

	asm("repe cmpsb"
	    : "=@ccnz" (diff), "+D" (s1), "+S" (s2), "+c" (len));

	return diff;
}

#endif /* _ASM_X86_SHARED_STRING_H */
