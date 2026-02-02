/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Shared definitions for Atlantis PRCM Clock and Reset Drivers
 *
 * Copyright (c) 2026 Tenstorrent
 */
#ifndef __SOC_ATLANTIS_PRCM_H__
#define __SOC_ATLANTIS_PRCM_H__

#include <linux/bits.h>
#include <linux/types.h>

struct atlantis_prcm_adev {
	struct auxiliary_device adev;
	struct regmap *regmap;
};

static inline struct atlantis_prcm_adev *
to_atlantis_prcm_adev(struct auxiliary_device *adev)
{
	return container_of(adev, struct atlantis_prcm_adev, adev);
}

/* RCPU Reset Register Offsets */
#define RCPU_BLK_RST_REG 0x001c
#define LSIO_BLK_RST_REG 0x0020
#define HSIO_BLK_RST_REG 0x000c
#define PCIE_SUBS_RST_REG 0x0000
#define MM_RSTN_REG 0x0014

#endif
