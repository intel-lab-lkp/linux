/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/arch/arm64/include/asm/neon.h
 *
 * Copyright (C) 2013 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#ifndef __ASM_NEON_H
#define __ASM_NEON_H

#include <linux/types.h>
#include <asm/fpsimd.h>

#define cpu_has_neon()		system_supports_fpsimd()

void __kernel_neon_begin(struct user_fpsimd_state *);
void __kernel_neon_end(void);

#define kernel_neon_begin()	do { __kernel_neon_begin(&(struct user_fpsimd_state){})
#define kernel_neon_end()	__kernel_neon_end(); } while (0)

#endif /* ! __ASM_NEON_H */
