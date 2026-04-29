// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Pengutronix, Fabian Pflug <kernel@pengutronix.de>
 */

#include <linux/mtd/spi-nor.h>

#include "core.h"

static const struct flash_info cypress_nor_parts[] = {
	{
		.id = SNOR_ID(0x50, 0x51, 0x80, 0x06),
		.name = "cy15x104qs",
		.size = SZ_512K,
		.sector_size = SZ_512K,
		.flags = SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ | SPI_NOR_NO_ERASE,
	}
};

const struct spi_nor_manufacturer spi_nor_cypress = {
	.name = "cypress",
	.parts = cypress_nor_parts,
	.nparts = ARRAY_SIZE(cypress_nor_parts),
};
