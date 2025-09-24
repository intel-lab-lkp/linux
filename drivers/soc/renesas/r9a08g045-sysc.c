// SPDX-License-Identifier: GPL-2.0
/*
 * RZ/G3S System controller driver
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/init.h>

#include "rz-sysc.h"

#define SYS_USB_PWRRDY		0xd70
#define SYS_PCIE_RST_RSM_B	0xd74

static const struct rz_sysc_soc_id_init_data rzg3s_sysc_soc_id_init_data __initconst = {
	.family = "RZ/G3S",
	.id = 0x85e0447,
	.devid_offset = 0xa04,
	.revision_mask = GENMASK(31, 28),
	.specific_id_mask = GENMASK(27, 0),
};

static bool rzg3s_regmap_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SYS_USB_PWRRDY:
	case SYS_PCIE_RST_RSM_B:
		return true;
	default:
		return false;
	}
}

const struct rz_sysc_init_data rzg3s_sysc_init_data __initconst = {
	.soc_id_init_data = &rzg3s_sysc_soc_id_init_data,
	.readable_reg = rzg3s_regmap_readable_reg,
	.writeable_reg = rzg3s_regmap_readable_reg,
	.max_register = 0xe20,
};
