// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/rawnand.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* NFI Registers */
#define MA35_NFI_REG_DMACTL		0x400
#define   DMA_EN				BIT(0)
#define   DMA_RST				BIT(1)
#define   DMA_BUSY				BIT(9)

#define MA35_NFI_REG_DMASA		0x408
#define MA35_NFI_REG_GCTL		0x800
#define   NAND_EN				BIT(3)

#define MA35_NFI_REG_NANDCTL		0x8A0
#define   SWRST				BIT(0)
#define   DMA_R_EN				BIT(1)
#define   DMA_W_EN				BIT(2)
#define   ECC_CHK				BIT(7)
#define   PROT3BEN				BIT(8)
#define   PSIZE_2K				BIT(16)
#define   PSIZE_4K				BIT(17)
#define   PSIZE_8K				GENMASK(17, 16)
#define   PSIZE_MASK				GENMASK(17, 16)
#define   BCH_T24				BIT(18)
#define   BCH_T8				BIT(20)
#define   BCH_T12				BIT(21)
#define   BCH_NONE				(0x0)
#define   BCH_MASK				GENMASK(22, 18)
#define   ECC_EN				BIT(23)
#define   DISABLE_CS0				BIT(25)

#define MA35_NFI_REG_NANDINTEN	0x8A8
#define MA35_NFI_REG_NANDINTSTS	0x8AC
#define   INT_DMA				BIT(0)
#define   INT_ECC				BIT(2)
#define   INT_RB0				BIT(10)
#define   INT_RB0_STS				BIT(18)

#define MA35_NFI_REG_NANDCMD		0x8B0
#define MA35_NFI_REG_NANDADDR		0x8B4
#define   ENDADDR				BIT(31)

#define MA35_NFI_REG_NANDDATA		0x8B8
#define MA35_NFI_REG_NANDRACTL	0x8BC
#define MA35_NFI_REG_NANDECTL		0x8C0
#define   ENABLE_WP				0x0
#define   DISABLE_WP				BIT(0)

#define MA35_NFI_REG_NANDECCES0	0x8D0
#define   ECC_STATUS_MASK			GENMASK(1, 0)
#define   ECC_ERR_CNT_MASK			GENMASK(4, 0)

#define MA35_NFI_REG_NANDECCEA0	0x900
#define MA35_NFI_REG_NANDECCED0	0x960
#define MA35_NFI_REG_NANDRA0		0xA00

/* Define for the BCH hardware ECC engine */
/* define the total padding bytes for 512/1024 data segment */
#define MA35_BCH_PADDING_512	32
#define MA35_BCH_PADDING_1024	64
/* define the BCH parity code length for 512 bytes data pattern */
#define MA35_PARITY_BCH8	15
#define MA35_PARITY_BCH12	23
/* define the BCH parity code length for 1024 bytes data pattern */
#define MA35_PARITY_BCH24	45

struct ma35_nand_info {
	struct nand_controller controller;
	struct nand_chip chip;
	struct device *dev;
	void __iomem *regs;
	int irq;
	struct clk *clk;
	struct completion complete;
	u32 bch;
	u32 bitflips;
	u8 *ecc_buf;
};

static int ma35_ooblayout_ecc(struct mtd_info *mtd, int section,
			      struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section)
		return -ERANGE;

	oobregion->length = chip->ecc.total;
	oobregion->offset = mtd->oobsize - oobregion->length;

	return 0;
}

static int ma35_ooblayout_free(struct mtd_info *mtd, int section,
			       struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section)
		return -ERANGE;

	oobregion->length = mtd->oobsize - chip->ecc.total - 2;
	oobregion->offset = 2;

	return 0;
}

static const struct mtd_ooblayout_ops ma35_ooblayout_ops = {
	.free = ma35_ooblayout_free,
	.ecc = ma35_ooblayout_ecc,
};

