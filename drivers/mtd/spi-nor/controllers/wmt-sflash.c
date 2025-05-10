// SPDX-License-Identifier: GPL-2.0-only
/*
 * VIA/WonderMedia SPI NOR flash controller driver
 *
 * Copyright (c) 2025 Alexey Charkov <alchark@gmail.com>
 */
#include <linux/clk.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define SF_CHIP_SEL_0_CFG	0x000		/* chip select 0 config */
#define SF_CHIP_SEL_1_CFG	0x008		/* chip select 0 config */
#define SF_CHIP_SEL_CFG(x)	(8 * (x))
#define SF_CHIP_SEL_ADDR	GENMASK(31, 16) /* 64kb aligned address */
#define SF_CHIP_SEL_SIZE	GENMASK(11, 8)	/* log2(size/32kb) */

#define SF_SPI_INTF_CFG		0x040	/* SPI interface config */
#define SF_ADDR_WIDTH_32	BIT(0)	/* 0: 24 bit, 1: 32 bit addr */
#define SF_USR_RD_CMD_MOD	BIT(4)	/* 0: normal, 1: user cmd read */
#define SF_USR_WR_CMD_MOD	BIT(5)	/* 0: normal, 1: user cmd write */
#define SF_PROG_CMD_MOD		BIT(6)	/* 0: normal, 1: prog cmd */
#define SF_CS_DELAY		GENMASK(18, 16)	/* chip select delay */
#define SF_RES_DELAY		GENMASK(27, 24) /* reset delay */
#define SF_PDWN_DELAY		GENMASK(31, 28) /* power down delay */

#define SF_SPI_RD_WR_CTR	0x050	/* read/write control */
#define SF_RD_FAST		BIT(0)	/* 0: normal read, 1: fast read */
#define SF_RD_ID		BIT(4)	/* 0: read status, 1: read ID */

#define SF_SPI_WR_EN_CTR	0x060	/* write enable control */
#define SF_CS0_WR_EN		BIT(0)
#define SF_CS1_WR_EN		BIT(1)
#define SF_CS_WR_EN(x)		BIT(x)

#define SF_SPI_ER_CTR		0x070	/* erase control */
#define SF_CHIP_ERASE		BIT(0)	/* full chip erase */
#define SF_SEC_ERASE		BIT(15)	/* sector erase */

#define SF_SPI_ER_START_ADDR	0x074	/* erase start address */
#define SF_CHIP_ER_CS0		BIT(0)	/* erase chip 0 */
#define SF_CHIP_ER_CS1		BIT(1)	/* erase chip 1 */
#define SF_CHIP_ER_CS(x)	BIT(x)
#define SF_ER_START_ADDR	GENMASK(31, 16)

#define SF_SPI_ERROR_STATUS	0x080
#define SF_MASLOCK_ERR		BIT(0)	/* master lock */
#define SF_PCMD_ACC_ERR		BIT(1)	/* programmable cmd access */
#define SF_PCMD_OP_ERR		BIT(2)	/* programmable cmd opcode */
#define SF_PWR_DWN_ACC_ERR	BIT(3)	/* power down access */
#define SF_MEM_REGION_ERR	BIT(4)	/* memory region */
#define SF_WR_PROT_ERR		BIT(5)	/* write protection */
#define SF_SPI_ERROR_CLEARALL	(SF_MASLOCK_ERR | \
				 SF_PCMD_ACC_ERR | \
				 SF_PCMD_OP_ERR | \
				 SF_PWR_DWN_ACC_ERR | \
				 SF_MEM_REGION_ERR | \
				 SF_WR_PROT_ERR)

#define SF_SPI_MEM_0_SR_ACC	0x100	/* status read from chip 0 */
#define SF_SPI_MEM_1_SR_ACC	0x110	/* status read from chip 1 */
#define SF_SPI_MEM_SR_ACC(x)	(0x100 + 0x10 * (x))

#define SF_SPI_PDWN_CTR_0	0x180	/* power down chip 0 */
#define SF_SPI_PDWN_CTR_1	0x190	/* power down chip 1 */
#define SF_SPI_PDWN_CTR_(x)	(0x180 + 0x10 * (x))
#define SF_PWR_DOWN		BIT(0)

#define SF_SPI_PROG_CMD_CTR	0x200	/* programmable cmd control */
#define SF_PROG_CMD_EN		BIT(0)	/* enable programmable cmd */
#define SF_PROG_CMD_CS		GENMASK(1, 1)	/* chip select for cmd */
#define SF_RX_DATA_SIZE		GENMASK(22, 16)	/* receive data size */
#define SF_TX_DATA_SIZE		GENMASK(30, 24)	/* transmit data size */

