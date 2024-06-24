// SPDX-License-Identifier: GPL-2.0
/*
 * Author:
 *      Andrey Zolotarev <andrey.zolotarev@keenetic.com> - the main driver logic
 *      Maxim Anisimov <maxim.anisimov.ua@gmail.com> - adaptation to mainline linux kernel
 *
 * Based on:
 *      https://github.com/keenetic/kernel-49/commit/bacade569fb12bc0ad31ba09bca9b890118fbca7
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_HEYANGTEK		0xC9

#define STATUS_ECC_LIMIT_BITFLIPS	(3 << 4)

static SPINAND_OP_VARIANTS(read_cache_variants,
		SPINAND_PAGE_READ_FROM_CACHE_QUADIO_OP(0, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_X4_OP(0, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_DUALIO_OP(0, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_X2_OP(0, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_OP(true, 0, 1, NULL, 0),
		SPINAND_PAGE_READ_FROM_CACHE_OP(false, 0, 1, NULL, 0));

static SPINAND_OP_VARIANTS(write_cache_variants,
		SPINAND_PROG_LOAD_X4(true, 0, NULL, 0),
		SPINAND_PROG_LOAD(true, 0, NULL, 0));

static SPINAND_OP_VARIANTS(update_cache_variants,
		SPINAND_PROG_LOAD_X4(false, 0, NULL, 0),
		SPINAND_PROG_LOAD(false, 0, NULL, 0));

static int hyfxgq4uda_ooblayout_ecc(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	region->offset = section * 16 + 8;
	region->length = 8;

	return 0;
}

static int hyfxgq4uda_ooblayout_free(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *region)
{
	if (section > 3)
		return -ERANGE;

	/* ECC-protected user meta-data */
	region->offset = section * 16 + 4;
	region->length = 4;

	return 0;
}

static const struct mtd_ooblayout_ops hyfxgq4uda_ooblayout = {
	.ecc = hyfxgq4uda_ooblayout_ecc,
	.free = hyfxgq4uda_ooblayout_free,
};

static int hyfxgq4uda_ecc_get_status(struct spinand_device *spinand,
				     u8 status)
{
	struct nand_device *nand = spinand_to_nand(spinand);

	switch (status & STATUS_ECC_MASK) {
	case STATUS_ECC_NO_BITFLIPS:
		return 0;

	case STATUS_ECC_UNCOR_ERROR:
		return -EBADMSG;

	case STATUS_ECC_HAS_BITFLIPS:
		return nanddev_get_ecc_conf(nand)->strength >> 1;

	case STATUS_ECC_LIMIT_BITFLIPS:
		return nanddev_get_ecc_conf(nand)->strength;

	default:
		break;
	}

	return -EINVAL;
}

static const struct spinand_info heyangtek_spinand_table[] = {
	SPINAND_INFO("HYF1GQ4UDACAE",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_ADDR, 0x21),
		     NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(4, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&hyfxgq4uda_ooblayout,
				     hyfxgq4uda_ecc_get_status)),
};

static const struct spinand_manufacturer_ops heyangtek_spinand_manuf_ops = {
};

const struct spinand_manufacturer heyangtek_spinand_manufacturer = {
	.id = SPINAND_MFR_HEYANGTEK,
	.name = "HeYangTek",
	.chips = heyangtek_spinand_table,
	.nchips = ARRAY_SIZE(heyangtek_spinand_table),
	.ops = &heyangtek_spinand_manuf_ops,
};
