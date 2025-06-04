// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Amlogic SPI communication Scatter-Gather Controller
 *
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 *
 * Author: Sunny Luo <sunny.luo@amlogic.com>
 * Author: Xianwei Zhao <xianwei.zhao@amlogic.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/clk-provider.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/reset.h>
#include <linux/pinctrl/consumer.h>
#include <linux/delay.h>
#include <linux/cacheflush.h>

/* Register Map */
#define SPICC_REG_CFG_READY		0x00
#define SPICC_REG_CFG_SPI		0x04
#define HW_POS				BIT(6)
#define HW_NEG				BIT(7)
#define SPICC_REG_CFG_START		0x08
#define SPICC_REG_CFG_BUS		0x0C
#define SPICC_REG_PIO_TX_DATA_L		0x10
#define SPICC_REG_PIO_TX_DATA_H		0x14
#define SPICC_REG_PIO_RX_DATA_L		0x18
#define SPICC_REG_PIO_RX_DATA_H		0x1C
#define SPICC_REG_MEM_TX_ADDR_L		0x10
#define SPICC_REG_MEM_TX_ADDR_H		0x14
#define SPICC_REG_MEM_RX_ADDR_L		0x18
#define SPICC_REG_MEM_RX_ADDR_H		0x1C
#define SPICC_REG_DESC_LIST_L		0x20
#define SPICC_REG_DESC_LIST_H		0x24
#define SPICC_DESC_PENDING		BIT(31)
#define SPICC_REG_DESC_CURRENT_L	0x28
#define SPICC_REG_DESC_CURRENT_H	0x2c
#define SPICC_REG_IRQ_STS		0x30
#define SPICC_REG_IRQ_ENABLE		0x34
#define SPICC_RCH_DESC_EOC		BIT(0)
#define SPICC_RCH_DESC_INVALID		BIT(1)
#define SPICC_RCH_DESC_RESP		BIT(2)
#define SPICC_RCH_DATA_RESP		BIT(3)
#define SPICC_WCH_DESC_EOC		BIT(4)
#define SPICC_WCH_DESC_INVALID		BIT(5)
#define SPICC_WCH_DESC_RESP		BIT(6)
#define SPICC_WCH_DATA_RESP		BIT(7)
#define SPICC_DESC_ERR			BIT(8)
#define SPICC_SPI_READY			BIT(9)
#define SPICC_DESC_DONE			BIT(10)
#define SPICC_DESC_CHAIN_DONE		BIT(11)

#define SPICC_BLOCK_MAX			0x100000

#define SPICC_OP_MODE_WRITE_CMD		0
#define SPICC_OP_MODE_READ_STS		1
#define SPICC_OP_MODE_WRITE		2
#define SPICC_OP_MODE_READ		3

#define SPICC_DATA_MODE_NONE		0
#define SPICC_DATA_MODE_PIO		1
#define SPICC_DATA_MODE_MEM		2
#define SPICC_DATA_MODE_SG		3

#define SPICC_CLK_DIV_SHIFT		0
#define SPICC_CLK_DIV_WIDTH		8
#define SPICC_CLK_DIV_MASK		GENMASK(7, 0)
#define SPICC_CLK_DIV_MAX		256
/* recommended by specification */
#define SPICC_CLK_DIV_MIN		4

#define SPICC_PCLK_RATE_MIN		24000000

#define SPICC_SINGLE_SPI		0
#define SPICC_DUAL_SPI			1
#define SPICC_QUAD_SPI			2

union spicc_cfg_spi {
	u32			d32;
	struct  {
		u32		bus64_en:1;
		u32		slave_en:1;
		u32		ss:2;
		u32		flash_wp_pin_en:1;
		u32		flash_hold_pin_en:1;
		u32		hw_pos:1; /* start on vsync rising */
		u32		hw_neg:1; /* start on vsync falling */
		u32		rsv:24;
	} b;
};

union spicc_cfg_start {
	u32			d32;
	struct  {
		u32		block_num:20;
		u32		block_size:3;
		u32		dc_level:1;
		u32		op_mode:2;
		u32		rx_data_mode:2;
		u32		tx_data_mode:2;
		u32		eoc:1;
		u32		pending:1;
	} b;
};

