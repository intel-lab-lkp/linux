// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Ahmed Naseef <naseefkm@gmail.com>
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_DOSILICON        0xE5

static SPINAND_OP_VARIANTS(read_cache_variants,
		SPINAND_PAGE_READ_FROM_CACHE_X4_OP(0, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_X2_OP(0, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_OP(true, 0, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_OP(false, 0, 1, NULL, 0));

static SPINAND_OP_VARIANTS(write_cache_variants,
		SPINAND_PROG_LOAD_X4(true, 0, NULL, 0),
		SPINAND_PROG_LOAD(true, 0, NULL, 0));

static SPINAND_OP_VARIANTS(update_cache_variants,
		SPINAND_PROG_LOAD_X4(false, 0, NULL, 0),
		SPINAND_PROG_LOAD(false, 0, NULL, 0));

static int ds35xx_ooblayout_ecc(struct mtd_info *mtd, int section,
				struct mtd_oob_region *region)
{
	return -ERANGE;
}

static int ds35xx_ooblayout_free(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;
	region->offset = 2;
	region->length = 62;
	return 0;
}

static const struct mtd_ooblayout_ops ds35xx_ooblayout = {
	.ecc = ds35xx_ooblayout_ecc,
	.free = ds35xx_ooblayout_free,
};

static const struct spinand_info dosilicon_spinand_table[] = {
	SPINAND_INFO("DS35Q1GA",
		SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x71),
		NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		NAND_ECCREQ(4, 512),
		SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					 &write_cache_variants,
					 &update_cache_variants),
		SPINAND_HAS_QE_BIT,
		SPINAND_ECCINFO(&ds35xx_ooblayout, NULL)),
	SPINAND_INFO("DS35M1GA",
		SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x21),
		NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		NAND_ECCREQ(4, 512),
		SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					 &write_cache_variants,
					 &update_cache_variants),
		SPINAND_HAS_QE_BIT,
		SPINAND_ECCINFO(&ds35xx_ooblayout, NULL)),
};

static const struct spinand_manufacturer_ops dosilicon_spinand_manuf_ops = {
};

const struct spinand_manufacturer dosilicon_spinand_manufacturer = {
	.id = SPINAND_MFR_DOSILICON,
	.name = "Dosilicon",
	.chips = dosilicon_spinand_table,
	.nchips = ARRAY_SIZE(dosilicon_spinand_table),
	.ops = &dosilicon_spinand_manuf_ops,
};
