/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 */

#ifndef _ASM_RISCV_TIMEX_H
#define _ASM_RISCV_TIMEX_H

#include <asm/csr.h>
#include <asm/mmio.h>

#include <linux/jump_label.h>

typedef unsigned long cycles_t;

/*
 * Used for directly accessing MMIO timers. For now
 * this includes the M-mode clint timer and the GCR.U mtime shadow copy.
 */
extern u64 __iomem *riscv_time_val;

extern cycles_t (*get_cycles_ptr)(void);
extern u32 (*get_cycles_hi_ptr)(void);

#define get_cycles get_cycles_ptr
#define get_cycles_hi get_cycles_hi_ptr

#ifdef CONFIG_RISCV_M_MODE

/*
 * Much like MIPS, we may not have a viable counter to use at an early point
 * in the boot process. Unfortunately we don't have a fallback, so instead
 * we just return 0.
 */
static inline unsigned long random_get_entropy(void)
{
	if (unlikely(riscv_time_val == NULL))
		return random_get_entropy_fallback();
	return get_cycles();
}

#define random_get_entropy()	random_get_entropy()
#endif

#ifdef CONFIG_64BIT
static inline cycles_t __maybe_unused mmio_get_cycles(void)
{
	return readq_relaxed(riscv_time_val);
}
#else /* !CONFIG_64BIT */
static inline cycles_t __maybe_unused mmio_get_cycles(void)
{
	return readl_relaxed(((u32 __iomem *)riscv_time_val));
}
#endif /* CONFIG_64BIT */

static inline cycles_t __maybe_unused get_cycles_csr(void)
{
	return csr_read(CSR_TIME);
}

static inline u32 __maybe_unused mmio_get_cycles_hi(void)
{
	return readl_relaxed(((u32 __iomem *)riscv_time_val) + 1);
}

static inline u32 __maybe_unused get_cycles_hi_csr(void)
{
	return csr_read(CSR_TIMEH);
}

#ifdef CONFIG_64BIT
static inline u64 get_cycles64(void)
{
	return get_cycles();
}
#else /* !CONFIG_64BIT */
static inline u64 get_cycles64(void)
{
	u32 hi, lo;

	do {
		hi = get_cycles_hi();
		lo = get_cycles();
	} while (hi != get_cycles_hi());

	return ((u64)hi << 32) | lo;
}
#endif /* CONFIG_64BIT */

#define ARCH_HAS_READ_CURRENT_TIMER
static inline int read_current_timer(unsigned long *timer_val)
{
	*timer_val = get_cycles();
	return 0;
}

#endif /* _ASM_RISCV_TIMEX_H */