union spicc_cfg_bus {
	u32			d32;
	struct  {
		u32		clk_div:8;
		u32		rx_tuning_in_pclk:4; /* signed */
		u32		tx_tuning_in_pclk:4; /* signed */
		u32		cs_setup_in_sclk:4;
		u32		lane:2;
		u32		half_duplex_en:1;
		u32		little_endian_en:1;
		u32		dc_mode:1;
		u32		null_ctl:1;
		u32		dummy_ctl:1;
		u32		read_turn_around:2;
		u32		keep_ss:1;
		u32		cpha:1;
		u32		cpol:1;
	} b;
};

struct spicc_sg_link {
	u32			valid:1;
	u32			eoc:1;
	u32			irq:1;
	u32			act:3;
	u32			ring:1;
	u32			rsv:1;
	u32			len:24;
	u32			addr;
};

struct spicc_descriptor {
	union spicc_cfg_start		cfg_start;
	union spicc_cfg_bus		cfg_bus;
	u64				tx_paddr;
	u64				rx_paddr;
};

struct spicc_descriptor_extra {
	struct spicc_sg_link		*tx_ccsg;
	struct spicc_sg_link		*rx_ccsg;
	int				tx_ccsg_len;
	int				rx_ccsg_len;
};

struct spicc_device {
	struct spi_controller		*controller;
	struct platform_device		*pdev;
	void __iomem			*base;
	struct clk			*core;
	struct clk			*pclk;
	struct clk			*sclk;
	struct completion		completion;
	u32				status;
	u32				speed_hz;
	u32				effective_speed_hz;
	u32				bytes_per_word;
	union spicc_cfg_spi		cfg_spi;
	union spicc_cfg_start		cfg_start;
	union spicc_cfg_bus		cfg_bus;
};

#define spicc_writel(_spicc, _val, _offset) \
	 writel(_val, (_spicc)->base + (_offset))
#define spicc_readl(_spicc, _offset) \
	readl((_spicc)->base + (_offset))

static int spi_delay_to_sclk(u32 slck_speed_hz, struct spi_delay *delay)
{
	u32 ns;

	if (!delay)
		return 0;

	if (delay->unit == SPI_DELAY_UNIT_SCK)
		return delay->value;

	ns = spi_delay_to_ns(delay, NULL);
	if (ns < 0)
		return 0;

	return DIV_ROUND_UP_ULL(slck_speed_hz * ns, NSEC_PER_SEC);
}

static inline int aml_spicc_sem_down_read(struct spicc_device *spicc)
{
	int ret;

	ret = spicc_readl(spicc, SPICC_REG_CFG_READY);
	if (ret)
		spicc_writel(spicc, 0, SPICC_REG_CFG_READY);

	return ret;
}

static inline void aml_spicc_sem_up_write(struct spicc_device *spicc)
{
	spicc_writel(spicc, 1, SPICC_REG_CFG_READY);
}

static int aml_spicc_set_speed(struct spicc_device *spicc, uint speed_hz)
{
	u32 div;

	if (!speed_hz || speed_hz == spicc->speed_hz)
		return 0;

	spicc->speed_hz = speed_hz;
	clk_set_rate(spicc->sclk, speed_hz);
	/* Store the div for the descriptor mode */
	div = FIELD_GET(SPICC_CLK_DIV_MASK,
			spicc_readl(spicc, SPICC_REG_CFG_BUS));
	spicc->cfg_bus.b.clk_div = div;
	spicc->effective_speed_hz = clk_get_rate(spicc->sclk);
	dev_dbg(&spicc->pdev->dev,
		"desired speed %dHz, effective speed %dHz, div=%d\n",
		speed_hz, spicc->effective_speed_hz, div);

	return 0;
}

static int aml_spicc_setup_device(struct spicc_device *spicc,
				  struct spi_device *spi)
{
	if (!spi->bits_per_word || spi->bits_per_word % 8) {
		dev_err(&spicc->pdev->dev, "invalid wordlen %d\n", spi->bits_per_word);
		return -EINVAL;
	}

	spicc->bytes_per_word = spi->bits_per_word >> 3;
	spicc->cfg_start.b.block_size = spicc->bytes_per_word & 0x7;
	spicc->cfg_spi.b.ss = spi_get_chipselect(spi, 0);

