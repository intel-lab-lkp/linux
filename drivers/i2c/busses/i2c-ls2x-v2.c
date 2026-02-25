// SPDX-License-Identifier: GPL-2.0-only
/*
 * Loongson-2K0300 I2C controller driver
 *
 * Copyright (C) 2025-2026 Loongson Technology Corporation Limited
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/units.h>

/* Loongson-2 fast I2C offset registers */
#define LOONGSON2_I2C_CR1	0x00	/* I2C control 1 register */
#define LOONGSON2_I2C_CR2	0x04	/* I2C control 2 register */
#define LOONGSON2_I2C_OAR	0x08	/* I2C slave address register */
#define LOONGSON2_I2C_DR	0x10	/* I2C data register */
#define LOONGSON2_I2C_SR1	0x14	/* I2C status 1 register */
#define LOONGSON2_I2C_SR2	0x18	/* I2C status 2 register */
#define LOONGSON2_I2C_CCR	0x1c	/* I2C clock control register */
#define LOONGSON2_I2C_TRISE	0x20	/* I2C trise register */
#define LOONGSON2_I2C_FLTR	0x24

/* Bitfields of I2C control 1 register */
#define LOONGSON2_I2C_CR1_PE		BIT(0)
#define LOONGSON2_I2C_CR1_START		BIT(8)
#define LOONGSON2_I2C_CR1_STOP		BIT(9)
#define LOONGSON2_I2C_CR1_ACK		BIT(10)
#define LOONGSON2_I2C_CR1_POS		BIT(11)

#define LOONGSON2_I2C_CR1_OP_MASK	(LOONGSON2_I2C_CR1_START | LOONGSON2_I2C_CR1_STOP)

/* Bitfields of I2C control 2 register */
#define LOONGSON2_I2C_CR2_FREQ		GENMASK(5, 0)	/* APB Clock Frequency in MHz */
#define LOONGSON2_I2C_CR2_ITERREN	BIT(8)	/* Fault-Class Interrupt Enable */
#define LOONGSON2_I2C_CR2_ITEVTEN	BIT(9)	/* Event-Based Interrupt Enable */
#define LOONGSON2_I2C_CR2_ITBUFEN	BIT(10)	/* Cache-Class Interrupt Enable */

#define LOONGSON2_I2C_CR2_INT_MASK	\
	(LOONGSON2_I2C_CR2_ITBUFEN | LOONGSON2_I2C_CR2_ITEVTEN | LOONGSON2_I2C_CR2_ITERREN)

/* Bitfields of I2C status 1 register */
#define LOONGSON2_I2C_SR1_SB		BIT(0)
#define LOONGSON2_I2C_SR1_ADDR		BIT(1)
#define LOONGSON2_I2C_SR1_BTF		BIT(2)
#define LOONGSON2_I2C_SR1_RXNE		BIT(6)
#define LOONGSON2_I2C_SR1_TXE		BIT(7)
#define LOONGSON2_I2C_SR1_BERR		BIT(8)
#define LOONGSON2_I2C_SR1_ARLO		BIT(9)
#define LOONGSON2_I2C_SR1_AF		BIT(10)

#define LOONGSON2_I2C_SR1_ITEVTEN_MASK	\
	(LOONGSON2_I2C_SR1_BTF | LOONGSON2_I2C_SR1_ADDR | LOONGSON2_I2C_SR1_SB)
#define LOONGSON2_I2C_SR1_ITBUFEN_MASK	(LOONGSON2_I2C_SR1_TXE | LOONGSON2_I2C_SR1_RXNE)
#define LOONGSON2_I2C_SR1_ITERREN_MASK	\
	(LOONGSON2_I2C_SR1_AF | LOONGSON2_I2C_SR1_ARLO | LOONGSON2_I2C_SR1_BERR)

/* Bitfields of I2C status 2 register */
#define LOONGSON2_I2C_SR2_BUSY		BIT(1)

/* Bitfields of I2C clock control register */
#define LOONGSON2_I2C_CCR_CCR		GENMASK(11, 0)
#define LOONGSON2_I2C_CCR_DUTY		BIT(14)
#define LOONGSON2_I2C_CCR_FS		BIT(15)

/* Bitfields of I2C trise register */
#define LOONGSON2_I2C_TRISE_SCL		GENMASK(5, 0)

#define LOONGSON2_I2C_FREE_SLEEP_US	(1 * USEC_PER_MSEC)
#define LOONGSON2_I2C_FREE_TIMEOUT_US	(5 * USEC_PER_MSEC)

/**
 * struct loongson2_i2c_msg - client specific data
 * @buf: data buffer
 * @count: number of bytes to be transferred
 * @result: result of the transfer
 * @addr: 8-bit slave addr, including r/w bit
 * @stop: last I2C msg to be sent, i.e. STOP to be generated
 */