static inline void ma35_clear_spare(struct nand_chip *chip, int size)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	int i;

	for (i = 0; i < size/4; i++)
		writel(0xff, nand->regs + MA35_NFI_REG_NANDRA0);
}

static inline void read_remaining_bytes(struct ma35_nand_info *nand, u32 *buf,
					u32 offset, int size)
{
	u32 value = readl(nand->regs + MA35_NFI_REG_NANDRA0 + offset);
	u8 *ptr = (u8 *)buf;
	int i;

	for (i = 0; i < size; i++)
		ptr[i] = (value >> (i * 8)) & 0xff;
}

static inline void ma35_read_spare(struct nand_chip *chip, int size, u32 *buf, u32 offset)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	int i, j;

	if ((offset % 4) == 0) {
		for (i = 0, j = 0; i < size / 4; i++, j += 4)
			*buf++ = readl(nand->regs + MA35_NFI_REG_NANDRA0 + offset + j);

		read_remaining_bytes(nand, buf, offset + j, size % 4);
	} else {
		read_remaining_bytes(nand, buf, offset, 4 - (offset % 4));
		offset += 4;
		size -= (4 - (offset % 4));

		for (i = 0, j = 0; i < size / 4; i++, j += 4)
			*buf++ = readl(nand->regs + MA35_NFI_REG_NANDRA0 + offset + j);

		read_remaining_bytes(nand, buf, offset + j, size % 4);
	}
}

static inline void ma35_write_spare(struct nand_chip *chip, int size, u32 *buf)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	u32 value;
	int i, j;
	u8 *ptr;

	for (i = 0, j = 0; i < size / 4; i++, j += 4)
		writel(*buf++, nand->regs + MA35_NFI_REG_NANDRA0 + j);

	ptr = (u8 *)buf;
	switch (size % 4) {
	case 1:
		writel(*ptr, nand->regs + MA35_NFI_REG_NANDRA0 + j);
		break;
	case 2:
		value = *ptr | (*(ptr+1) << 8);
		writel(value, nand->regs + MA35_NFI_REG_NANDRA0 + j);
		break;
	case 3:
		value = *ptr | (*(ptr+1) << 8) | (*(ptr+2) << 16);
		writel(value, nand->regs + MA35_NFI_REG_NANDRA0 + j);
		break;
	default:
		break;
	}
}

static inline void ma35_nand_target_enable(struct ma35_nand_info *nand)
{
	writel(readl(nand->regs + MA35_NFI_REG_NANDCTL) & (~DISABLE_CS0),
		nand->regs+MA35_NFI_REG_NANDCTL);
}

static inline void ma35_nand_target_disable(struct ma35_nand_info *nand)
{
	writel(readl(nand->regs + MA35_NFI_REG_NANDCTL) | DISABLE_CS0,
		nand->regs + MA35_NFI_REG_NANDCTL);
}

static void ma35_nand_hwecc_init(struct ma35_nand_info *nand)
{
	struct mtd_info *mtd = nand_to_mtd(&nand->chip);
	u32 reg;

	/* resets the internal state machine and counters
	 * This bit will be auto cleared after a few clock cycles.
	 */
	reg = readl(nand->regs + MA35_NFI_REG_NANDCTL);
	reg |= SWRST;
	writel(reg, nand->regs + MA35_NFI_REG_NANDCTL);
	while (readl(nand->regs + MA35_NFI_REG_NANDCTL) & SWRST)
		;

	/* Redundant area size */
	writel(mtd->oobsize, nand->regs + MA35_NFI_REG_NANDRACTL);

	/* Protect redundant 3 bytes */
	reg = readl(nand->regs + MA35_NFI_REG_NANDCTL);
	reg |= (PROT3BEN | ECC_CHK);
	writel(reg, nand->regs + MA35_NFI_REG_NANDCTL);

	if (nand->bch == BCH_NONE) {
		/* Disable H/W ECC, ECC parity check enable bit during read page */
		writel(readl(nand->regs + MA35_NFI_REG_NANDCTL) & (~ECC_EN),
			nand->regs + MA35_NFI_REG_NANDCTL);
	} else {
		/* Set BCH algorithm */
		writel((readl(nand->regs + MA35_NFI_REG_NANDCTL) & (~BCH_MASK)) |
			nand->bch, nand->regs + MA35_NFI_REG_NANDCTL);

		/* Enable H/W ECC, ECC parity check enable bit during read page */
		writel(readl(nand->regs + MA35_NFI_REG_NANDCTL) | ECC_EN,
			nand->regs + MA35_NFI_REG_NANDCTL);
	}
}