	spicc->cfg_bus.b.cpol = !!(spi->mode & SPI_CPOL);
	spicc->cfg_bus.b.cpha = !!(spi->mode & SPI_CPHA);
	spicc->cfg_bus.b.little_endian_en = !!(spi->mode & SPI_LSB_FIRST);
	spicc->cfg_bus.b.half_duplex_en = !!(spi->mode & SPI_3WIRE);

	return 0;
}

static bool aml_spicc_can_dma(struct spi_controller *ctlr,
			      struct spi_device *spi,
			      struct spi_transfer *xfer)
{
	return true;
}

static void aml_spicc_sg_xlate(struct sg_table *sgt, struct spicc_sg_link *ccsg)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		ccsg->valid = 1;
		/* EOC specially for the last sg */
		ccsg->eoc = sg_is_last(sg);
		ccsg->ring = 0;
		ccsg->len = sg_dma_len(sg);
		ccsg->addr = (u32)sg_dma_address(sg);
		ccsg++;
	}
}

static int nbits_to_lane[] = {
	SPICC_SINGLE_SPI,
	SPICC_SINGLE_SPI,
	SPICC_DUAL_SPI,
	-EINVAL,
	SPICC_QUAD_SPI
};

static int aml_spicc_setup_transfer(struct spicc_device *spicc,
				    struct spi_transfer *xfer,
				    struct spicc_descriptor *desc,
				    struct spicc_descriptor_extra *exdesc)
{
	int block_size, blocks;
	struct device *dev = &spicc->pdev->dev;
	struct spicc_sg_link *ccsg;
	int ccsg_len;
	dma_addr_t paddr;
	int ret;

	memset(desc, 0, sizeof(*desc));
	if (exdesc)
		memset(exdesc, 0, sizeof(*exdesc));
	aml_spicc_set_speed(spicc, xfer->speed_hz);
	xfer->effective_speed_hz = spicc->effective_speed_hz;
	desc->cfg_start.d32 = spicc->cfg_start.d32;
	desc->cfg_bus.d32 = spicc->cfg_bus.d32;

	block_size = xfer->bits_per_word >> 3;
	blocks = xfer->len / block_size;

	desc->cfg_start.b.tx_data_mode = SPICC_DATA_MODE_NONE;
	desc->cfg_start.b.rx_data_mode = SPICC_DATA_MODE_NONE;
	desc->cfg_start.b.eoc = 0;
	desc->cfg_bus.b.keep_ss = !xfer->cs_change;
	desc->cfg_bus.b.null_ctl = 0;

	if (xfer->tx_buf || xfer->tx_dma) {
		desc->cfg_bus.b.lane = nbits_to_lane[xfer->tx_nbits];
		desc->cfg_start.b.op_mode = SPICC_OP_MODE_WRITE;
	}
	if (xfer->rx_buf || xfer->rx_dma) {
		desc->cfg_bus.b.lane = nbits_to_lane[xfer->rx_nbits];
		desc->cfg_start.b.op_mode = SPICC_OP_MODE_READ;
	}

	if (desc->cfg_start.b.op_mode == SPICC_OP_MODE_READ_STS) {
		desc->cfg_start.b.block_size = blocks;
		desc->cfg_start.b.block_num = 1;
	} else {
		desc->cfg_start.b.block_size = block_size & 0x7;
		blocks = min_t(int, blocks, SPICC_BLOCK_MAX);
		desc->cfg_start.b.block_num = blocks;
	}

	if (xfer->tx_sg.nents && xfer->tx_sg.sgl) {
		ccsg_len = xfer->tx_sg.nents * sizeof(struct spicc_sg_link);
		ccsg = kzalloc(ccsg_len, GFP_KERNEL | GFP_DMA);
		if (!ccsg) {
			dev_err(dev, "alloc tx_ccsg failed\n");
			return -ENOMEM;
		}

		aml_spicc_sg_xlate(&xfer->tx_sg, ccsg);
		paddr = dma_map_single(dev, (void *)ccsg,
				       ccsg_len, DMA_TO_DEVICE);
		ret = dma_mapping_error(dev, paddr);
		if (ret) {
			kfree(ccsg);
			dev_err(dev, "tx ccsg map failed\n");
			return ret;
		}

		desc->tx_paddr = paddr;
		desc->cfg_start.b.tx_data_mode = SPICC_DATA_MODE_SG;
		exdesc->tx_ccsg = ccsg;
		exdesc->tx_ccsg_len = ccsg_len;
		dma_sync_sgtable_for_device(spicc->controller->cur_tx_dma_dev,
					    &xfer->tx_sg, DMA_TO_DEVICE);
	} else if (xfer->tx_buf || xfer->tx_dma) {
		paddr = xfer->tx_dma;
		if (!paddr) {
			paddr = dma_map_single(dev, (void *)xfer->tx_buf,
					       xfer->len, DMA_TO_DEVICE);
			ret = dma_mapping_error(dev, paddr);
			if (ret) {
				dev_err(dev, "tx buf map failed\n");
				return ret;
			}
		}
		desc->tx_paddr = paddr;
		desc->cfg_start.b.tx_data_mode = SPICC_DATA_MODE_MEM;
	}

