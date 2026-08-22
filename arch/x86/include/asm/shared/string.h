/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_STRING_H
#define _ASM_X86_SHARED_STRING_H

/*
 * This inline memcmp() returns 0 (equal) or 1 (not equal).
 * The regular memcmp() returns <0 (less than), 0 (equal), or >0 (greater than)
 * to indicate ordering as well.
 */
static __always_inline int __inline_memcmp(const void *s1, const void *s2, size_t len)
{
	bool diff;

	/*
	 * Make sure ZF is properly set in the len==0 case because in it,
	 * RCX==0 and the REPE; CMPSB won't get executed.
	 */
	asm volatile("test %3, %3\n\t"
		     "repe cmpsb"
		     : "=@ccnz" (diff), "+D" (s1), "+S" (s2), "+c" (len)
		     : : "memory");

	return diff;
}

#endif /* _ASM_X86_SHARED_STRING_H */
