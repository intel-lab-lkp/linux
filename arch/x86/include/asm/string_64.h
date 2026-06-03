/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_STRING_64_H
#define _ASM_X86_STRING_64_H

#ifdef __KERNEL__
#include <linux/jump_label.h>
#include <linux/align.h>

/* Written 2002 by Andi Kleen */

/* Even with __builtin_ the compiler may decide to use the out of line
   function. */

#if defined(__SANITIZE_MEMORY__) && defined(__NO_FORTIFY)
#include <linux/kmsan_string.h>
#endif

#define __HAVE_ARCH_MEMCPY 1
extern void *memcpy(void *to, const void *from, size_t len);
extern void *__memcpy(void *to, const void *from, size_t len);

#define __HAVE_ARCH_MEMSET
void *memset(void *s, int c, size_t n);
void *__memset(void *s, int c, size_t n);
KCFI_REFERENCE(__memset);

/*
 * KMSAN needs to instrument as much code as possible. Use C versions of
 * memsetXX() from lib/string.c under KMSAN.
 */
#if !defined(CONFIG_KMSAN)
#define __HAVE_ARCH_MEMSET16
static inline void *memset16(uint16_t *s, uint16_t v, size_t n)
{
	const auto s0 = s;
	asm volatile (
		"rep stosw"
		: "+D" (s), "+c" (n)
		: "a" (v)
		: "memory"
	);
	return s0;
}

#define __HAVE_ARCH_MEMSET32
static inline void *memset32(uint32_t *s, uint32_t v, size_t n)
{
	const auto s0 = s;
	asm volatile (
		"rep stosl"
		: "+D" (s), "+c" (n)
		: "a" (v)
		: "memory"
	);
	return s0;
}

#define __HAVE_ARCH_MEMSET64
static inline void *memset64(uint64_t *s, uint64_t v, size_t n)
{
	const auto s0 = s;
	asm volatile (
		"rep stosq"
		: "+D" (s), "+c" (n)
		: "a" (v)
		: "memory"
	);
	return s0;
}
#endif

#define __HAVE_ARCH_MEMMOVE
void *memmove(void *dest, const void *src, size_t count);
void *__memmove(void *dest, const void *src, size_t count);
KCFI_REFERENCE(__memmove);

int memcmp(const void *cs, const void *ct, size_t count);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
int strcmp(const char *cs, const char *ct);

#ifdef CONFIG_ARCH_HAS_UACCESS_FLUSHCACHE
#define __HAVE_ARCH_MEMCPY_FLUSHCACHE 1
void __memcpy_flushcache(void *dst, const void *src, size_t cnt);

/*
 * Only map memcpy_streaming() to memcpy_flushcache() when the destination
 * is already 8-byte aligned and the size can be handled without cached
 * head/tail fragments in __memcpy_flushcache().
 */
static __always_inline bool memcpy_flushcache_nt_safe(const void *dst,
						      size_t cnt)
{
	unsigned long d = (unsigned long)dst;

	return cnt && IS_ALIGNED(d, 8) && IS_ALIGNED(cnt, 4);
}

static __always_inline void memcpy_flushcache_4(void *dst, const void *src)
{
	asm volatile("movntil %1, %0"
		     : "=m"(*(u32 *)dst)
		     : "r"(*(const u32 *)src)
		     : "memory");
}

static __always_inline void memcpy_flushcache_8(void *dst, const void *src)
{
	asm volatile("movntiq %1, %0"
		     : "=m"(*(u64 *)dst)
		     : "r"(*(const u64 *)src)
		     : "memory");
}

static __always_inline void memcpy_flushcache_16(void *dst,
						 const void *src)
{
	memcpy_flushcache_8(dst, src);
	memcpy_flushcache_8(dst + 8, src + 8);
}

static __always_inline void memcpy_flushcache_32(void *dst,
						 const void *src)
{
	memcpy_flushcache_16(dst, src);
	memcpy_flushcache_16(dst + 16, src + 16);
}

static __always_inline void memcpy_flushcache_64(void *dst,
						 const void *src)
{
	memcpy_flushcache_32(dst, src);
	memcpy_flushcache_32(dst + 32, src + 32);
}

/*
 * Keep the additional aligned fixed-size cases on the inline movnti path.
 * Leave the existing 4/8/16-byte cases handled directly in
 * memcpy_flushcache() so they do not pick up the extra alignment gating
 * used by the larger fixed-size helpers.
 */
static __always_inline int memcpy_flushcache_large(void *dst,
						   const void *src,
						   size_t cnt)
{
	unsigned long d = (unsigned long)dst;
	char *dptr = dst;
	const char *sptr = src;

	if (!IS_ALIGNED(d, 8))
		return 0;

	switch (cnt) {
	case 32:
		memcpy_flushcache_32(dptr, sptr);
		return 1;
	case 48:
		memcpy_flushcache_32(dptr, sptr);
		memcpy_flushcache_16(dptr + 32, sptr + 32);
		return 1;
	case 64:
		memcpy_flushcache_64(dptr, sptr);
		return 1;
	case 80:
		memcpy_flushcache_64(dptr, sptr);
		memcpy_flushcache_16(dptr + 64, sptr + 64);
		return 1;
	case 96:
		memcpy_flushcache_64(dptr, sptr);
		memcpy_flushcache_32(dptr + 64, sptr + 64);
		return 1;
	}

	return 0;
}

static __always_inline void memcpy_flushcache(void *dst, const void *src,
					      size_t cnt)
{
	if (!cnt)
		return;

	if (__builtin_constant_p(cnt)) {
		switch (cnt) {
		case 4:
			memcpy_flushcache_4(dst, src);
			return;
		case 8:
			memcpy_flushcache_8(dst, src);
			return;
		case 16:
			memcpy_flushcache_16(dst, src);
			return;
		}

		if (memcpy_flushcache_large(dst, src, cnt))
			return;
	}

	__memcpy_flushcache(dst, src, cnt);
}

#define __HAVE_ARCH_MEMCPY_STREAMING 1
static __always_inline void memcpy_streaming(void *dst, const void *src,
					     size_t cnt)
{
	if (!cnt)
		return;

	if (memcpy_flushcache_nt_safe(dst, cnt))
		memcpy_flushcache(dst, src, cnt);
	else
		memcpy(dst, src, cnt);
}

static __always_inline void memcpy_streaming_drain(void)
{
	asm volatile("sfence" : : : "memory");
}
#endif

#endif /* __KERNEL__ */

#endif /* _ASM_X86_STRING_64_H */