/* Correct data by BCH alrogithm */
static void ma35_nfi_correct(struct ma35_nand_info *nand, u8 index,
				 u8 err_cnt, u8 *addr)
{
	u32 temp_data[24], temp_addr[24];
	u32 padding_len, parity_len;
	u32 value, offset, remain;
	u32 err_data[6];
	u8  i, j;

	/* configurations */
	switch (nand->bch) {
	case BCH_T24:
		parity_len = MA35_PARITY_BCH24;
		padding_len = MA35_BCH_PADDING_1024;
		break;
	case BCH_T12:
		parity_len = MA35_PARITY_BCH12;
		padding_len = MA35_BCH_PADDING_512;
		break;
	case BCH_T8:
		parity_len = MA35_PARITY_BCH8;
		padding_len = MA35_BCH_PADDING_512;
		break;
	default:
		dev_warn(nand->dev, "NAND ERROR: invalid SMCR_BCH_TSEL = 0x%08X\n",
			(u32)(readl(nand->regs + MA35_NFI_REG_NANDCTL) & BCH_MASK));
		return;
	}

	/* got valid BCH_ECC_DATAx and parse them to temp_data[]
	 * got the valid register number of BCH_ECC_DATAx since
	 * one register include 4 error bytes
	 */
	j = (err_cnt + 3) / 4;
	j = (j > 6) ? 6 : j;
	for (i = 0; i < j; i++)
		err_data[i] = readl(nand->regs + MA35_NFI_REG_NANDECCED0 + i * 4);

	for (i = 0; i < j; i++) {
		temp_data[i*4+0] = err_data[i] & 0xff;
		temp_data[i*4+1] = (err_data[i] >> 8) & 0xff;
		temp_data[i*4+2] = (err_data[i] >> 16) & 0xff;
		temp_data[i*4+3] = (err_data[i] >> 24) & 0xff;
	}

	/* got valid REG_BCH_ECC_ADDRx and parse them to temp_addr[]
	 * got the valid register number of REG_BCH_ECC_ADDRx since
	 * one register include 2 error addresses
	 */
	j = (err_cnt + 1) / 2;
	j = (j > 12) ? 12 : j;
	for (i = 0; i < j; i++) {
		temp_addr[i*2+0] = readl(nand->regs + MA35_NFI_REG_NANDECCEA0 + i * 4)
					& 0x07ff;
		temp_addr[i*2+1] = (readl(nand->regs + MA35_NFI_REG_NANDECCEA0 + i * 4)
					>> 16) & 0x07ff;
	}

	/* pointer to begin address of field that with data error */
	addr += index * nand->chip.ecc.steps;

	/* correct each error bytes */
	for (i = 0; i < err_cnt; i++) {
		u32 corrected_index = temp_addr[i];

		/* for wrong data in field */
		if (corrected_index < nand->chip.ecc.steps)
			*(addr + corrected_index) ^= temp_data[i];

		/* for wrong first-3-bytes in redundancy area */
		else if (corrected_index < (nand->chip.ecc.steps + 3)) {
			corrected_index -= nand->chip.ecc.steps;
			temp_addr[i] += (parity_len * index);	/* field offset */

			value = readl(nand->regs + MA35_NFI_REG_NANDRA0);
			value ^= temp_data[i] << (8 * corrected_index);
			writel(value, nand->regs + MA35_NFI_REG_NANDRA0);
		}
		/* for wrong parity code in redundancy area
		 * BCH_ERR_ADDRx = [data in field] + [3 bytes] + [xx] + [parity code]
		 *                                   |<--     padding bytes      -->|
		 * The BCH_ERR_ADDRx for last parity code always = field size + padding size.
		 * So, the first parity code = field size + padding size - parity code length.
		 * For example, for BCH T12, the first parity code = 512 + 32 - 23 = 521.
		 * That is, error byte address offset within field is
		 */
		else {
			corrected_index -= (nand->chip.ecc.steps + padding_len - parity_len);

			/* final address = first parity code of first field +
			 *                 offset of fields +
			 *                 offset within field
			 */
			offset = (readl(nand->regs + MA35_NFI_REG_NANDRACTL) & 0x1ff) -
				(parity_len * nand->chip.ecc.steps) +
				(parity_len * index) + corrected_index;

			remain = offset % 4;
			value = readl(nand->regs + MA35_NFI_REG_NANDRA0 + offset - remain);
			value ^= temp_data[i] << (8 * remain);
			writel(value, nand->regs + MA35_NFI_REG_NANDRA0 + offset - remain);
		}
	}
}