	if (xfer->rx_sg.nents && xfer->rx_sg.sgl) {
		ccsg_len = xfer->rx_sg.nents * sizeof(struct spicc_sg_link);
		ccsg = kzalloc(ccsg_len, GFP_KERNEL | GFP_DMA);
		if (!ccsg) {
			dev_err(dev, "alloc rx_ccsg failed\n");
			return -ENOMEM;
		}

		aml_spicc_sg_xlate(&xfer->rx_sg, ccsg);
		paddr = dma_map_single(dev, (void *)ccsg,
				       ccsg_len, DMA_TO_DEVICE);
		ret = dma_mapping_error(dev, paddr);
		if (ret) {
			kfree(ccsg);
			dev_err(dev, "rx ccsg map failed\n");
			return ret;
		}

		desc->rx_paddr = paddr;
		desc->cfg_start.b.rx_data_mode = SPICC_DATA_MODE_SG;
		exdesc->rx_ccsg = ccsg;
		exdesc->rx_ccsg_len = ccsg_len;
		dma_sync_sgtable_for_device(spicc->controller->cur_rx_dma_dev,
					    &xfer->rx_sg, DMA_FROM_DEVICE);
	} else if (xfer->rx_buf || xfer->rx_dma) {
		paddr = xfer->rx_dma;
		if (!paddr) {
			paddr = dma_map_single(dev, xfer->rx_buf,
					       xfer->len, DMA_FROM_DEVICE);
			ret = dma_mapping_error(dev, paddr);
			if (ret) {
				dev_err(dev, "rx buf map failed\n");
				return ret;
			}
		}

		desc->rx_paddr = paddr;
		desc->cfg_start.b.rx_data_mode = SPICC_DATA_MODE_MEM;
	}

	return 0;
}

static void aml_spicc_cleanup_transfer(struct spicc_device *spicc,
				       struct spi_transfer *xfer,
				       struct spicc_descriptor *desc,
				       struct spicc_descriptor_extra *exdesc)
{
	struct device *dev = &spicc->pdev->dev;

	if (desc->tx_paddr) {
		if (desc->cfg_start.b.tx_data_mode == SPICC_DATA_MODE_SG) {
			dma_unmap_single(dev, (dma_addr_t)desc->tx_paddr,
					 exdesc->tx_ccsg_len, DMA_TO_DEVICE);
			kfree(exdesc->tx_ccsg);
			dma_sync_sgtable_for_cpu(spicc->controller->cur_tx_dma_dev,
						 &xfer->tx_sg, DMA_TO_DEVICE);
		} else if (!xfer->tx_dma) {
			dma_unmap_single(dev, (dma_addr_t)desc->tx_paddr,
					 xfer->len, DMA_TO_DEVICE);
		}
	}

	if (desc->rx_paddr) {
		if (desc->cfg_start.b.rx_data_mode == SPICC_DATA_MODE_SG) {
			dma_unmap_single(dev, (dma_addr_t)desc->rx_paddr,
					 exdesc->rx_ccsg_len, DMA_TO_DEVICE);
			kfree(exdesc->rx_ccsg);
			dma_sync_sgtable_for_cpu(spicc->controller->cur_rx_dma_dev,
						 &xfer->rx_sg, DMA_FROM_DEVICE);
		} else if (!xfer->rx_dma) {
			dma_unmap_single(dev, (dma_addr_t)desc->rx_paddr,
					 xfer->len, DMA_FROM_DEVICE);
		}
	}
}

