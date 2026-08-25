// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 ISSI
 *
 * Authors:
 *	Bill Lee <blee@issi.com>
 *	Jeff Kim <jekim@issi.com>
 * Co-Author:
 *	Han Xu <han.xu@nxp.com>
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mtd/spinand.h>

#define SPINAND_MFR_ISSI 0x9d

/*
 * ___________________________________________________________________________________________
 * |       ISSI SPI NAND Flash ECC Status Register Bit Descriptions(Refer status register)     |
 * |------|------|------|----------------------------------------------------------------------|
 * | Bit6 | Bit5 | Bit4 | ECC Status                                                           |
 * |------|------|------|----------------------------------------------------------------------|
 * | 0    | 0    | 0    | No bit error detected during the previous read algorithm.            |
 * | 0    | 0    | 1    | 1-3 bit errors detected and corrected; no data refreshment required. |
 * | 0    | 1    | 0    | >8 bit errors detected and NOT corrected.                            |
 * | 0    | 1    | 1    | 4-6 bit errors detected and corrected; refresh recommended.          |
 * | 1    | 0    | 0    | Reserved                                                             |
 * | 1    | 0    | 1    | 7-8 bit errors detected and corrected; refresh must be done.         |
 * | 1    | 1    | 0    | Reserved                                                             |
 * | 1    | 1    | 1    | Invalid state                                                        |
 * |------|------|------|----------------------------------------------------------------------|
 */
#define ISSI_STATUS_ECC_MASK GENMASK(6, 4)
#define ISSI_STATUS_ECC_NO_BITFLIPS (0 << 4)
#define ISSI_STATUS_ECC_1TO3_BITFLIPS (1 << 4)
#define ISSI_STATUS_ECC_UNCOR_ERROR (2 << 4)
#define ISSI_STATUS_ECC_4TO6_BITFLIPS (3 << 4)
#define ISSI_STATUS_ECC_7TO8_BITFLIPS (5 << 4)

/*
 * As per datasheet, die selection is done by the 7th bit of Drive
 * Strength Register (Address 0xD0).
 */
#define ISSI_DIE_SELECT_REG 0xD0
#define ISSI_SELECT_DIE_MASK BIT(7)
#define ISSI_SELECT_DIE(x) ((x) << 7)

static SPINAND_OP_VARIANTS(
	quadio_read_cache_variants,
	SPINAND_PAGE_READ_FROM_CACHE_1S_1S_4S_OP(0, 1, NULL, 0, 0),
	SPINAND_PAGE_READ_FROM_CACHE_1S_1S_2S_OP(0, 1, NULL, 0, 0),
	SPINAND_PAGE_READ_FROM_CACHE_FAST_1S_1S_1S_OP(0, 1, NULL, 0, 0),
	SPINAND_PAGE_READ_FROM_CACHE_1S_1S_1S_OP(0, 1, NULL, 0, 0));

static SPINAND_OP_VARIANTS(x4_write_cache_variants,
			   SPINAND_PROG_LOAD_1S_1S_4S_OP(true, 0, NULL,
							 0), // 0x32 quad io
			   SPINAND_PROG_LOAD_1S_1S_1S_OP(true, 0, NULL,
							 0)); // 0x02 single io

static SPINAND_OP_VARIANTS(
	x4_update_cache_variants,
	SPINAND_PROG_LOAD_1S_1S_4S_OP(false, 0, NULL, 0), // 0x34 random quad io
	SPINAND_PROG_LOAD_1S_1S_1S_OP(false, 0, NULL,
				      0)); // 0x84 random single io

/*
 * ISSI - 8 bit ECC per 544 - bytes Spare Area Mapping
 *  ________________________________________________________________________
 * |________________________________________________________________________|
 * | Max Addr | Min Addr | ECC Prot | Area     | Description                |
 * |----------|----------|----------|----------|----------------------------|
 * | 0x01FF   | 0x0000   | Yes      | Main 0   | User data 0                |
 * | 0x03FF   | 0x0200   | Yes      | Main 1   | User data 1                |
 * | 0x05FF   | 0x0400   | Yes      | Main 2   | User data 2                |
 * | 0x07FF   | 0x0600   | Yes      | Main 3   | User data 3                |
 * -------------------------------------------------------------------------|
 * | 0x080F   | 0x0800   | Yes      | Spare 0  | Spare 0                    |
 * | 0x081F   | 0x0810   | Yes      | Spare 1  | Spare 1                    |
 * | 0x082F   | 0x0820   | Yes      | Spare 2  | Spare 2                    |
 * | 0x083F   | 0x0830   | Yes      | Spare 3  | Spare 3                    |
 * -------------------------------------------------------------------------|
 * | 0x084F   | 0x0840   | Yes      | Parity 0 | ECC for main/spare 0       |
 * | 0x085F   | 0x0850   | Yes      | Parity 1 | ECC for main/spare 1       |
 * | 0x086F   | 0x0860   | Yes      | Parity 2 | ECC for main/spare 2       |
 * | 0x087F   | 0x0870   | Yes      | Parity 3 | ECC for main/spare 3       |
 * |________________________________________________________________________|
 */
static int issi_8_ooblayout_ecc(struct mtd_info *mtd, int section,
				struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	region->offset = mtd->oobsize / 2;
	region->length = mtd->oobsize / 2;

	return 0;
}

static int issi_8_ooblayout_free(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	/* Reserve 2 bytes for the BBM. */
	region->offset = 2;
	region->length = (mtd->oobsize / 2) - 2;

	return 0;
}

