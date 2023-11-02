// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright(c) 2021 Shanghai Zhaoxin Semiconductor Corporation.
 *                    All rights reserved.
 */

#include <linux/pci.h>
#include "i2c-viai2c-common.h"

#define ZX_I2C_NAME             "Zhaoxin-I2C"

/*
 * registers
 */
/* Zhaoxin specific register bit fields */
/* REG_CR Bit fields */
#define   ZXI2C_CR_MST_RST		BIT(7)
#define   ZXI2C_CR_FIFO_MODE		BIT(14)
/* REG_ISR/IMR Bit fields */
#define   ZXI2C_IRQ_FIFONACK		BIT(4)
#define   ZXI2C_IRQ_FIFOEND		BIT(3)
#define   ZXI2C_IRQ_MASK		(VIAI2C_ISR_MASK_ALL \
					| ZXI2C_IRQ_FIFOEND \
					| ZXI2C_IRQ_FIFONACK)
/* Zhaoxin specific registers */
#define ZXI2C_REG_CLK		0x10
#define   ZXI2C_CLK_50M			BIT(0)
#define ZXI2C_REG_REV		0x11
#define ZXI2C_REG_HCR		0x12
#define   ZXI2C_HCR_RST_FIFO		GENMASK(1, 0)
#define ZXI2C_REG_HTDR		0x13
#define ZXI2C_REG_HRDR		0x14
#define ZXI2C_REG_HTLR		0x15
#define ZXI2C_REG_HRLR		0x16
#define ZXI2C_REG_HWCNTR	0x18
#define ZXI2C_REG_HRCNTR	0x19

/* parameters Constants */
#define ZXI2C_GOLD_FSTP_100K	0xF3
#define ZXI2C_GOLD_FSTP_400K	0x38
#define ZXI2C_GOLD_FSTP_1M	0x13
#define ZXI2C_GOLD_FSTP_3400K	0x37
#define ZXI2C_HS_MASTER_CODE	(0x08 << 8)
#define ZXI2C_FIFO_SIZE		32

/* Structure definition */
#define zxi2c viai2c

static int zxi2c_fifo_xfer(struct zxi2c *i2c, struct i2c_msg *msg)
{
	u16 xfered_len = 0;
	u16 byte_left = msg->len;
	u16 tcr_val = i2c->tcr;
	void __iomem *base = i2c->base;
	bool read = !!(msg->flags & I2C_M_RD);

	while (byte_left) {
		u16 i;
		u8 tmp;
		int error;
		u16 xfer_len = min_t(u16, byte_left, ZXI2C_FIFO_SIZE);

		byte_left -= xfer_len;

		/* reset fifo buffer */
		tmp = ioread8(base + ZXI2C_REG_HCR);
		iowrite8(tmp | ZXI2C_HCR_RST_FIFO, base + ZXI2C_REG_HCR);

		/* set xfer len */
		if (read) {
			iowrite8(xfer_len - 1, base + ZXI2C_REG_HRLR);
		} else {
			iowrite8(xfer_len - 1, base + ZXI2C_REG_HTLR);
			/* set write data */
			for (i = 0; i < xfer_len; i++)
				iowrite8(msg->buf[xfered_len + i],
						base + ZXI2C_REG_HTDR);
		}

		/* prepare to stop transmission */
		if (i2c->hrv && !byte_left) {
			tmp = ioread8(i2c->base + VIAI2C_REG_CR);
			tmp |= read ? VIAI2C_CR_RX_END : VIAI2C_CR_TX_END;
			iowrite8(tmp, base + VIAI2C_REG_CR);
		}

		reinit_completion(&i2c->complete);

		if (xfered_len) {
			/* continue transmission */
			tmp = ioread8(i2c->base + VIAI2C_REG_CR);
			iowrite8(tmp |= VIAI2C_CR_CPU_RDY,
					i2c->base + VIAI2C_REG_CR);
		} else {
			/* start transmission */
			tcr_val |= (read ? VIAI2C_TCR_MASTER_READ : 0);
			writew(tcr_val | msg->addr, base + VIAI2C_REG_TCR);
		}

		error = viai2c_wait_status(i2c, ZXI2C_IRQ_FIFOEND);
		if (error)
			return error;

		/* get the received data */
		if (read)
			for (i = 0; i < xfer_len; i++)
				msg->buf[xfered_len + i] =
					ioread8(base + ZXI2C_REG_HRDR);

		xfered_len += xfer_len;
	}

	return 1;
}