static void aml_spicc_setup_null_desc(struct spicc_device *spicc,
				      struct spicc_descriptor *desc,
				      u32 n_sclk)
{
	/* unit is the last xfer sclk */
	desc->cfg_start.d32 = spicc->cfg_start.d32;
	desc->cfg_bus.d32 = spicc->cfg_bus.d32;
	desc->cfg_start.b.op_mode = SPICC_OP_MODE_WRITE;
	desc->cfg_start.b.block_size = 1;
	desc->cfg_start.b.block_num = DIV_ROUND_UP(n_sclk, 8);
	desc->cfg_bus.b.null_ctl = 1;
}

static void aml_spicc_pending(struct spicc_device *spicc,
			      dma_addr_t desc_paddr,
			      bool trig,
			      bool irq_en)
{
	u32 desc_l, desc_h, cfg_spi;

#ifdef	CONFIG_ARCH_DMA_ADDR_T_64BIT
	desc_l = (u64)desc_paddr & 0xffffffff;
	desc_h = (u64)desc_paddr >> 32;
#else
	desc_l = desc_paddr & 0xffffffff;
	desc_h = 0;
#endif

	cfg_spi = spicc->cfg_spi.d32;
	if (trig)
		cfg_spi |= HW_POS;
	else
		desc_h |= SPICC_DESC_PENDING;

	spicc_writel(spicc, irq_en ? SPICC_DESC_CHAIN_DONE : 0,
		     SPICC_REG_IRQ_ENABLE);
	spicc_writel(spicc, cfg_spi, SPICC_REG_CFG_SPI);
	spicc_writel(spicc, desc_l, SPICC_REG_DESC_LIST_L);
	spicc_writel(spicc, desc_h, SPICC_REG_DESC_LIST_H);
}

static irqreturn_t aml_spicc_irq(int irq, void *data)
{
	struct spicc_device *spicc = (void *)data;
	u32 sts;

	spicc->status = 0;
	sts = spicc_readl(spicc, SPICC_REG_IRQ_STS);
	spicc_writel(spicc, sts, SPICC_REG_IRQ_STS);
	if (sts & (SPICC_RCH_DESC_INVALID |
		   SPICC_RCH_DESC_RESP |
		   SPICC_RCH_DATA_RESP |
		   SPICC_WCH_DESC_INVALID |
		   SPICC_WCH_DESC_RESP |
		   SPICC_WCH_DATA_RESP |
		   SPICC_DESC_ERR)) {
		spicc->status = sts;
	}

	complete(&spicc->completion);

	return IRQ_HANDLED;
}

static int aml_spicc_transfer_one_message(struct spi_controller *ctlr,
					  struct spi_message *msg)
{
	struct spicc_device *spicc = spi_controller_get_devdata(ctlr);
	struct device *dev = &spicc->pdev->dev;
	unsigned long long ms = 0;
	struct spi_transfer *xfer;
	struct spicc_descriptor *descs, *desc;
	struct spicc_descriptor_extra *exdescs, *exdesc;
	dma_addr_t descs_paddr;
	int desc_num = 1, descs_len;
	u32 cs_hold_in_sclk = 0;
	int ret = -EIO;

	if (!aml_spicc_sem_down_read(spicc)) {
		spi_finalize_current_message(ctlr);
		dev_err(dev, "controller busy\n");
		return -EBUSY;
	}

	/*calculate the desc num for all xfer */
	list_for_each_entry(xfer, &msg->transfers, transfer_list)
		desc_num++;

	/* alloc descriptor/extra-descriptor table */
	descs = kcalloc(desc_num, sizeof(*desc) + sizeof(*exdesc),
			GFP_KERNEL | GFP_DMA);
	if (!descs) {
		spi_finalize_current_message(ctlr);
		aml_spicc_sem_up_write(spicc);
		return -ENOMEM;
	}
	descs_len = sizeof(*desc) * desc_num;
	exdescs = (struct spicc_descriptor_extra *)(descs + desc_num);

	/* config descriptor for each xfer */
	desc = descs;
	exdesc = exdescs;
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		ret = aml_spicc_setup_transfer(spicc, xfer, desc, exdesc);
		if (ret) {
			dev_err(dev, "config descriptor failed\n");
			goto end;
		}