struct loongson2_i2c_msg {
	u8	*buf;
	u32	count;
	int	result;
	u8      addr;
	bool	stop;
};

/**
 * struct loongson2_i2c_priv - private data of the controller
 * @adapter: I2C adapter for this controller
 * @clk: hw i2c clock
 * @complete: completion of I2C message
 * @regmap: regmap of the I2C device
 * @speed: I2C clock frequency of the controller. Standard or Fast are supported
 * @parent_rate: I2C clock parent rate in MHz
 * @msg: I2C transfer information
 */
struct loongson2_i2c_priv {
	struct i2c_adapter		adapter;
	struct completion		complete;
	struct clk			*clk;
	struct regmap			*regmap;
	int				speed;
	int				parent_rate;
	struct loongson2_i2c_msg	msg;
};

static void loongson2_i2c_disable_irq(struct loongson2_i2c_priv *priv)
{
	regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR2, LOONGSON2_I2C_CR2_INT_MASK, 0);
}

static void loongson2_i2c_read_msg(struct loongson2_i2c_priv *priv)
{
	struct loongson2_i2c_msg *msg = &priv->msg;
	u32 rbuf;

	regmap_read(priv->regmap, LOONGSON2_I2C_DR, &rbuf);
	*msg->buf++ = rbuf;
	msg->count--;
}

static void loongson2_i2c_write_msg(struct loongson2_i2c_priv *priv, u8 byte)
{
	regmap_write(priv->regmap, LOONGSON2_I2C_DR, byte);
}

static void loongson2_i2c_terminate_xfer(struct loongson2_i2c_priv *priv)
{
	struct loongson2_i2c_msg *msg = &priv->msg;

	loongson2_i2c_disable_irq(priv);
	regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_OP_MASK,
			   msg->stop ? LOONGSON2_I2C_CR1_STOP : LOONGSON2_I2C_CR1_START);
	complete(&priv->complete);
}

static void loongson2_i2c_handle_read(struct loongson2_i2c_priv *priv, int flag)
{
	struct loongson2_i2c_msg *msg = &priv->msg;
	unsigned int i;
	bool changed;

	switch (msg->count) {
	case 1:
		/* only transmit 1 bytes condition */
		loongson2_i2c_disable_irq(priv);
		loongson2_i2c_read_msg(priv);
		complete(&priv->complete);
		break;
	case 2:
		if (flag != 1) {
			/* ensure only transmit 2 bytes condition */
			regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR2,
					   LOONGSON2_I2C_CR2_ITBUFEN, 0);
			break;
		}
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_OP_MASK,
				   msg->stop ? LOONGSON2_I2C_CR1_STOP : LOONGSON2_I2C_CR1_START);

		loongson2_i2c_disable_irq(priv);

		for (i = msg->count; i > 0; i--)
			loongson2_i2c_read_msg(priv);

		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_POS, 0);
		complete(&priv->complete);
		break;
	case 3:
		regmap_update_bits_check(priv->regmap, LOONGSON2_I2C_CR2, LOONGSON2_I2C_CR2_ITBUFEN,
					 0, &changed);
		if (changed)
			break;
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_ACK, 0);
		fallthrough;
	default:
		loongson2_i2c_read_msg(priv);
	}
}

static void loongson2_i2c_handle_write(struct loongson2_i2c_priv *priv)
{
	struct loongson2_i2c_msg *msg = &priv->msg;

	if (msg->count) {
		loongson2_i2c_write_msg(priv, *msg->buf++);
		msg->count--;
		if (!msg->count)
			regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR2,
					   LOONGSON2_I2C_CR2_ITBUFEN, 0);
	} else {
		loongson2_i2c_terminate_xfer(priv);
	}
}

static void loongson2_i2c_handle_rx_addr(struct loongson2_i2c_priv *priv)
{
	struct loongson2_i2c_msg *msg = &priv->msg;

	switch (msg->count) {
	case 0:
		loongson2_i2c_terminate_xfer(priv);
		break;
	case 1:
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1,
				   LOONGSON2_I2C_CR1_ACK | LOONGSON2_I2C_CR1_POS, 0);
		/* start or stop */
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_OP_MASK,
				   msg->stop ? LOONGSON2_I2C_CR1_STOP : LOONGSON2_I2C_CR1_START);
		break;
	case 2:
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_ACK, 0);
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_POS,
				   LOONGSON2_I2C_CR1_POS);
		break;

	default:
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_ACK,
				   LOONGSON2_I2C_CR1_ACK);
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_POS, 0);
	}
}