#define SF_SPI_USER_CMD_VAL	0x210
#define SF_USR_RD_CMD		GENMASK(7, 0)	/* user read command */
#define SF_USR_WR_CMD		GENMASK(23, 16)	/* user write command */

#define SF_SPI_PROG_CMD_WBF	0x300	/* 64 bytes pcmd write buffer */
#define SF_SPI_PROG_CMD_RBF	0x380	/* 64 bytes pcmd read buffer */

#define SF_WAIT_TIMEOUT		1000000

struct wmt_sflash_priv {
	size_t			cs;
	struct wmt_sflash_host	*host;
	void __iomem		*mmap_base;
	resource_size_t		mmap_phys;
};

#define SF_MAX_CHIP_NUM		2
struct wmt_sflash_host {
	struct device		*dev;
	struct clk		*clk;

	void __iomem		*regbase;
	struct resource		*mmap_res[SF_MAX_CHIP_NUM];

	struct spi_nor		*nor[SF_MAX_CHIP_NUM];
	size_t			num_chips;
};

static int wmt_sflash_prep(struct spi_nor *nor)
{
	struct wmt_sflash_priv *priv = nor->priv;
	struct wmt_sflash_host *host = priv->host;

	return clk_prepare_enable(host->clk);
}

static void wmt_sflash_unprep(struct spi_nor *nor)
{
	struct wmt_sflash_priv *priv = nor->priv;
	struct wmt_sflash_host *host = priv->host;

	clk_disable_unprepare(host->clk);
}

static void wmt_sflash_pcmd_mode(struct wmt_sflash_host *host, bool enable)
{
	u32 reg = readl(host->regbase + SF_SPI_INTF_CFG);

	reg &= ~SF_PROG_CMD_MOD;
	reg |= FIELD_PREP(SF_PROG_CMD_MOD, enable);
	writel(reg, host->regbase + SF_SPI_INTF_CFG);
}

static inline int wmt_sflash_wait_pcmd(struct wmt_sflash_host *host)
{
	u32 reg;

	return readl_poll_timeout(host->regbase + SF_SPI_PROG_CMD_CTR, reg,
		!(reg & SF_PROG_CMD_EN), 1, SF_WAIT_TIMEOUT);
}

static int wmt_sflash_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf,
			       size_t len)
{
	struct wmt_sflash_priv *priv = nor->priv;
	struct wmt_sflash_host *host = priv->host;
	int ret;
	u32 reg;

	if (len > 64) {
		dev_err(host->dev,
		"Cannot read %d bytes from registers\n", len);
		return -EINVAL;
	}

	wmt_sflash_pcmd_mode(host, true);
	writeb(opcode, host->regbase + SF_SPI_PROG_CMD_WBF);

	reg = SF_PROG_CMD_EN |
	      FIELD_PREP(SF_PROG_CMD_CS, priv->cs) |
	      FIELD_PREP(SF_TX_DATA_SIZE, 1) |
	      FIELD_PREP(SF_RX_DATA_SIZE, len);
	writel(reg, host->regbase + SF_SPI_PROG_CMD_CTR);

	ret = wmt_sflash_wait_pcmd(host);

	if (len)
		memcpy_fromio(buf, host->regbase + SF_SPI_PROG_CMD_RBF, len);

	wmt_sflash_pcmd_mode(host, false);

	return ret;
}

static int wmt_sflash_write_reg(struct spi_nor *nor, u8 opcode, const u8 *buf,
				size_t len)
{
	struct wmt_sflash_priv *priv = nor->priv;
	struct wmt_sflash_host *host = priv->host;
	int ret;
	u32 reg;

	if (len > 63) {
		dev_err(host->dev,
		"Cannot write %d bytes to registers\n", len);
		return -EINVAL;
	}

	wmt_sflash_pcmd_mode(host, true);
	writeb(opcode, host->regbase + SF_SPI_PROG_CMD_WBF);

	if (len)
		memcpy_toio(host->regbase + SF_SPI_PROG_CMD_WBF + 1, buf, len);

	reg = SF_PROG_CMD_EN |
	      FIELD_PREP(SF_PROG_CMD_CS, priv->cs) |
	      FIELD_PREP(SF_TX_DATA_SIZE, len + 1);
	writel(reg, host->regbase + SF_SPI_PROG_CMD_CTR);

	ret = wmt_sflash_wait_pcmd(host);
	wmt_sflash_pcmd_mode(host, false);

	return ret;
}

