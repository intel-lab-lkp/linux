/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) 2026 Tenstorrent
 */
#ifndef __SOC_ATLANTIS_SYSCON_H__
#define __SOC_ATLANTIS_SYSCON_H__

#include <linux/bits.h>
#include <linux/types.h>

struct atlantis_ccu_adev {
	struct auxiliary_device adev;
	struct regmap *regmap;
};

#define to_atlantis_ccu_adev(_adev) \
	container_of((_adev), struct atlantis_ccu_adev, adev)

/* RCPU Reset Register Offsets */
#define RCPU_BLK_RST_REG	0x001c
#define LSIO_BLK_RST_REG	0x0020
#define HSIO_BLK_RST_REG	0x000c
#define PCIE_SUBS_RST_REG	0x0000
#define MM_RSTN_REG		0x0014

#endif