		/* calculate cs-setup delay with the first xfer speed */
		if (list_is_first(&xfer->transfer_list, &msg->transfers))
			desc->cfg_bus.b.cs_setup_in_sclk =
				spi_delay_to_sclk(xfer->effective_speed_hz, &msg->spi->cs_setup);

		/* calculate cs-hold delay with the last xfer speed */
		if (list_is_last(&xfer->transfer_list, &msg->transfers))
			cs_hold_in_sclk =
				spi_delay_to_sclk(xfer->effective_speed_hz, &msg->spi->cs_hold);

		desc++;
		exdesc++;
		ms += DIV_ROUND_UP_ULL(8LL * MSEC_PER_SEC * xfer->len,
				       xfer->effective_speed_hz);
	}

	if (cs_hold_in_sclk)
		/* additional null-descriptor to achieve the cs-hold delay */
		aml_spicc_setup_null_desc(spicc, desc, cs_hold_in_sclk);
	else
		desc--;

	desc->cfg_bus.b.keep_ss = 0;
	desc->cfg_start.b.eoc = 1;

	/* some tolerances */
	ms += ms + 20;
	if (ms > UINT_MAX)
		ms = UINT_MAX;

	descs_paddr = dma_map_single(dev, (void *)descs,
				     descs_len, DMA_TO_DEVICE);
	ret = dma_mapping_error(dev, descs_paddr);
	if (ret) {
		dev_err(dev, "desc table map failed\n");
		goto end;
	}

	reinit_completion(&spicc->completion);
	aml_spicc_pending(spicc, descs_paddr, false, true);
	if (wait_for_completion_timeout(&spicc->completion,
					spi_controller_is_target(spicc->controller) ?
					MAX_SCHEDULE_TIMEOUT : msecs_to_jiffies(ms)))
		ret = spicc->status ? -EIO : 0;
	else
		ret = -ETIMEDOUT;

	dma_unmap_single(dev, descs_paddr, descs_len, DMA_TO_DEVICE);
end:
	desc = descs;
	exdesc = exdescs;
	list_for_each_entry(xfer, &msg->transfers, transfer_list)
		aml_spicc_cleanup_transfer(spicc, xfer, desc++, exdesc++);
	kfree(descs);

	if (!ret)
		msg->actual_length = msg->frame_length;
	msg->status = ret;
	spi_finalize_current_message(ctlr);
	aml_spicc_sem_up_write(spicc);

	return ret;
}

static int aml_spicc_prepare_message(struct spi_controller *ctlr,
				     struct spi_message *message)
{
	struct spicc_device *spicc = spi_controller_get_devdata(ctlr);

	return aml_spicc_setup_device(spicc, message->spi);
}

static int aml_spicc_setup(struct spi_device *spi)
{
	if (!spi->controller_state)
		spi->controller_state = spi_controller_get_devdata(spi->controller);

	return 0;
}

static void aml_spicc_cleanup(struct spi_device *spi)
{
	spi->controller_state = NULL;
}

static int aml_spicc_target_abort(struct spi_controller *ctlr)
{
	struct spicc_device *spicc = spi_controller_get_devdata(ctlr);

	spicc->status = 0;
	spicc_writel(spicc, 0, SPICC_REG_DESC_LIST_H);
	complete(&spicc->completion);

	return 0;
}

#define DIV_NUM (SPICC_CLK_DIV_MAX - SPICC_CLK_DIV_MIN + 1)
static struct clk_div_table linear_div_table[DIV_NUM + 1] = {
	[0] = {0, 0 /* init flag */},
	[DIV_NUM] = {0, 0 /* sentinel */}
};

