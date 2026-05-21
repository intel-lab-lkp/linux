/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_STRING_64_H
#define _ASM_X86_STRING_64_H

#ifdef __KERNEL__
#include <linux/jump_label.h>

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

static __always_inline void memcpy_flushcache_4(void *dst, const void *src)
{
	asm ("movntil %1, %0" : "=m"(*(u32 *)dst) : "r"(*(u32 *)src));
}

static __always_inline void memcpy_flushcache_8(void *dst, const void *src)
{
	asm ("movntiq %1, %0" : "=m"(*(u64 *)dst) : "r"(*(u64 *)src));
}

static __always_inline void memcpy_flushcache_16(void *dst, const void *src)
{
	memcpy_flushcache_8(dst, src);
	memcpy_flushcache_8(dst + 8, src + 8);
}

/*
 * Keep common fixed-size copies on the inline movnti path instead of
 * dropping into the generic helper.
 */
static __always_inline int memcpy_flushcache_small(void *dst, const void *src,
						   size_t cnt)
{
	switch (cnt) {
	case 96:
		memcpy_flushcache_16(dst + 80, src + 80);
		fallthrough;
	case 80:
		memcpy_flushcache_16(dst + 64, src + 64);
		fallthrough;
	case 64:
		memcpy_flushcache_16(dst + 48, src + 48);
		fallthrough;
	case 48:
		memcpy_flushcache_16(dst + 32, src + 32);
		fallthrough;
	case 32:
		memcpy_flushcache_16(dst + 16, src + 16);
		fallthrough;
	case 16:
		memcpy_flushcache_16(dst, src);
		return 1;

	case 88:
		memcpy_flushcache_16(dst + 72, src + 72);
		fallthrough;
	case 72:
		memcpy_flushcache_16(dst + 56, src + 56);
		fallthrough;
	case 56:
		memcpy_flushcache_16(dst + 40, src + 40);
		fallthrough;
	case 40:
		memcpy_flushcache_16(dst + 24, src + 24);
		fallthrough;
	case 24:
		memcpy_flushcache_16(dst + 8, src + 8);
		fallthrough;
	case 8:
		memcpy_flushcache_8(dst, src);
		return 1;

	case 4:
		memcpy_flushcache_4(dst, src);
		return 1;
	}

	return 0;
}

static __always_inline void memcpy_flushcache(void *dst, const void *src, size_t cnt)
{
	if (__builtin_constant_p(cnt) && memcpy_flushcache_small(dst, src, cnt))
		return;
	__memcpy_flushcache(dst, src, cnt);
}

#define __HAVE_ARCH_MEMCPY_STREAMING 1
static __always_inline void memcpy_streaming(void *dst, const void *src,
					     size_t cnt)
{
	memcpy_flushcache(dst, src, cnt);
}

static __always_inline void memcpy_streaming_drain(void)
{
	asm volatile("sfence" : : : "memory");
}

#endif

#endif /* __KERNEL__ */

#endif /* _ASM_X86_STRING_64_H */