static int ma35_nfi_ecc_check(struct nand_chip *chip, u8 *addr)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	int i, j, nchunks = 0;
	int report_err = 0;
	int err_cnt = 0;
	u32 status;

	nchunks = mtd->writesize / chip->ecc.steps;
	if (nchunks < 4)
		nchunks = 1;
	else
		nchunks /= 4;

	for (j = 0; j < nchunks; j++) {
		status = readl(nand->regs + MA35_NFI_REG_NANDECCES0 + j * 4);
		if (!status)
			continue;

		for (i = 0; i < 4; i++) {
			if (!(status & ECC_STATUS_MASK)) {
				/* No error */
				status >>= 8;
				continue;
			} else if ((status & ECC_STATUS_MASK) == 0x01) {
				/* Correctable error */
				err_cnt = (status >> 2) & ECC_ERR_CNT_MASK;
				ma35_nfi_correct(nand, j*4+i, err_cnt, addr);
				report_err += err_cnt;
			} else {
				/* uncorrectable error */
				dev_warn(nand->dev, "uncorrectable error! 0x%4x\n", status);
				return -1;
			}
			status >>= 8;
		}
	}
	return report_err;
}

static void ma35_nand_dmac_init(struct ma35_nand_info *nand)
{
	/* DMAC reset and enable */
	writel(DMA_RST | DMA_EN, nand->regs + MA35_NFI_REG_DMACTL);
	writel(DMA_EN, nand->regs + MA35_NFI_REG_DMACTL);

	/* Clear DMA finished flag */
	writel(INT_DMA | INT_ECC, nand->regs + MA35_NFI_REG_NANDINTSTS);

	init_completion(&nand->complete);
}