static int aml_spicc_clk_init(struct spicc_device *spicc)
{
	struct device *dev = &spicc->pdev->dev;
	struct clk_init_data init;
	struct clk_divider *div;
	const char *parent_names[1];
	char name[32];
	u32 pclk_rate = 0;
	u32 val;
	int i;

	spicc->core = devm_clk_get_enabled(dev, "core");
	if (IS_ERR_OR_NULL(spicc->core))
		dev_warn(dev, "core clock request failed\n");

	spicc->pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR_OR_NULL(spicc->pclk)) {
		dev_err(dev, "pclk clock request failed\n");
		return PTR_ERR(spicc->pclk);
	}
	clk_set_min_rate(spicc->pclk, SPICC_PCLK_RATE_MIN);

	if (!linear_div_table[0].div)
		for (i = 0; i < DIV_NUM; i++) {
			linear_div_table[i].val = i + SPICC_CLK_DIV_MIN - 1;
			linear_div_table[i].div = i + SPICC_CLK_DIV_MIN;
		}

	div = devm_kzalloc(dev, sizeof(*div), GFP_KERNEL);
	if (!div)
		return -ENOMEM;

	div->flags = CLK_DIVIDER_ROUND_CLOSEST;
	div->reg = spicc->base + SPICC_REG_CFG_BUS;
	div->shift = SPICC_CLK_DIV_SHIFT;
	div->width = SPICC_CLK_DIV_WIDTH;
	div->table = linear_div_table;

	/* Register value should not be outside of the table */
	val = spicc_readl(spicc, SPICC_REG_CFG_BUS);
	val &= ~SPICC_CLK_DIV_MASK;
	val |= FIELD_PREP(SPICC_CLK_DIV_MASK, SPICC_CLK_DIV_MIN - 1);
	spicc_writel(spicc, val, SPICC_REG_CFG_BUS);

	/* Register clk-divider */
	parent_names[0] = __clk_get_name(spicc->pclk);
	snprintf(name, sizeof(name), "%s_div", dev_name(dev));
	init.name = name;
	init.ops = &clk_divider_ops;
	init.flags = CLK_SET_RATE_PARENT;
	/*
	 * For multiple transmission of different speeds in one message, the
	 * parent clock rate needs to be fixed by dts configuration.
	 */
	of_property_read_u32(dev->of_node, "fixed-pclk-rate", &pclk_rate);
	if (pclk_rate) {
		clk_set_rate(spicc->pclk, pclk_rate);
		init.flags = 0;
	}
	init.parent_names = parent_names;
	init.num_parents = 1;
	div->hw.init = &init;

	spicc->sclk = devm_clk_register(dev, &div->hw);
	if (IS_ERR_OR_NULL(spicc->sclk)) {
		dev_err(dev, "clock registration failed\n");
		return PTR_ERR(spicc->sclk);
	}

	return 0;
}

static int aml_spicc_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct spicc_device *spicc;
	int ret, irq;

	ctlr = __spi_alloc_controller(&pdev->dev, sizeof(*spicc),
				      of_property_read_bool(pdev->dev.of_node, "slave"));
	if (!ctlr) {
		dev_err(&pdev->dev, "controller allocation failed\n");
		return -ENOMEM;
	}
	spicc = spi_controller_get_devdata(ctlr);
	spicc->controller = ctlr;

	spicc->pdev = pdev;
	platform_set_drvdata(pdev, spicc);

	spicc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR_OR_NULL(spicc->base)) {
		dev_err(&pdev->dev, "io resource mapping failed\n");
		ret = PTR_ERR(spicc->base);
		goto out_controller;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto out_controller;
	}

	ret = devm_request_irq(&pdev->dev, irq, aml_spicc_irq,
			       IRQF_TRIGGER_RISING, NULL, spicc);
	if (ret) {
		dev_err(&pdev->dev, "irq request failed\n");
		goto out_controller;
	}

	ret = aml_spicc_clk_init(spicc);
	if (ret)
		goto out_controller;

	spicc->cfg_spi.d32 = 0;
	spicc->cfg_start.d32 = 0;
	spicc->cfg_bus.d32 = 0;

	spicc->cfg_spi.b.flash_wp_pin_en = 1;
	spicc->cfg_spi.b.flash_hold_pin_en = 1;
	if (spi_controller_is_target(ctlr)) {
		spicc->cfg_spi.b.slave_en = true;
		spicc->cfg_bus.b.tx_tuning_in_pclk = -1;
	}
	/* default pending */
	spicc->cfg_start.b.pending = 1;

	device_reset_optional(&pdev->dev);
	ctlr->num_chipselect = 4;
	ctlr->dev.of_node = pdev->dev.of_node;
	ctlr->mode_bits = SPI_CPHA | SPI_CPOL | SPI_LSB_FIRST |
			  SPI_3WIRE | SPI_TX_QUAD | SPI_RX_QUAD;
	ctlr->max_speed_hz = 1000 * 1000 * 100;
	ctlr->min_speed_hz = 1000 * 10;
	ctlr->setup = aml_spicc_setup;
	ctlr->cleanup = aml_spicc_cleanup;
	ctlr->prepare_message = aml_spicc_prepare_message;
	ctlr->transfer_one_message = aml_spicc_transfer_one_message;
	ctlr->target_abort = aml_spicc_target_abort;
	ctlr->can_dma = aml_spicc_can_dma;
	ctlr->max_dma_len = SPICC_BLOCK_MAX;
	dma_set_max_seg_size(&pdev->dev, SPICC_BLOCK_MAX);
	ret = devm_spi_register_controller(&pdev->dev, ctlr);
	if (ret) {
		dev_err(&pdev->dev, "spi controller registration failed\n");
		goto out_clk;
	}

	init_completion(&spicc->completion);

	return 0;

