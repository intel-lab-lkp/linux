// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/mtd/spi-nor.h>

#include "core.h"

/**
 * macronix_nor_otp_enter() - Send Enter Secured OTP instruction to the chip.
 * @nor:	pointer to 'struct spi_nor'.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int macronix_nor_otp_enter(struct spi_nor *nor)
{
	int error;

	error = spi_nor_send_cmd(nor, SPINOR_OP_ENSO);

	if (error)
		dev_dbg(nor->dev, "error %d on Macronix Enter Secured OTP\n", error);

	return error;
}

/**
 * macronix_nor_otp_exit() - Send Exit Secured OTP instruction to the chip.
 * @nor:	pointer to 'struct spi_nor'.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int macronix_nor_otp_exit(struct spi_nor *nor)
{
	int error;

	error = spi_nor_send_cmd(nor, SPINOR_OP_EXSO);

	if (error)
		dev_dbg(nor->dev, "error %d on Macronix Exit Secured OTP\n", error);

	return error;
}

/**
 * macronix_nor_otp_read() - read security register
 * @nor:	pointer to 'struct spi_nor'
 * @addr:       offset to read from
 * @len:        number of bytes to read
 * @buf:        pointer to dst buffer
 *
 * Return: number of bytes read successfully, -errno otherwise
 */
static int macronix_nor_otp_read(struct spi_nor *nor, loff_t addr, size_t len, u8 *buf)
{
	int ret, error;

	error = macronix_nor_otp_enter(nor);
	if (error)
		return error;

	ret = spi_nor_read_data(nor, addr, len, buf);

	error = macronix_nor_otp_exit(nor);

	if (ret < 0)
		dev_dbg(nor->dev, "error %d on Macronix read OTP data\n", ret);
	else if (error)
		return error;

	return ret;
}

/**
 * macronix_nor_otp_write() - write security register
 * @nor:        pointer to 'struct spi_nor'
 * @addr:       offset to write to
 * @len:        number of bytes to write
 * @buf:        pointer to src buffer
 *
 * Return: number of bytes written successfully, -errno otherwise
 */
static int macronix_nor_otp_write(struct spi_nor *nor, loff_t addr, size_t len, const u8 *buf)
{
	int error, ret = 0;

	error = macronix_nor_otp_enter(nor);
	if (error)
		return error;

	error = spi_nor_write_enable(nor);
	if (error)
		goto otp_write_err;

	ret = spi_nor_write_data(nor, addr, len, buf);
	if (ret < 0) {
		dev_dbg(nor->dev, "error %d on Macronix write OTP data\n", ret);
		goto otp_write_err;
	}

	error = spi_nor_wait_till_ready(nor);
	if (error)
		dev_dbg(nor->dev, "error %d on Macronix waiting write OTP finish\n", error);

otp_write_err:

	error = macronix_nor_otp_exit(nor);

	return ret;
}

/**
 * macronix_nor_otp_lock() - lock the OTP region
 * @nor:        pointer to 'struct spi_nor'
 * @region:     OTP region
 *
 * Return: 0 on success, -errno otherwise.
 */
static int macronix_nor_otp_lock(struct spi_nor *nor, unsigned int region)
{
	int error;
	u8 *rdscur = nor->bouncebuf;

	error = spi_nor_read_reg(nor, SPINOR_OP_RDSCUR, 1);
	if (error) {
		dev_dbg(nor->dev, "error %d on read security register\n", error);
		return error;
	}

	switch (region) {
	case 0: /* Lock 1st 4K-bit region */
		if (rdscur[0] & SEC_REG_LDS1)
			return 0; /* Already locked */
		rdscur[0] |= SEC_REG_LDS1;
		break;
	case 1: /* Lock 2nd 4K-bit region */
		if (rdscur[0] & SEC_REG_LDS2)
			return 0; /* Already locked */
		rdscur[0] |= SEC_REG_LDS2;
		break;
	default:
		return 0; /* Unknown region */
	}

	error = spi_nor_write_reg(nor, SPINOR_OP_WRSCUR, 1);
	if (error)
		dev_dbg(nor->dev, "error %d on update security register\n", error);

	return error;
}

/**
 * macronix_nor_otp_is_locked() - get the OTP region lock status
 * @nor:        pointer to 'struct spi_nor'
 * @region:     OTP region
 *
 * Return: 0 on success, -errno otherwise.
 */
static int macronix_nor_otp_is_locked(struct spi_nor *nor, unsigned int region)
{
	int ret;
	u8 *rdscur = nor->bouncebuf;

	ret = spi_nor_read_reg(nor, SPINOR_OP_RDSCUR, 1);
	if (ret) {
		dev_dbg(nor->dev, "error %d on read security register\n", ret);
		return ret;
	}

	switch (region) {
	case 0: /* 1st 4K-bit region */
		ret = (rdscur[0] & SEC_REG_LDS1) > 0;
		break;
	case 1: /* 2nd 4K-bit region */
		ret = (rdscur[0] & SEC_REG_LDS2) > 0;
		break;
	default: /* Unknown region */
		break;
	}
	return ret;
}