static int ma35_nand_do_write(struct nand_chip *chip, const u8 *addr, u32 len)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	dma_addr_t dma_addr;
	int ret = 0, i;
	u32 reg;

	if (len != mtd->writesize) {
		for (i = 0; i < len; i++)
			writel(addr[i], nand->regs + MA35_NFI_REG_NANDDATA);
		return 0;
	}

	ma35_nand_dmac_init(nand);

	writel(mtd->oobsize, nand->regs + MA35_NFI_REG_NANDRACTL);

	writel(INT_DMA, nand->regs + MA35_NFI_REG_NANDINTEN);
	/* To mark this page as dirty. */
	reg = readl(nand->regs + MA35_NFI_REG_NANDRA0);
	if (reg & 0xffff0000)
		writel(reg & 0xffff, nand->regs + MA35_NFI_REG_NANDRA0);

	dma_addr = dma_map_single(nand->dev, (void *)addr, len, DMA_TO_DEVICE);
	ret = dma_mapping_error(nand->dev, dma_addr);
	if (ret) {
		dev_err(nand->dev, "dma mapping error\n");
		return -EINVAL;
	}
	dma_sync_single_for_device(nand->dev, dma_addr, len, DMA_TO_DEVICE);

	writel((unsigned long)dma_addr, nand->regs + MA35_NFI_REG_DMASA);
	writel(readl(nand->regs + MA35_NFI_REG_NANDCTL) | DMA_W_EN,
		nand->regs + MA35_NFI_REG_NANDCTL);
	ret = wait_for_completion_timeout(&nand->complete, msecs_to_jiffies(1000));
	if (!ret) {
		dev_err(nand->dev, "write timeout\n");
		ret = -ETIMEDOUT;
	}

	dma_unmap_single(nand->dev, dma_addr, len, DMA_TO_DEVICE);

	return ret;
}

static int ma35_nand_do_read(struct nand_chip *chip, u8 *addr, u32 len)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret = 0, cnt = 0, i;
	dma_addr_t dma_addr;
	u32 reg;

	if (len != mtd->writesize) {
		for (i = 0; i < len; i++)
			*(addr+i) = (u8)readl(nand->regs + MA35_NFI_REG_NANDDATA);
		return 0;
	}

	ma35_nand_dmac_init(nand);

	writel(mtd->oobsize, nand->regs + MA35_NFI_REG_NANDRACTL);

	/* setup and start DMA using dma_addr */
	dma_addr = dma_map_single(nand->dev, (void *)addr, len, DMA_FROM_DEVICE);
	ret = dma_mapping_error(nand->dev, dma_addr);
	if (ret) {
		dev_err(nand->dev, "dma mapping error\n");
		return -EINVAL;
	}

	writel((unsigned long)dma_addr, nand->regs + MA35_NFI_REG_DMASA);
	writel(readl(nand->regs + MA35_NFI_REG_NANDCTL) | DMA_R_EN,
		nand->regs + MA35_NFI_REG_NANDCTL);
	ret = wait_for_completion_timeout(&nand->complete, msecs_to_jiffies(1000));
	if (!ret) {
		dev_err(nand->dev, "read timeout\n");
		ret = -ETIMEDOUT;
	}

	dma_unmap_single(nand->dev, dma_addr, len, DMA_FROM_DEVICE);

	reg = readl(nand->regs + MA35_NFI_REG_NANDINTSTS);
	if (reg & INT_ECC) {
		cnt = ma35_nfi_ecc_check(&nand->chip, addr);
		if (cnt < 0) {
			mtd->ecc_stats.failed++;
			writel(DMA_RST | DMA_EN, nand->regs + MA35_NFI_REG_DMACTL);
			writel(readl(nand->regs + MA35_NFI_REG_NANDCTL) | SWRST,
				nand->regs + MA35_NFI_REG_NANDCTL);
		} else {
			mtd->ecc_stats.corrected += cnt;
			nand->bitflips = cnt;
		}
		writel(INT_ECC, nand->regs + MA35_NFI_REG_NANDINTSTS);
	}

	return ret;
}

static int ma35_nand_write_page_hwecc(struct nand_chip *chip, const u8 *buf,
				      int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	void *ecc_calc = chip->ecc.calc_buf;

	ma35_clear_spare(chip, mtd->oobsize);
	ma35_write_spare(chip, mtd->oobsize - chip->ecc.total,
			(u32 *)chip->oob_poi);

	nand_prog_page_begin_op(chip, page, 0, buf, mtd->writesize);
	nand_prog_page_end_op(chip);

	/* Copy parity code in NANDRA to calc */
	ma35_read_spare(chip, chip->ecc.total, (u32 *)ecc_calc,
			mtd->oobsize - chip->ecc.total);

	/* Copy parity code in calc to oob_poi */
	memcpy(chip->oob_poi + (mtd->oobsize - chip->ecc.total),
		ecc_calc, chip->ecc.total);

	return 0;
}

