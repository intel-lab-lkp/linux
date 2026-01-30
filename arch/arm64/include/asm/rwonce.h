/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Google LLC.
 */
#ifndef __ASM_RWONCE_H
#define __ASM_RWONCE_H

#if defined(CONFIG_LTO) && !defined(__ASSEMBLER__)

#include <linux/compiler_types.h>
#include <asm/alternative-macros.h>

#ifndef BUILD_VDSO

#define __LOAD_RCPC(sfx, regs...)					\
	ALTERNATIVE(							\
		"ldar"	#sfx "\t" #regs,				\
		".arch_extension rcpc\n"				\
		"ldapr"	#sfx "\t" #regs,				\
	ARM64_HAS_LDAPR)

#ifdef USE_TYPEOF_UNQUAL
#define __rwonce_typeof_unqual(x) TYPEOF_UNQUAL(x)
#else
/*
 * Fallback for older compilers (Clang < 19).
 *
 * Uses the fact that, for all supported Clang versions, 'auto' correctly drops
 * qualifiers. Unlike typeof_unqual(), the type must be completely defined, i.e.
 * no forward-declared struct pointer dereferences.  The array-to-pointer decay
 * case does not matter for usage in READ_ONCE() either.
 */
#define __rwonce_typeof_unqual(x) typeof(({ auto ____t = (x); ____t; }))
#endif

/*
 * When building with LTO, there is an increased risk of the compiler
 * converting an address dependency headed by a READ_ONCE() invocation
 * into a control dependency and consequently allowing for harmful
 * reordering by the CPU.
 *
 * Ensure that such transformations are harmless by overriding the generic
 * READ_ONCE() definition with one that provides RCpc acquire semantics
 * when building with LTO.
 */
#define __READ_ONCE(x)							\
({									\
	auto __x = &(x);						\
	auto __ret = (__rwonce_typeof_unqual(*__x) *)__x;		\
	/* Hides alias reassignment from Clang's -Wthread-safety. */	\
	auto __retp = &__ret;						\
	union { typeof(*__ret) __val; char __c[1]; } __u;		\
	*__retp = &__u.__val;						\
	switch (sizeof(x)) {						\
	case 1:								\
		asm volatile(__LOAD_RCPC(b, %w0, %1)			\
			: "=r" (*(__u8 *)__u.__c)			\
			: "Q" (*__x) : "memory");			\
		break;							\
	case 2:								\
		asm volatile(__LOAD_RCPC(h, %w0, %1)			\
			: "=r" (*(__u16 *)__u.__c)			\
			: "Q" (*__x) : "memory");			\
		break;							\
	case 4:								\
		asm volatile(__LOAD_RCPC(, %w0, %1)			\
			: "=r" (*(__u32 *)__u.__c)			\
			: "Q" (*__x) : "memory");			\
		break;							\
	case 8:								\
		asm volatile(__LOAD_RCPC(, %0, %1)			\
			: "=r" (*(__u64 *)__u.__c)			\
			: "Q" (*__x) : "memory");			\
		break;							\
	default:							\
		__u.__val = *(volatile typeof(*__x) *)__x;		\
	}								\
	*__ret;								\
})

#endif	/* !BUILD_VDSO */
#endif	/* CONFIG_LTO && !__ASSEMBLER__ */

#include <asm-generic/rwonce.h>

#endif	/* __ASM_RWONCE_H */