static int wmt_sflash_wait_spi(struct wmt_sflash_priv *priv)
{
	struct wmt_sflash_host *host = priv->host;
	int timeout = SF_WAIT_TIMEOUT;
	u32 error;

	while (timeout--) {
		if (!(readl(host->regbase +
			    SF_SPI_MEM_SR_ACC(priv->cs)) & 1))
			return 0;

		error = readl(host->regbase + SF_SPI_ERROR_STATUS);
		if (error & SF_MASLOCK_ERR) {
			dev_err(host->dev,
				"Master lock error\n");
			goto err;
		}
		if (error & SF_PCMD_ACC_ERR) {
			dev_err(host->dev,
				"Programmable command access error\n");
			goto err;
		}
		if (error & SF_PCMD_OP_ERR) {
			dev_err(host->dev,
				"Programmable command opcode error\n");
			goto err;
		}
		if (error & SF_PWR_DWN_ACC_ERR) {
			dev_err(host->dev,
				"Power down access error\n");
			goto err;
		}
		if (error & SF_MEM_REGION_ERR) {
			dev_err(host->dev,
				"Memory region error\n");
			goto err;
		}
		if (error & SF_WR_PROT_ERR) {
			dev_err(host->dev,
				"Write protection error\n");
			goto err;
		}
	}
	return 0;

err:
	writel(SF_SPI_ERROR_CLEARALL, host->regbase + SF_SPI_ERROR_STATUS);
	return -EBUSY;
}

static ssize_t wmt_sflash_read(struct spi_nor *nor, loff_t from, size_t len,
			       u_char *read_buf)
{
	struct wmt_sflash_priv *priv = nor->priv;
	struct wmt_sflash_host *host = priv->host;
	u32 reg = nor->read_opcode == SPINOR_OP_READ_FAST ? SF_RD_FAST : 0;

	writel(reg, host->regbase + SF_SPI_RD_WR_CTR);

	if (wmt_sflash_wait_spi(priv))
		return 0;

	memcpy_fromio(read_buf, priv->mmap_base + from, len);
	return len;
}

static ssize_t wmt_sflash_write(struct spi_nor *nor, loff_t to, size_t len,
				const u_char *write_buf)
{
	struct wmt_sflash_priv *priv = nor->priv;
	struct wmt_sflash_host *host = priv->host;
	size_t burst, offset = 0;

	writel(SF_CS_WR_EN(priv->cs),
	       host->regbase + SF_SPI_WR_EN_CTR);

	while (offset < len) {
		/* select 8 / 4 / 2 / 1 byte write length */
		burst = 1 << min(3, ilog2(len - offset));
		memcpy_toio(priv->mmap_base + to + offset,
			    write_buf + offset, burst);

		if (wmt_sflash_wait_spi(priv))
			return offset;

		offset += burst;
	}

	writel(0, host->regbase + SF_SPI_WR_EN_CTR);

	return offset;
}

static int wmt_sflash_erase(struct spi_nor *nor, loff_t offs)
{
	struct wmt_sflash_priv *priv = nor->priv;
	struct wmt_sflash_host *host = priv->host;
	int ret = 0;
	u32 reg;

	if (offs & (SZ_64K - 1)) {
		dev_err(host->dev,
			"Erase offset 0x%llx not on 64k boundary\n", offs);
		return -EINVAL;
	}

	writel(SF_CS_WR_EN(priv->cs),
	       host->regbase + SF_SPI_WR_EN_CTR);

	reg = SF_CHIP_ER_CS(priv->cs) |
	      FIELD_PREP(SF_ER_START_ADDR, (priv->mmap_phys + offs) >> 16);
	writel(reg, host->regbase + SF_SPI_ER_START_ADDR);

	writel(SF_SEC_ERASE, host->regbase + SF_SPI_ER_CTR);

	ret = wmt_sflash_wait_spi(priv);
	writel(0, host->regbase + SF_SPI_WR_EN_CTR);

	return ret;
}

static const struct spi_nor_controller_ops wmt_sflash_controller_ops = {
	.prepare	= wmt_sflash_prep,
	.unprepare	= wmt_sflash_unprep,
	.read_reg	= wmt_sflash_read_reg,
	.write_reg	= wmt_sflash_write_reg,
	.read		= wmt_sflash_read,
	.write		= wmt_sflash_write,
	.erase		= wmt_sflash_erase,
};