static int ma35_nand_read_page_hwecc(struct nand_chip *chip, u8 *buf,
					int oob_required, int page)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	u32 reg;

	/* read the OOB area  */
	nand_read_oob_op(chip, page, 0, chip->oob_poi, mtd->oobsize);
	nand->bitflips = 0;

	/* copy OOB data to NANDRA for page read */
	ma35_write_spare(chip, mtd->oobsize, (u32 *)chip->oob_poi);

	reg = readl(nand->regs + MA35_NFI_REG_NANDRA0);
	if (reg & 0xffff0000) {
		memset((void *)buf, 0xff, mtd->writesize);
	} else {
		/* read data from nand */
		nand_read_page_op(chip, page, 0, buf, mtd->writesize);

		/* restore OOB data from SMRA */
		ma35_read_spare(chip, mtd->oobsize, (u32 *)chip->oob_poi, 0);
	}

	return nand->bitflips;
}

static int ma35_nand_read_oob_hwecc(struct nand_chip *chip, int page)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	u32 reg;

	nand_read_oob_op(chip, page, 0, chip->oob_poi, mtd->oobsize);

	/* copy OOB data to NANDRA for page read */
	ma35_write_spare(chip, mtd->oobsize, (u32 *)chip->oob_poi);

	reg = readl(nand->regs + MA35_NFI_REG_NANDRA0);
	if (reg & 0xffff0000)
		memset((void *)chip->oob_poi, 0xff, mtd->oobsize);

	return 0;
}

static irqreturn_t ma35_nand_irq(int irq, void *id)
{
	struct ma35_nand_info *nand = (struct ma35_nand_info *)id;
	u32 isr;

	isr = readl(nand->regs + MA35_NFI_REG_NANDINTSTS);
	if (isr & INT_DMA) {
		writel(INT_DMA, nand->regs + MA35_NFI_REG_NANDINTSTS);
		complete(&nand->complete);
	}

	return IRQ_HANDLED;
}

static int ma35_nand_attach_chip(struct nand_chip *chip)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	unsigned int reg;

	if (chip->options & NAND_BUSWIDTH_16) {
		dev_err(nand->dev, "16 bits bus width not supported");
		return -EINVAL;
	}

	/* support only ecc hw mode */
	if (chip->ecc.engine_type != NAND_ECC_ENGINE_TYPE_ON_HOST) {
		dev_err(nand->dev, "ecc.engine_type not supported\n");
		return -EINVAL;
	}

	nand->ecc_buf = devm_kzalloc(nand->dev, mtd->writesize + mtd->oobsize,
					GFP_KERNEL);
	if (!nand->ecc_buf)
		return  -ENOMEM;
	chip->ecc.calc_buf = nand->ecc_buf;

	/* Set PSize */
	reg = readl(nand->regs + MA35_NFI_REG_NANDCTL) & (~PSIZE_MASK);
	if (mtd->writesize == 2048)
		writel(reg | PSIZE_2K, nand->regs + MA35_NFI_REG_NANDCTL);
	else if (mtd->writesize == 4096)
		writel(reg | PSIZE_4K, nand->regs + MA35_NFI_REG_NANDCTL);
	else if (mtd->writesize == 8192)
		writel(reg | PSIZE_8K, nand->regs + MA35_NFI_REG_NANDCTL);

	chip->ecc.steps = mtd->writesize / chip->ecc.size;
	if (chip->ecc.strength == 0) {
		nand->bch = BCH_NONE; /* No ECC */
		chip->ecc.total = 0;
	} else if (chip->ecc.strength <= 8) {
		nand->bch = BCH_T8; /* T8 */
		chip->ecc.total = chip->ecc.steps * MA35_PARITY_BCH8;
	} else if (chip->ecc.strength <= 12) {
		nand->bch = BCH_T12; /* T12 */
		chip->ecc.total = chip->ecc.steps * MA35_PARITY_BCH12;
	} else if (chip->ecc.strength <= 24) {
		nand->bch = BCH_T24; /* T24 */
		chip->ecc.total = chip->ecc.steps * MA35_PARITY_BCH24;
	} else {
		dev_warn(nand->dev, "NAND Controller is not support this flash. (%d, %d)\n",
			mtd->writesize, mtd->oobsize);
	}

	chip->ecc.bytes = chip->ecc.total / chip->ecc.steps;
	mtd_set_ooblayout(mtd, &ma35_ooblayout_ops);

	/* add mtd-id. The string should same as uboot definition */
	mtd->name = "nand0";

	ma35_nand_hwecc_init(nand);

	writel(DISABLE_WP, nand->regs + MA35_NFI_REG_NANDECTL);

	return 0;
}

