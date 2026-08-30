/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2022-2024 Rivos, Inc
 * Copyright 2026 Jisheng Zhang <jszhang@kernel.org>
 */

#ifndef _ASM_CPUFEATURE_MACROS_H
#define _ASM_CPUFEATURE_MACROS_H

#include <asm/hwcap.h>
#include <asm/alternative-macros.h>

#define STANDARD_EXT		0

bool __riscv_isa_extension_available(const unsigned long *isa_bitmap, unsigned int bit);
#define riscv_isa_extension_available(isa_bitmap, ext)	\
	__riscv_isa_extension_available(isa_bitmap, RISCV_ISA_EXT_##ext)

static __always_inline bool __riscv_has_extension_likely(const unsigned long vendor,
							 const unsigned long ext)
{
	asm goto(ALTERNATIVE("j	%l[l_no]", "nop", %[vendor], %[ext], 1)
	:
	: [vendor] "i" (vendor), [ext] "i" (ext)
	:
	: l_no);

	return true;
l_no:
	return false;
}

static __always_inline bool __riscv_has_extension_unlikely(const unsigned long vendor,
							   const unsigned long ext)
{
	asm goto(ALTERNATIVE("nop", "j	%l[l_yes]", %[vendor], %[ext], 1)
	:
	: [vendor] "i" (vendor), [ext] "i" (ext)
	:
	: l_yes);

	return false;
l_yes:
	return true;
}

static __always_inline bool riscv_has_extension_unlikely(const unsigned long ext)
{
	compiletime_assert(ext < RISCV_ISA_EXT_MAX, "ext must be < RISCV_ISA_EXT_MAX");

	return __riscv_has_extension_unlikely(STANDARD_EXT, ext);
}

static __always_inline bool riscv_has_extension_likely(const unsigned long ext)
{
	compiletime_assert(ext < RISCV_ISA_EXT_MAX, "ext must be < RISCV_ISA_EXT_MAX");

	return __riscv_has_extension_likely(STANDARD_EXT, ext);
}

static __always_inline bool __riscv_has_cap_likely(const unsigned long cap)
{
	asm goto(ALTERNATIVE("j	%l[l_no]", "nop", 0, %[cap], 1)
	:
	: [cap] "i" (cap)
	:
	: l_no);

	return true;
l_no:
	return false;
}

static __always_inline bool __riscv_has_cap_unlikely(const unsigned long cap)
{

	asm goto(ALTERNATIVE("nop", "j	%l[l_yes]", 0, %[cap], 1)
	:
	: [cap] "i" (cap)
	:
	: l_yes);

	return false;
l_yes:
	return true;
}

static __always_inline bool riscv_has_cap_unlikely(const unsigned long cap)
{
	compiletime_assert(cap >= RISCV_ISA_EXT_MAX &&
			   cap < RISCV_CAP_MAX,
			   "cap must be >= RISCV_ISA_EXT_MAX and < RISCV_CAP_MAX");

	return __riscv_has_cap_unlikely(cap);
}

static __always_inline bool riscv_has_cap_likely(const unsigned long cap)
{
	compiletime_assert(cap >= RISCV_ISA_EXT_MAX &&
			   cap < RISCV_CAP_MAX,
			   "cap must be >= RISCV_ISA_EXT_MAX and < RISCV_CAP_MAX");

	return __riscv_has_cap_likely(cap);
}

#endif /* _ASM_CPUFEATURE_MACROS_H */