static int wmt_sflash_register(struct device_node *np,
				   struct wmt_sflash_host *host)
{
	const struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
			SNOR_HWCAPS_READ_FAST |
			SNOR_HWCAPS_PP,
	};
	struct device *dev = host->dev;
	struct wmt_sflash_priv *priv;
	struct mtd_info *mtd;
	struct spi_nor *nor;
	int ret;
	u32 reg;

	nor = devm_kzalloc(dev, sizeof(*nor), GFP_KERNEL);
	if (!nor)
		return -ENOMEM;

	nor->dev = dev;
	spi_nor_set_flash_node(nor, np);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = of_property_read_u32(np, "reg", &priv->cs);
	if (ret)
		return dev_err_probe(dev, ret,
				     "There's no reg property for %pOF\n",
				     np);

	if (priv->cs >= SF_MAX_CHIP_NUM)
		return dev_err_probe(dev, -ENXIO,
				     "Chip select %d is out of bounds\n",
				     priv->cs);

	priv->host = host;
	nor->priv = priv;
	nor->controller_ops = &wmt_sflash_controller_ops;

	ret = spi_nor_scan(nor, NULL, &hwcaps);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to scan SPI NOR chip\n");

	mtd = &nor->mtd;
	mtd->name = np->name;

	priv->mmap_phys = host->mmap_res[priv->cs]->end - mtd->size + 1;
	priv->mmap_phys &= -SZ_64K;

	priv->mmap_base = devm_ioremap(dev, priv->mmap_phys, mtd->size);
	if (IS_ERR(priv->mmap_base))
		return dev_err_probe(dev, PTR_ERR(priv->mmap_base),
			"Failed to map chip %d at address 0x%x size 0x%llx\n",
			priv->cs, priv->mmap_phys, mtd->size);

	reg = FIELD_PREP(SF_CHIP_SEL_ADDR, priv->mmap_phys >> 16) |
	      FIELD_PREP(SF_CHIP_SEL_SIZE, order_base_2(mtd->size) - 15);
	writel(reg, host->regbase + SF_CHIP_SEL_CFG(priv->cs));

	reg = FIELD_PREP(SF_CS_DELAY, 3);
	writel(reg, host->regbase + SF_SPI_INTF_CFG);

	/* the controller only handles 64k aligned addresses */
	mtd->erasesize = max(mtd->erasesize, SZ_64K);

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register MTD device\n");

	host->nor[host->num_chips] = nor;
	host->num_chips++;
	return 0;
}

static void wmt_sflash_unregister_all(struct wmt_sflash_host *host)
{
	int i;

	for (i = 0; i < host->num_chips; i++)
		mtd_device_unregister(&host->nor[i]->mtd);
}

static int wmt_sflash_register_all(struct wmt_sflash_host *host)
{
	struct device *dev = host->dev;
	struct device_node *np;
	int ret;

	for_each_available_child_of_node(dev->of_node, np) {
		ret = wmt_sflash_register(np, host);
		if (ret) {
			of_node_put(np);
			goto fail;
		}

		if (host->num_chips == SF_MAX_CHIP_NUM) {
			dev_warn(dev, "Flash count exceeds the maximum chipselect number\n");
			of_node_put(np);
			break;
		}
	}
	return 0;

fail:
	wmt_sflash_unregister_all(host);
	return ret;
}

static int wmt_sflash_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wmt_sflash_host *host;
	char mmap_str[32];
	int ret, i;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return dev_err_probe(dev, -ENOMEM,
			"Failed to allocate controller private data\n");

	platform_set_drvdata(pdev, host);
	host->dev = dev;

	host->regbase = devm_platform_ioremap_resource_byname(pdev, "io");
	if (IS_ERR(host->regbase))
		return dev_err_probe(dev, PTR_ERR(host->regbase),
			"Failed to remap controller MMIO registers\n");

	for (i = 0; i < SF_MAX_CHIP_NUM; i++) {
		snprintf(mmap_str, sizeof(mmap_str), "chip%d-mmap", i);

		host->mmap_res[i] = platform_get_resource_byname(pdev,
						IORESOURCE_MEM, mmap_str);
		if (!host->mmap_res[i])
			return dev_err_probe(dev, -ENXIO,
				"Memory map region not found for chip %d\n",
				i);
	}

	host->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(host->clk))
		return dev_err_probe(dev, PTR_ERR(host->clk),
			"Failed to get clock\n");

	ret = clk_prepare_enable(host->clk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable clock\n");

	ret = wmt_sflash_register_all(host);

	clk_disable_unprepare(host->clk);
	return ret;
}

static void wmt_sflash_remove(struct platform_device *pdev)
{
	struct wmt_sflash_host *host = platform_get_drvdata(pdev);

	wmt_sflash_unregister_all(host);
}

static const struct of_device_id wmt_sflash_dt_ids[] = {
	{ .compatible = "via,vt8500-sflash"},
	{ .compatible = "wm,wm8505-sflash"},
	{ .compatible = "wm,wm8650-sflash"},
	{ .compatible = "wm,wm8750-sflash"},
	{ .compatible = "wm,wm8850-sflash"},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wmt_sflash_dt_ids);

static struct platform_driver wmt_sflash_driver = {
	.driver = {
		.name	= "wmt-sflash",
		.of_match_table = wmt_sflash_dt_ids,
	},
	.probe	= wmt_sflash_probe,
	.remove = wmt_sflash_remove,
};
module_platform_driver(wmt_sflash_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VIA/WonderMedia SPI NOR flash controller driver");
