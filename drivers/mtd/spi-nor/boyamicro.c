// SPDX-License-Identifier: GPL-2.0

#include <linux/mtd/spi-nor.h>

#include "core.h"

#define BOYA_SPINOR_OP_WRSR2		0x31

#define BOYA_SPI_NOR_WRSR2_OP(buf)					\
	SPI_MEM_OP(SPI_MEM_OP_CMD(BOYA_SPINOR_OP_WRSR2, 0),		\
		   SPI_MEM_OP_NO_ADDR,					\
		   SPI_MEM_OP_NO_DUMMY,					\
		   SPI_MEM_OP_DATA_OUT(1, buf, 0))

static int boya_spi_nor_write_sr2(struct spi_nor *nor, const u8 *sr2)
{
	int ret;

	ret = spi_nor_write_enable(nor);
	if (ret)
		return ret;

	if (nor->spimem) {
		struct spi_mem_op op = BOYA_SPI_NOR_WRSR2_OP(sr2);

		spi_nor_spimem_setup_op(nor, &op, nor->reg_proto);

		ret = spi_mem_exec_op(nor->spimem, &op);
	} else {
		ret = spi_nor_controller_ops_write_reg(nor, BOYA_SPINOR_OP_WRSR2,
						       sr2, 1);
	}

	if (ret) {
		dev_dbg(nor->dev, "error %d writing SR2\n", ret);
		return ret;
	}

	return spi_nor_wait_till_ready(nor);
}

static int by25q128_sr2_bit1_quad_enable(struct spi_nor *nor)
{
	int ret;
	u8 sr2_written;
	u8 *sr2 = nor->bouncebuf;

	/* Check current Quad Enable bit value. */
	ret = spi_nor_read_cr(nor, sr2);
	if (ret)
		return ret;
	if (*sr2 & SR2_QUAD_EN_BIT1)
		return 0;

	/* Update the Quad Enable bit. */
	*sr2 |= SR2_QUAD_EN_BIT1;

	ret = boya_spi_nor_write_sr2(nor, sr2);
	if (ret)
		return ret;

	sr2_written = *sr2;

	/* Read back and check it. */
	ret = spi_nor_read_cr(nor, sr2);
	if (ret)
		return ret;

	if (*sr2 != sr2_written) {
		dev_dbg(nor->dev, "SR2: Read back test failed\n");
		return -EIO;
	}

	return 0;
}

static int
by25q128_post_bfpt(struct spi_nor *nor,
		   const struct sfdp_parameter_header *bfpt_header,
		   const struct sfdp_bfpt *bfpt)
{
	/**
	 * BY25Q128xS series SFDP table does not define the Quad
	 * Enable methods. Overwrite the default Quad Enable method.
	 */
	nor->params->quad_enable = by25q128_sr2_bit1_quad_enable;

	/* The 01H command can only be used to write SR1 */
	nor->flags &= ~SNOR_F_HAS_16BIT_SR;

	return 0;
}

static const struct spi_nor_fixups by25q128_fixups = {
	.post_bfpt = by25q128_post_bfpt,
};

static const struct flash_info boyamicro_parts[] = {
	{
		/* BY25Q128AS, BY25Q128ES */
		.id = SNOR_ID(0x68, 0x40, 0x18),
		.fixups = &by25q128_fixups,
	},
};

const struct spi_nor_manufacturer spi_nor_boyamicro = {
	.name = "boyamicro",
	.parts = boyamicro_parts,
	.nparts = ARRAY_SIZE(boyamicro_parts),
};