static void loongson2_i2c_isr_error(u32 status, void *data)
{
	struct loongson2_i2c_priv *priv = data;
	struct loongson2_i2c_msg *msg = &priv->msg;

	/* Arbitration lost */
	if (status & LOONGSON2_I2C_SR1_ARLO) {
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_SR1, LOONGSON2_I2C_SR1_ARLO, 0);
		msg->result = -EAGAIN;
		goto out;
	}

	/*
	 * Acknowledge failure:
	 * In master transmitter mode a Stop must be generated by software
	 */
	if (status & LOONGSON2_I2C_SR1_AF) {
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_STOP,
				   LOONGSON2_I2C_CR1_STOP);
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_SR1, LOONGSON2_I2C_SR1_AF, 0);
		msg->result = -EIO;
		goto out;
	}

	/* Bus error */
	if (status & LOONGSON2_I2C_SR1_BERR) {
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_SR1, LOONGSON2_I2C_SR1_BERR, 0);
		msg->result = -EIO;
		goto out;
	}

out:
	loongson2_i2c_disable_irq(priv);
	complete(&priv->complete);
}

static irqreturn_t loongson2_i2c_isr_event(int irq, void *data)
{
	struct loongson2_i2c_priv *priv = data;
	struct device *dev = priv->adapter.dev.parent;
	struct loongson2_i2c_msg *msg = &priv->msg;
	u32 status, ien, event, cr2, possible_status;

	regmap_read(priv->regmap, LOONGSON2_I2C_SR1, &status);
	if (status & LOONGSON2_I2C_SR1_ITERREN_MASK) {
		loongson2_i2c_isr_error(status, data);
		return IRQ_NONE;
	}

	regmap_read(priv->regmap, LOONGSON2_I2C_CR2, &cr2);
	ien = cr2 & LOONGSON2_I2C_CR2_INT_MASK;

	/* Update possible_status if buffer interrupt is enabled */
	possible_status = LOONGSON2_I2C_SR1_ITEVTEN_MASK;
	if (ien & LOONGSON2_I2C_CR2_ITBUFEN)
		possible_status |= LOONGSON2_I2C_SR1_ITBUFEN_MASK;

	event = status & possible_status;
	if (!event) {
		dev_dbg(dev, "spurious evt irq (status=0x%08x, ien=0x%08x)\n", status, ien);
		return IRQ_NONE;
	}

	/* Start condition generated */
	if (event & LOONGSON2_I2C_SR1_SB)
		loongson2_i2c_write_msg(priv, msg->addr);

	/* I2C Address sent */
	if (event & LOONGSON2_I2C_SR1_ADDR) {
		if (msg->addr & I2C_M_RD)
			loongson2_i2c_handle_rx_addr(priv);
		/* Clear ADDR flag */
		regmap_read(priv->regmap, LOONGSON2_I2C_SR2, &status);
		/* Enable buffer interrupts for RX/TX not empty events */
		regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR2, LOONGSON2_I2C_CR2_ITBUFEN,
				   LOONGSON2_I2C_CR2_ITBUFEN);
	}

	if (msg->addr & I2C_M_RD) {
		/* RX not empty */
		if (event & LOONGSON2_I2C_SR1_RXNE)
			loongson2_i2c_handle_read(priv, 0);

		if (event & LOONGSON2_I2C_SR1_BTF)
			loongson2_i2c_handle_read(priv, 1);
	} else {
		/* TX empty */
		if (event & LOONGSON2_I2C_SR1_TXE)
			loongson2_i2c_handle_write(priv);

		if (event & LOONGSON2_I2C_SR1_BTF)
			loongson2_i2c_handle_write(priv);
	}

	return IRQ_HANDLED;
}

static int loongson2_i2c_xfer_msg(struct loongson2_i2c_priv *priv, struct i2c_msg *msg,
				  bool is_stop)
{
	struct loongson2_i2c_msg *l_msg = &priv->msg;
	unsigned long timeout;

	l_msg->addr   = i2c_8bit_addr_from_msg(msg);
	l_msg->buf    = msg->buf;
	l_msg->count  = msg->len;
	l_msg->stop   = is_stop;
	l_msg->result = 0;

	reinit_completion(&priv->complete);

	/* Enable events and errors interrupts */
	regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR2,
			   LOONGSON2_I2C_CR2_ITEVTEN | LOONGSON2_I2C_CR2_ITERREN,
			   LOONGSON2_I2C_CR2_ITEVTEN | LOONGSON2_I2C_CR2_ITERREN);

	timeout = wait_for_completion_timeout(&priv->complete, priv->adapter.timeout);
	if (!timeout)
		return -ETIMEDOUT;

	return l_msg->result;
}