out_clk:
	if (spicc->core)
		clk_disable_unprepare(spicc->core);
	clk_disable_unprepare(spicc->pclk);
out_controller:
	spi_controller_put(ctlr);

	return ret;
}

static void aml_spicc_remove(struct platform_device *pdev)
{
	struct spicc_device *spicc = platform_get_drvdata(pdev);

	if (spicc->core)
		clk_disable_unprepare(spicc->core);
	clk_disable_unprepare(spicc->pclk);
}

static int aml_spicc_off(struct spicc_device *spicc)
{
	pinctrl_pm_select_sleep_state(&spicc->pdev->dev);
	clk_disable_unprepare(spicc->pclk);
	if (spicc->core)
		clk_disable_unprepare(spicc->core);

	return 0;
}

static int aml_spicc_on(struct spicc_device *spicc)
{
	if (spicc->core)
		clk_prepare_enable(spicc->core);
	clk_prepare_enable(spicc->pclk);
	pinctrl_pm_select_default_state(&spicc->pdev->dev);

	return 0;
}

#ifdef CONFIG_HIBERNATION
static int aml_spicc_freeze(struct device *dev)
{
	struct spicc_device *spicc = dev_get_drvdata(dev);

	return aml_spicc_off(spicc);
}

static int aml_spicc_thaw(struct device *dev)
{
	struct spicc_device *spicc = dev_get_drvdata(dev);

	return aml_spicc_on(spicc);
}

static int aml_spicc_restore(struct device *dev)
{
	struct spicc_device *spicc = dev_get_drvdata(dev);

	return aml_spicc_on(spicc);
}
#endif

static void aml_spicc_shutdown(struct platform_device *pdev)
{
	struct spicc_device *spicc = platform_get_drvdata(pdev);

	aml_spicc_off(spicc);
}

static int aml_spicc_suspend(struct device *dev)
{
	struct spicc_device *spicc = dev_get_drvdata(dev);
	int ret;

	ret = spi_controller_suspend(spicc->controller);
	if (!ret)
		ret = aml_spicc_off(spicc);

	return ret;
}

static int aml_spicc_resume(struct device *dev)
{
	struct spicc_device *spicc = dev_get_drvdata(dev);
	int ret;

	ret = aml_spicc_on(spicc);
	if (!ret)
		ret = spi_controller_resume(spicc->controller);

	return ret;
}

static const struct dev_pm_ops amlogic_spicc_pm_ops = {
	.suspend	= aml_spicc_suspend,
	.resume		= aml_spicc_resume,
#ifdef CONFIG_HIBERNATION
	.freeze		= aml_spicc_freeze,
	.thaw		= aml_spicc_thaw,
	.restore	= aml_spicc_restore,
#endif
};

static const struct of_device_id amlogic_spisg_of_match[] = {
	{
		.compatible = "amlogic,a4-spisg",
	},

	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, amlogic_spisg_of_match);

struct platform_driver amlogic_spisg_driver = {
	.probe = aml_spicc_probe,
	.remove = aml_spicc_remove,
	.shutdown = aml_spicc_shutdown,
	.driver  = {
		.name = "amlogic-spisg",
		.of_match_table = of_match_ptr(amlogic_spisg_of_match),
		.pm = &amlogic_spicc_pm_ops,
	},
};

module_platform_driver(amlogic_spisg_driver);

MODULE_DESCRIPTION("Amlogic SPI Scatter-Gather Controller driver");
MODULE_AUTHOR("Sunny Luo <sunny.luo@amlogic.com>");
MODULE_LICENSE("GPL");