static const struct mtd_ooblayout_ops issi_8_ooblayout = {
	.ecc = issi_8_ooblayout_ecc,
	.free = issi_8_ooblayout_free,
};

static int issi_select_target(struct spinand_device *spinand,
			      unsigned int target)
{
	int ret;
	u8 regval;

	if (target > 1)
		return -EINVAL;

	/* Read the current value of the register */
	ret = spinand_read_reg_op(spinand, ISSI_DIE_SELECT_REG, &regval);
	if (ret)
		return ret;

	/* Update only the die select bit (bit 7) */
	regval &= ~ISSI_SELECT_DIE_MASK;
	regval |= ISSI_SELECT_DIE(target);

	/* Write back the updated value */
	return spinand_write_reg_op(spinand, ISSI_DIE_SELECT_REG, regval);
}

static int issi_8_ecc_get_status(struct spinand_device *spinand, u8 status)
{
	switch (status & ISSI_STATUS_ECC_MASK) {
	case ISSI_STATUS_ECC_NO_BITFLIPS:
		return 0;

	case ISSI_STATUS_ECC_UNCOR_ERROR:
		return -EBADMSG;

	case ISSI_STATUS_ECC_1TO3_BITFLIPS:
		return 3;

	case ISSI_STATUS_ECC_4TO6_BITFLIPS:
		return 6;

	case ISSI_STATUS_ECC_7TO8_BITFLIPS:
		return 8;

	default:
		break;
	}

	return -EINVAL;
}

static const struct spinand_info issi_spinand_table[] = {
	/* IS37/38SMW01G8B 1Gb 1.8V */
	SPINAND_INFO("IS37/38SMW01G8B",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x15),
		     /*
		      * - Memory Cell: 1bit/Memory Cell
		      * - Page Size: 2176 bytes (2048 + 128) bytes
		      * - Block Size: 64 pages (128K + 8K) bytes
		      * - Plane Size:
		      *   1Gb: 1024 blocks per plane
		      * - Device Size:
		      *   1Gb: 1 plane = 1024 blocks
		      */
		     NAND_MEMORG(1, 2048, 128, 64, 1024, 40, 1, 1, 1),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&quadio_read_cache_variants,
					      &x4_write_cache_variants,
					      &x4_update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&issi_8_ooblayout, issi_8_ecc_get_status),
		     SPINAND_SELECT_TARGET(issi_select_target)),

	/* IS37/38SMW02G8B 2Gb 1.8V */
	SPINAND_INFO("IS37/38SMW02G8B",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x25),
		     /*
		      * - Memory Cell: 1bit/Memory Cell
		      * - Page Size: 2176 bytes (2048 + 128) bytes
		      * - Block Size: 64 pages (128K + 8K) bytes
		      * - Plane Size:
		      *   2Gb: 2048 blocks per plane
		      * - Device Size:
		      *   2Gb: 1 plane = 2048 blocks
		      */
		     NAND_MEMORG(1, 2048, 128, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&quadio_read_cache_variants,
					      &x4_write_cache_variants,
					      &x4_update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&issi_8_ooblayout, issi_8_ecc_get_status),
		     SPINAND_SELECT_TARGET(issi_select_target)),

	/* IS37/38SML04G8 4Gb 3.3V */
	SPINAND_INFO("IS37/38SML04G8",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x34),
		     /*
		      * Architecture
		      * - Memory Cell: 1bit/Memory Cell
		      * - Page Size: 2176 bytes (2048 + 128) bytes
		      * - Block Size: 64 pages (128K + 8K) bytes
		      * - Plane Size: 2048 blocks per plane
		      * - Die Size: 2Gb with 2048 blocks
		      * - Device Size: 4Gb with 2-die stacks
		      */
		     NAND_MEMORG(1, 2048, 128, 64, 2048, 40, 1, 1, 2),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&quadio_read_cache_variants,
					      &x4_write_cache_variants,
					      &x4_update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&issi_8_ooblayout, issi_8_ecc_get_status),
		     SPINAND_SELECT_TARGET(issi_select_target)),

	/* IS37/38SMW04G8B 4Gb 1.8V */
	SPINAND_INFO("IS37/38SMW04G8B",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_DUMMY, 0x35),
		     /*
		      * Architecture
		      * - Memory Cell: 1bit/Memory Cell
		      * - Page Size: 2176 bytes (2048 + 128) bytes
		      * - Block Size: 64 pages (128K + 8K) bytes
		      * - Plane Size: 2048 blocks per plane
		      * - Die Size: 2Gb with 2048 blocks
		      * - Device Size: 4Gb with 2-die stacks
		      */
		     NAND_MEMORG(1, 2048, 128, 64, 2048, 40, 1, 1, 2),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&quadio_read_cache_variants,
					      &x4_write_cache_variants,
					      &x4_update_cache_variants),
		     SPINAND_HAS_QE_BIT,
		     SPINAND_ECCINFO(&issi_8_ooblayout, issi_8_ecc_get_status),
		     SPINAND_SELECT_TARGET(issi_select_target)),
};

static int issi_spinand_init(struct spinand_device *spinand)
{
	/*
	 * manufacturer specific initialization can be done here
	 */

	return 0;
}

static const struct spinand_manufacturer_ops issi_spinand_manuf_ops = {
	.init = issi_spinand_init,
};

const struct spinand_manufacturer issi_spinand_manufacturer = {
	.id = SPINAND_MFR_ISSI,
	.name = "ISSI",
	.chips = issi_spinand_table,
	.nchips = ARRAY_SIZE(issi_spinand_table),
	.ops = &issi_spinand_manuf_ops,
};