static int zxi2c_master_xfer(struct i2c_adapter *adap,
			struct i2c_msg *msgs, int num)
{
	u8 tmp;
	int ret;
	struct zxi2c *i2c = (struct zxi2c *)i2c_get_adapdata(adap);

	ret = viai2c_wait_bus_ready(i2c);
	if (ret)
		return ret;

	tmp = ioread8(i2c->base + VIAI2C_REG_CR);
	tmp &= ~(VIAI2C_CR_RX_END | VIAI2C_CR_TX_END);

	if (num == 1 && msgs->len >= 2 &&
	   (i2c->hrv || msgs->len <= ZXI2C_FIFO_SIZE)) {
		/* enable fifo mode */
		iowrite16(ZXI2C_CR_FIFO_MODE | tmp, i2c->base + VIAI2C_REG_CR);
		/* clear irq status */
		iowrite8(ZXI2C_IRQ_MASK, i2c->base + VIAI2C_REG_ISR);
		/* enable fifo irq */
		iowrite8(VIAI2C_ISR_NACK_ADDR | ZXI2C_IRQ_FIFOEND,
				i2c->base + VIAI2C_REG_IMR);

		ret = zxi2c_fifo_xfer(i2c, msgs);
	} else {
		/* enable byte mode */
		iowrite16(tmp, i2c->base + VIAI2C_REG_CR);
		/* clear irq status */
		iowrite8(ZXI2C_IRQ_MASK, i2c->base + VIAI2C_REG_ISR);
		/* enable byte irq */
		iowrite8(VIAI2C_ISR_NACK_ADDR | VIAI2C_IMR_BYTE,
				i2c->base + VIAI2C_REG_IMR);

		ret = viai2c_xfer(adap, msgs, num);
		if (ret < 0)
			iowrite16(tmp | VIAI2C_CR_END_MASK,
					i2c->base + VIAI2C_REG_CR);
		/* make sure the state machine is stopped */
		usleep_range(1, 2);
	}
	/* dis interrupt */
	iowrite8(0, i2c->base + VIAI2C_REG_IMR);

	return ret;
}

static u32 zxi2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm zxi2c_algorithm = {
	.master_xfer	= zxi2c_master_xfer,
	.functionality	= zxi2c_func,
};

static const struct i2c_adapter_quirks zxi2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN | I2C_AQ_COMB_WRITE_THEN_READ,
};

static const u32 zxi2c_speed_params_table[][3] = {
	/* speed, ZXI2C_TCR, ZXI2C_FSTP */
	{ I2C_MAX_STANDARD_MODE_FREQ, 0, ZXI2C_GOLD_FSTP_100K },
	{ I2C_MAX_FAST_MODE_FREQ, VIAI2C_TCR_FAST, ZXI2C_GOLD_FSTP_400K },
	{ I2C_MAX_FAST_MODE_PLUS_FREQ, VIAI2C_TCR_FAST, ZXI2C_GOLD_FSTP_1M },
	{ I2C_MAX_HIGH_SPEED_MODE_FREQ, VIAI2C_TCR_HS_MODE | VIAI2C_TCR_FAST,
	  ZXI2C_GOLD_FSTP_3400K },
};

static void zxi2c_set_bus_speed(struct zxi2c *i2c)
{
	iowrite16(i2c->tr, i2c->base + VIAI2C_REG_TR);
	iowrite8(ZXI2C_CLK_50M, i2c->base + ZXI2C_REG_CLK);
	iowrite16(i2c->mcr, i2c->base + VIAI2C_REG_MCR);
}