static int ma35_nfc_exec_instr(struct nand_chip *chip,
			      const struct nand_op_instr *instr)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	unsigned int i;
	u32 status;

	switch (instr->type) {
	case NAND_OP_CMD_INSTR:
		writel(instr->ctx.cmd.opcode, nand->regs + MA35_NFI_REG_NANDCMD);
		return 0;
	case NAND_OP_ADDR_INSTR:
		for (i = 0; i < instr->ctx.addr.naddrs; i++) {
			if (i == (instr->ctx.addr.naddrs - 1))
				writel(instr->ctx.addr.addrs[i] | ENDADDR,
					nand->regs + MA35_NFI_REG_NANDADDR);
			else
				writel(instr->ctx.addr.addrs[i],
					nand->regs + MA35_NFI_REG_NANDADDR);
		}
		return 0;
	case NAND_OP_DATA_IN_INSTR:
		ma35_nand_do_read(chip, instr->ctx.data.buf.in, instr->ctx.data.len);
		return 0;
	case NAND_OP_DATA_OUT_INSTR:
		ma35_nand_do_write(chip, instr->ctx.data.buf.out, instr->ctx.data.len);
		return 0;
	case NAND_OP_WAITRDY_INSTR:
		return readl_poll_timeout(nand->regs + MA35_NFI_REG_NANDINTSTS, status,
					  status & INT_RB0, 20,
					  instr->ctx.waitrdy.timeout_ms * 1000);
	default:
		break;
	}

	return -EINVAL;
}

static int ma35_nfc_exec_op(struct nand_chip *chip,
			  const struct nand_operation *op,
			  bool check_only)
{
	struct ma35_nand_info *nand = nand_get_controller_data(chip);
	int ret = 0;
	u32 i, reg;

	if (check_only)
		return 0;

	ma35_nand_target_enable(nand);

	reg = readl(nand->regs + MA35_NFI_REG_NANDINTSTS);
	reg |= INT_RB0;
	writel(reg, nand->regs + MA35_NFI_REG_NANDINTSTS);

	for (i = 0; i < op->ninstrs; i++) {
		ret = ma35_nfc_exec_instr(chip, &op->instrs[i]);
		if (ret)
			break;
	}

	ma35_nand_target_disable(nand);

	return ret;
}

static const struct nand_controller_ops ma35_nfc_ops = {
	.attach_chip = ma35_nand_attach_chip,
	.exec_op = ma35_nfc_exec_op,
};

