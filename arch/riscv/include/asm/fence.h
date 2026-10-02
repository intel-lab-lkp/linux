/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_FENCE_H
#define _ASM_RISCV_FENCE_H

#include <asm/alternative-macros.h>
#include <asm/hwcap.h>

#define RISCV_ZALASR_ALTERNATIVE(old, new) \
	ALTERNATIVE(old, new, 0, RISCV_ISA_EXT_ZALASR, 1)

#define RISCV_ZALASR_SMP_ALTERNATIVE(old, new) \
	ALTERNATIVE(old, new, 0, RISCV_ISA_EXT_ZALASR, CONFIG_SMP)

#define RISCV_FENCE_ASM(p, s)		"\tfence " #p "," #s "\n"
#define RISCV_FENCE(p, s) \
	({ __asm__ __volatile__ (RISCV_FENCE_ASM(p, s) : : : "memory"); })

#ifdef CONFIG_SMP
#define RISCV_ACQUIRE_BARRIER		RISCV_FENCE_ASM(r, rw)
#define RISCV_RELEASE_BARRIER		RISCV_FENCE_ASM(rw, w)
#define RISCV_FULL_BARRIER		RISCV_FENCE_ASM(rw, rw)
#else
#define RISCV_ACQUIRE_BARRIER
#define RISCV_RELEASE_BARRIER
#define RISCV_FULL_BARRIER
#endif

#endif	/* _ASM_RISCV_FENCE_H */
