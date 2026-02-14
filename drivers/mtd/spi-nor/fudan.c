// SPDX-License-Identifier: GPL-2.0
/*
 * Fudan Microelectronics SPI NOR flash support.
 *
 * JEDEC manufacturer ID 0xf7.
 */

#include <linux/mtd/spi-nor.h>

#include "core.h"

static const struct flash_info fudan_nor_parts[] = {
	{
		.id = SNOR_ID(0xf7, 0xf0, 0x30),
		.name = "fm25q256",
		.size = SZ_32M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	},
};

const struct spi_nor_manufacturer spi_nor_fudan = {
	.name = "fudan",
	.parts = fudan_nor_parts,
	.nparts = ARRAY_SIZE(fudan_nor_parts),
};