static int ma35_nand_probe(struct platform_device *pdev)
{
	struct ma35_nand_info *nand;
	struct nand_chip *chip;
	struct mtd_info *mtd;
	int ret = 0;

	nand = devm_kzalloc(&pdev->dev, sizeof(*nand), GFP_KERNEL);
	if (!nand)
		return -ENOMEM;

	nand_controller_init(&nand->controller);
	nand->controller.ops = &ma35_nfc_ops;

	nand->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(nand->regs))
		return PTR_ERR(nand->regs);

	nand->dev = &pdev->dev;
	chip = &nand->chip;
	nand_set_controller_data(chip, nand);
	nand_set_flash_node(chip, pdev->dev.of_node);

	nand->clk = devm_clk_get_enabled(&pdev->dev, "nand_gate");
	if (IS_ERR(nand->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(nand->clk),
				     "failed to find nand clock\n");

	nand->irq = platform_get_irq(pdev, 0);
	if (nand->irq < 0)
		return dev_err_probe(&pdev->dev, nand->irq,
				     "failed to get platform irq\n");

	ret = devm_request_irq(&pdev->dev, nand->irq, ma35_nand_irq,
				  IRQF_TRIGGER_HIGH, "ma35d1-nand", nand);
	if (ret) {
		dev_err(&pdev->dev, "failed to request NAND irq\n");
		return -ENXIO;
	}

	nand->chip.controller = &nand->controller;
	platform_set_drvdata(pdev, nand);

	chip->options |= NAND_NO_SUBPAGE_WRITE | NAND_USES_DMA;

	chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	chip->ecc.write_page = ma35_nand_write_page_hwecc;
	chip->ecc.read_page  = ma35_nand_read_page_hwecc;
	chip->ecc.read_oob   = ma35_nand_read_oob_hwecc;

	mtd = nand_to_mtd(chip);
	mtd->priv = chip;
	mtd->owner = THIS_MODULE;
	mtd->dev.parent = &pdev->dev;

	writel(NAND_EN, nand->regs + MA35_NFI_REG_GCTL);

	ret = nand_scan(chip, 1);
	if (ret)
		return ret;

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		nand_cleanup(chip);
		return ret;
	}

	return ret;
}

static void ma35_nand_remove(struct platform_device *pdev)
{
	struct ma35_nand_info *nand = platform_get_drvdata(pdev);
	int ret;

	ret = mtd_device_unregister(nand_to_mtd(&nand->chip));
	WARN_ON(ret);
	nand_cleanup(&nand->chip);
}

/* PM Support */
#ifdef CONFIG_PM
static int ma35_nand_suspend(struct platform_device *pdev, pm_message_t pm)
{
	struct ma35_nand_info *nand = platform_get_drvdata(pdev);
	int ret = 0;
	u32 val;

	/* wait DMAC to ready */
	ret = readl_poll_timeout(nand->regs + MA35_NFI_REG_DMACTL, val,
				 !(val & DMA_BUSY), 50, HZ/2);
	if (ret)
		dev_warn(&pdev->dev, "dma busy\n");

	clk_disable(nand->clk);

	return ret;
}

static int ma35_nand_resume(struct platform_device *pdev)
{
	struct ma35_nand_info *nand = platform_get_drvdata(pdev);

	clk_enable(nand->clk);
	ma35_nand_hwecc_init(nand);
	ma35_nand_dmac_init(nand);

	return 0;
}

#else
#define ma35_nand_suspend NULL
#define ma35_nand_resume NULL
#endif

static const struct of_device_id ma35_nfi_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-nand" },
	{},
};
MODULE_DEVICE_TABLE(of, ma35_nfi_of_match);

static struct platform_driver ma35_nand_driver = {
	.driver = {
		.name = "ma35d1-nand",
		.of_match_table = ma35_nfi_of_match,
	},
	.probe = ma35_nand_probe,
	.remove = ma35_nand_remove,
	.suspend = ma35_nand_suspend,
	.resume = ma35_nand_resume,
};

module_platform_driver(ma35_nand_driver);

MODULE_DESCRIPTION("Nuvoton ma35 NAND driver");
MODULE_AUTHOR("Hui-Ping Chen <hpchen0nvt@gmail.com>");
MODULE_LICENSE("GPL");