static int
mx25l25635_post_bfpt_fixups(struct spi_nor *nor,
			    const struct sfdp_parameter_header *bfpt_header,
			    const struct sfdp_bfpt *bfpt)
{
	/*
	 * MX25L25635F supports 4B opcodes but MX25L25635E does not.
	 * Unfortunately, Macronix has re-used the same JEDEC ID for both
	 * variants which prevents us from defining a new entry in the parts
	 * table.
	 * We need a way to differentiate MX25L25635E and MX25L25635F, and it
	 * seems that the F version advertises support for Fast Read 4-4-4 in
	 * its BFPT table.
	 */
	if (bfpt->dwords[SFDP_DWORD(5)] & BFPT_DWORD5_FAST_READ_4_4_4)
		nor->flags |= SNOR_F_4B_OPCODES;

	return 0;
}

static const struct spi_nor_fixups mx25l25635_fixups = {
	.post_bfpt = mx25l25635_post_bfpt_fixups,
};

static const struct flash_info macronix_nor_parts[] = {
	{
		.id = SNOR_ID(0xc2, 0x20, 0x10),
		.name = "mx25l512e",
		.size = SZ_64K,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x12),
		.name = "mx25l2005a",
		.size = SZ_256K,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x13),
		.name = "mx25l4005a",
		.size = SZ_512K,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x14),
		.name = "mx25l8005",
		.size = SZ_1M,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x15),
		.name = "mx25l1606e",
		.size = SZ_2M,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x16),
		.name = "mx25l3205d",
		.size = SZ_4M,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x17),
		.name = "mx25l6405d",
		.size = SZ_8M,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x18),
		.name = "mx25l12805d",
		.size = SZ_16M,
		.flags = SPI_NOR_HAS_LOCK | SPI_NOR_4BIT_BP,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x19),
		.name = "mx25l25635e",
		.size = SZ_32M,
		.no_sfdp_flags = SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
		.fixups = &mx25l25635_fixups
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x1a),
		.name = "mx66l51235f",
		.size = SZ_64M,
		.no_sfdp_flags = SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
		.fixup_flags = SPI_NOR_4B_OPCODES,
	}, {
		.id = SNOR_ID(0xc2, 0x20, 0x1b),
		.name = "mx66l1g45g",
		.size = SZ_128M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0xc2, 0x23, 0x14),
		.name = "mx25v8035f",
		.size = SZ_1M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x32),
		.name = "mx25u2033e",
		.size = SZ_256K,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x33),
		.name = "mx25u4035",
		.size = SZ_512K,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x34),
		.name = "mx25u8035",
		.size = SZ_1M,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x36),
		.name = "mx25u3235f",
		.size = SZ_4M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x37),
		.name = "mx25u6435f",
		.size = SZ_8M,
		.no_sfdp_flags = SECT_4K,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x38),
		.name = "mx25u12835f",
		.size = SZ_16M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x39),
		.name = "mx25u25635f",
		.size = SZ_32M,
		.no_sfdp_flags = SECT_4K,
		.fixup_flags = SPI_NOR_4B_OPCODES,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x3a),
		.name = "mx25u51245g",
		.size = SZ_64M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
		.fixup_flags = SPI_NOR_4B_OPCODES,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x3a),
		.name = "mx66u51235f",
		.size = SZ_64M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
		.fixup_flags = SPI_NOR_4B_OPCODES,
	}, {
		.id = SNOR_ID(0xc2, 0x25, 0x3c),
		.name = "mx66u2g45g",
		.size = SZ_256M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
		.fixup_flags = SPI_NOR_4B_OPCODES,
	}, {
		.id = SNOR_ID(0xc2, 0x26, 0x18),
		.name = "mx25l12855e",
		.size = SZ_16M,
	}, {
		.id = SNOR_ID(0xc2, 0x26, 0x19),
		.name = "mx25l25655e",
		.size = SZ_32M,
	}, {
		.id = SNOR_ID(0xc2, 0x26, 0x1b),
		.name = "mx66l1g55g",
		.size = SZ_128M,
		.no_sfdp_flags = SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0xc2, 0x28, 0x15),
		.name = "mx25r1635f",
		.size = SZ_2M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0xc2, 0x28, 0x16),
		.name = "mx25r3235f",
		.size = SZ_4M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0xc2, 0x81, 0x3a),
		.name = "mx25uw51245g",
		.n_banks = 4,
		.flags = SPI_NOR_RWW,
	}, {
		.id = SNOR_ID(0xc2, 0x9e, 0x16),
		.name = "mx25l3255e",
		.size = SZ_4M,
		.no_sfdp_flags = SECT_4K,
	}
};

static void macronix_nor_default_init(struct spi_nor *nor)
{
	nor->params->quad_enable = spi_nor_sr1_bit6_quad_enable;
}

static const struct spi_nor_otp_ops macronix_nor_otp_ops = {
	.read = macronix_nor_otp_read,
	.write = macronix_nor_otp_write,
	/* .erase = Macronix OTP do not support erase, */
	.lock = macronix_nor_otp_lock,
	.is_locked = macronix_nor_otp_is_locked,
};

static int macronix_nor_late_init(struct spi_nor *nor)
{
	if (nor->params->otp.org.n_regions)
		nor->params->otp.ops = &macronix_nor_otp_ops;

	if (!nor->params->set_4byte_addr_mode)
		nor->params->set_4byte_addr_mode = spi_nor_set_4byte_addr_mode_en4b_ex4b;

	return 0;
}

static const struct spi_nor_fixups macronix_nor_fixups = {
	.default_init = macronix_nor_default_init,
	.late_init = macronix_nor_late_init,
};

const struct spi_nor_manufacturer spi_nor_macronix = {
	.name = "macronix",
	.parts = macronix_nor_parts,
	.nparts = ARRAY_SIZE(macronix_nor_parts),
	.fixups = &macronix_nor_fixups,
};