static void zxi2c_get_bus_speed(struct zxi2c *i2c)
{
	u8 i, count;
	u8 fstp;
	const u32 *params;
	u32 acpi_speed = i2c_acpi_find_bus_speed(i2c->dev);

	count = ARRAY_SIZE(zxi2c_speed_params_table);
	for (i = 0; i < count; i++)
		if (acpi_speed == zxi2c_speed_params_table[i][0])
			break;
	/* if not found, use 400k as default */
	i = i < count ? i : 1;

	params = zxi2c_speed_params_table[i];
	fstp = ioread8(i2c->base + VIAI2C_REG_TR);
	if (abs(fstp - params[2]) > 0x10) {
		/*
		 * if BIOS setting value far from golden value,
		 * use golden value and warn user
		 */
		dev_warn(i2c->dev, "speed:%d, fstp:0x%x, golden:0x%x\n",
				params[0], fstp, params[2]);
		i2c->tr = params[2] | 0xff00;
	} else {
		i2c->tr = fstp | 0xff00;
	}

	i2c->tcr = params[1];
	i2c->mcr = ioread16(i2c->base + VIAI2C_REG_MCR);
	/* for Hs-mode, use 0000 1000 as master code */
	if (params[0] == I2C_MAX_HIGH_SPEED_MODE_FREQ)
		i2c->mcr |= ZXI2C_HS_MASTER_CODE;

	dev_info(i2c->dev, "speed mode is %s\n",
			i2c_freq_mode_string(params[0]));
}

static int zxi2c_probe(struct platform_device *pdev)
{
	int error;
	struct zxi2c *i2c;
	struct pci_dev *pci;
	struct i2c_adapter *adap;

	error = viai2c_init(pdev, &i2c);
	if (error)
		return error;

	zxi2c_get_bus_speed(i2c);
	zxi2c_set_bus_speed(i2c);

	i2c->platform = VIAI2C_PLAT_ZHAOXIN;
	i2c->hrv = ioread8(i2c->base + ZXI2C_REG_REV);

	adap = &i2c->adapter;
	adap->owner = THIS_MODULE;
	adap->algo = &zxi2c_algorithm;
	adap->retries = 2;
	adap->quirks = &zxi2c_quirks;
	adap->dev.parent = &pdev->dev;
	ACPI_COMPANION_SET(&adap->dev, ACPI_COMPANION(&pdev->dev));
	pci = to_pci_dev(pdev->dev.parent);
	snprintf(adap->name, sizeof(adap->name), "%s-%s-%s",
		ZX_I2C_NAME, dev_name(&pci->dev), dev_name(i2c->dev));
	i2c_set_adapdata(adap, i2c);

	return devm_i2c_add_adapter(&pdev->dev, adap);
}

static int zxi2c_resume(struct device *dev)
{
	struct zxi2c *i2c = dev_get_drvdata(dev);

	iowrite8(ZXI2C_CR_MST_RST, i2c->base + VIAI2C_REG_CR);
	zxi2c_set_bus_speed(i2c);

	return 0;
}

static const struct dev_pm_ops zxi2c_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(NULL, zxi2c_resume)
};

static const struct acpi_device_id zxi2c_acpi_match[] = {
	{"IIC1D17", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, zxi2c_acpi_match);

static struct platform_driver zxi2c_driver = {
	.probe = zxi2c_probe,
	.driver = {
		.name = ZX_I2C_NAME,
		.acpi_match_table = ACPI_PTR(zxi2c_acpi_match),
		.pm = &zxi2c_pm,
	},
};

module_platform_driver(zxi2c_driver);

MODULE_AUTHOR("HansHu@zhaoxin.com");
MODULE_DESCRIPTION("Shanghai Zhaoxin IIC driver");
MODULE_LICENSE("GPL");