static int loongson2_i2c_xfer(struct i2c_adapter *i2c_adap, struct i2c_msg msgs[], int num)
{
	struct loongson2_i2c_priv *priv = i2c_get_adapdata(i2c_adap);
	struct device *dev = priv->adapter.dev.parent;
	unsigned int i, status;
	int ret;

	/* Wait I2C bus free */
	ret = regmap_read_poll_timeout(priv->regmap, LOONGSON2_I2C_SR2, status,
				       !(status & LOONGSON2_I2C_SR2_BUSY),
				       LOONGSON2_I2C_FREE_SLEEP_US,
				       LOONGSON2_I2C_FREE_TIMEOUT_US);
	if (ret) {
		dev_dbg(dev, "The I2C bus is busy now.\n");
		return ret;
	}

	/* Start generation */
	regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_START,
			   LOONGSON2_I2C_CR1_START);

	for (i = 0; i < num; i++) {
		ret = loongson2_i2c_xfer_msg(priv, &msgs[i], i == num - 1);
		if (ret < 0)
			return ret;
	}

	return num;
}

static u32 loongson2_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm loongson2_i2c_algo = {
	.xfer		= loongson2_i2c_xfer,
	.functionality	= loongson2_i2c_func,
};

static int loongson2_i2c_adjust_bus_speed(struct loongson2_i2c_priv *priv)
{
	u32 val, freq_mhz, ccr;

	priv->parent_rate = clk_get_rate(priv->clk);
	freq_mhz = DIV_ROUND_UP(priv->parent_rate, HZ_PER_MHZ);

	if (priv->speed == I2C_MAX_STANDARD_MODE_FREQ) {
		 /* Select Standard mode */
		ccr = 0;
		val = DIV_ROUND_UP(priv->parent_rate, I2C_MAX_STANDARD_MODE_FREQ * 2);
	} else {
		/* Select Fast mode */
		ccr = LOONGSON2_I2C_CCR_FS;
		val = DIV_ROUND_UP(priv->parent_rate, I2C_MAX_FAST_MODE_FREQ * 3);
	}

	FIELD_MODIFY(LOONGSON2_I2C_CCR_CCR, &ccr, val);
	regmap_write(priv->regmap, LOONGSON2_I2C_CCR, ccr);

	regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR2, LOONGSON2_I2C_CR2_FREQ,
			   FIELD_GET(LOONGSON2_I2C_CR2_FREQ, freq_mhz));
	regmap_update_bits(priv->regmap, LOONGSON2_I2C_TRISE, LOONGSON2_I2C_TRISE_SCL,
			   LOONGSON2_I2C_TRISE_SCL);

	/* Enable I2C */
	regmap_update_bits(priv->regmap, LOONGSON2_I2C_CR1, LOONGSON2_I2C_CR1_PE,
			   LOONGSON2_I2C_CR1_PE);

	return 0;
}

static const struct regmap_config loongson2_i2c_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = LOONGSON2_I2C_TRISE,
};

static int loongson2_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct loongson2_i2c_priv *priv;
	struct i2c_adapter *adap;
	void __iomem *base;
	u32 clk_rate;
	int irq, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(dev, base, &loongson2_i2c_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap), "Failed to init regmap.\n");

	priv->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk), "Failed to enable clock.\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	adap = &priv->adapter;
	adap->retries = 5;
	adap->nr = pdev->id;
	adap->dev.parent = dev;
	adap->owner = THIS_MODULE;
	adap->algo = &loongson2_i2c_algo;
	adap->timeout = 2 * HZ;
	device_set_node(&adap->dev, dev_fwnode(dev));
	i2c_set_adapdata(adap, priv);
	strscpy(adap->name, pdev->name);
	init_completion(&priv->complete);
	platform_set_drvdata(pdev, priv);

	priv->speed = I2C_MAX_STANDARD_MODE_FREQ;
	ret = device_property_read_u32(dev, "clock-frequency", &clk_rate);
	if (!ret && clk_rate >= I2C_MAX_FAST_MODE_FREQ)
		priv->speed = I2C_MAX_FAST_MODE_FREQ;

	ret = loongson2_i2c_adjust_bus_speed(priv);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, irq, loongson2_i2c_isr_event, IRQF_SHARED, pdev->name, priv);
	if (ret)
		return ret;

	return devm_i2c_add_adapter(dev, adap);
}

static const struct of_device_id loongson2_i2c_id_table[] = {
	{ .compatible = "loongson,ls2k0300-i2c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, loongson2_i2c_id_table);

static struct platform_driver loongson2_i2c_driver = {
	.driver = {
		.name = "loongson2-i2c-v2",
		.of_match_table = loongson2_i2c_id_table,
	},
	.probe = loongson2_i2c_probe,
};
module_platform_driver(loongson2_i2c_driver);

MODULE_DESCRIPTION("Loongson-2K0300 I2C bus driver");
MODULE_AUTHOR("Loongson Technology Corporation Limited");
MODULE_LICENSE("GPL");
