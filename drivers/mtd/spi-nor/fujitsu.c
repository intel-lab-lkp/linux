// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/mtd/spi-nor.h>

#include "core.h"

static const struct flash_info fujitsu_nor_parts[] = {
	{
		.id = SNOR_ID(0x04, 0x7f, 0x27),
		.name = "mb85rs1mt",
		.size = SZ_128K,
		.sector_size = SZ_128K,
		.flags = SPI_NOR_NO_ERASE,
	}
};

const struct spi_nor_manufacturer spi_nor_fujitsu = {
	.name = "fujitsu",
	.parts = fujitsu_nor_parts,
	.nparts = ARRAY_SIZE(fujitsu_nor_parts),
};
