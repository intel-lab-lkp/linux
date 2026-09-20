/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RISC-V memory-mapped I/O ordering.
 *
 * Copyright (C) 1996-2000 Russell King
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (C) 2014 Regents of the University of California
 */

#ifndef _ASM_RISCV_MMIO_H
#define _ASM_RISCV_MMIO_H

#include <asm/fence.h>

/*
 * Relaxed I/O memory access primitives. These follow the Device memory
 * ordering rules but do not guarantee any ordering relative to Normal memory
 * accesses.  These are defined to order the indicated access (either a read or
 * write) with all other I/O memory accesses to the same peripheral. Since the
 * platform specification defines that all I/O regions are strongly ordered on
 * channel 0, no explicit fences are required to enforce this ordering.
 *
 * I/O memory access primitives.  Reads are ordered relative to any following
 * Normal memory read and delay() loop.  Writes are ordered relative to any
 * prior Normal memory write.  The memory barriers here are necessary as RISC-V
 * doesn't define any ordering between the memory space and the I/O space.
 */
#define __io_br()	do {} while (0)
#define __io_ar(v)	RISCV_FENCE(i, ir)
#define __io_bw()	RISCV_FENCE(w, o)
#define __io_aw()	mmiowb_set_pending()

#include <asm-generic/mmio.h>

#endif /* _ASM_RISCV_MMIO_H */
