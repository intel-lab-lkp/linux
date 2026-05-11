/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * SDXI MMIO register offsets and layouts.
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#ifndef DMA_SDXI_MMIO_H
#define DMA_SDXI_MMIO_H

#include <linux/bits.h>

enum sdxi_reg {
	/* SDXI 1.0 9.1 General Control and Status Registers */
	SDXI_MMIO_CTL0       = 0x00000,
	SDXI_MMIO_CTL2       = 0x00010,
	SDXI_MMIO_STS0       = 0x00100,
	SDXI_MMIO_CAP0       = 0x00200,
	SDXI_MMIO_CAP1       = 0x00208,
	SDXI_MMIO_VERSION    = 0x00210,
};

/* SDXI 1.0 Table 9-2: MMIO_CTL0 */
#define SDXI_MMIO_CTL0_FN_GSR         GENMASK_ULL(1, 0)

/* SDXI 1.0 Table 9-4: MMIO_CTL2 */
#define SDXI_MMIO_CTL2_MAX_BUFFER  GENMASK_ULL(3, 0)
#define SDXI_MMIO_CTL2_MAX_AKEY_SZ GENMASK_ULL(15, 12)
#define SDXI_MMIO_CTL2_MAX_CXT     GENMASK_ULL(31, 16)
#define SDXI_MMIO_CTL2_OPB_000_AVL GENMASK_ULL(63, 32)

/* SDXI 1.0 Table 9-5: MMIO_STS0 */
#define SDXI_MMIO_STS0_FN_GSV GENMASK_ULL(2, 0)

/* SDXI 1.0 Table 9-6: MMIO_CAP0 */
#define SDXI_MMIO_CAP0_SFUNC          GENMASK_ULL(15, 0)
#define SDXI_MMIO_CAP0_DB_STRIDE      GENMASK_ULL(22, 20)
#define SDXI_MMIO_CAP0_MAX_DS_RING_SZ GENMASK_ULL(28, 24)

/* SDXI 1.0 Table 9-7: MMIO_CAP1 */
#define SDXI_MMIO_CAP1_MAX_BUFFER    GENMASK_ULL(3, 0)
#define SDXI_MMIO_CAP1_MAX_AKEY_SZ   GENMASK_ULL(15, 12)
#define SDXI_MMIO_CAP1_MAX_CXT       GENMASK_ULL(31, 16)
#define SDXI_MMIO_CAP1_OPB_000_CAP   GENMASK_ULL(63, 32)

/* SDXI 1.0 Table 9-8: MMIO_VERSION */
#define SDXI_MMIO_VERSION_MINOR GENMASK_ULL(7, 0)
#define SDXI_MMIO_VERSION_MAJOR GENMASK_ULL(23, 16)

#endif  /* DMA_SDXI_MMIO_H */
