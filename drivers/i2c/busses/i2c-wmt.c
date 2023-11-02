// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Wondermedia I2C Master Mode Driver
 *
 *  Copyright (C) 2012 Tony Prisk <linux@prisktech.co.nz>
 *
 *  Derived from GPLv2+ licensed source:
 *  - Copyright (C) 2008 WonderMedia Technologies, Inc.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#define WMTI2C_REG_CR		0x00
#define WMTI2C_REG_TCR		0x02
#define WMTI2C_REG_CSR		0x04
#define WMTI2C_REG_ISR		0x06
#define WMTI2C_REG_IMR		0x08
#define WMTI2C_REG_CDR		0x0A
#define WMTI2C_REG_TR		0x0C
#define WMTI2C_REG_MCR		0x0E
#define WMTI2C_REG_SLAVE_CR	0x10
#define WMTI2C_REG_SLAVE_SR	0x12
#define WMTI2C_REG_SLAVE_ISR	0x14
#define WMTI2C_REG_SLAVE_IMR	0x16
#define WMTI2C_REG_SLAVE_DR	0x18
#define WMTI2C_REG_SLAVE_TR	0x1A

/* REG_CR Bit fields */
#define WMTI2C_CR_TX_NEXT_ACK		0x0000
#define WMTI2C_CR_ENABLE		0x0001
#define WMTI2C_CR_TX_NEXT_NO_ACK	0x0002
#define WMTI2C_CR_TX_END		0x0004
#define WMTI2C_CR_CPU_RDY		0x0008
#define WMTI2C_SLAV_MODE_SEL		0x8000

/* REG_TCR Bit fields */
#define WMTI2C_TCR_STANDARD_MODE	0x0000
#define WMTI2C_TCR_MASTER_WRITE		0x0000
#define WMTI2C_TCR_HS_MODE		0x2000
#define WMTI2C_TCR_MASTER_READ		0x4000
#define WMTI2C_TCR_FAST_MODE		0x8000
#define WMTI2C_TCR_SLAVE_ADDR_MASK	0x007F

/* REG_ISR Bit fields */
#define WMTI2C_ISR_NACK_ADDR		0x0001
#define WMTI2C_ISR_BYTE_END		0x0002
#define WMTI2C_ISR_SCL_TIMEOUT		0x0004
#define WMTI2C_ISR_WRITE_ALL		0x0007

/* REG_IMR Bit fields */
#define WMTI2C_IMR_ENABLE_ALL		0x0007

/* REG_CSR Bit fields */
#define WMTI2C_CSR_RCV_NOT_ACK		0x0001
#define WMTI2C_CSR_RCV_ACK_MASK		0x0001
#define WMTI2C_CSR_READY_MASK		0x0002

/* REG_TR */
#define WMTI2C_SCL_TIMEOUT(x)		(((x) & 0xFF) << 8)
#define WMTI2C_TR_STD			0x0064
#define WMTI2C_TR_HS			0x0019

/* REG_MCR */
#define WMTI2C_MCR_APB_96M		7
#define WMTI2C_MCR_APB_166M		12

#define WMT_I2C_TIMEOUT		(msecs_to_jiffies(1000))

struct wmt_i2c {
	struct i2c_adapter	adapter;
	struct completion	complete;
	struct device		*dev;
	void __iomem		*base;
	struct clk		*clk;
	u16			tcr;
	int			irq;
	u16			cmd_status;
};

static int wmt_i2c_wait_bus_not_busy(struct wmt_i2c *i2c)
{
	unsigned long timeout;
	void __iomem *base = i2c->base;

	timeout = jiffies + WMT_I2C_TIMEOUT;
	while (!(readw(base + WMTI2C_REG_CSR) & WMTI2C_CSR_READY_MASK)) {
		if (time_after(jiffies, timeout)) {
			dev_warn(i2c->dev,
					"timeout waiting for bus ready\n");
			return -EBUSY;
		}
		msleep(20);
	}

	return 0;
}

static int wmt_check_status(struct wmt_i2c *i2c)
{
	int ret = 0;
	unsigned long wait_result;

	wait_result = wait_for_completion_timeout(&i2c->complete,
						msecs_to_jiffies(500));
	if (!wait_result)
		return -ETIMEDOUT;

	if (i2c->cmd_status & WMTI2C_ISR_NACK_ADDR)
		ret = -EIO;

	if (i2c->cmd_status & WMTI2C_ISR_SCL_TIMEOUT)
		ret = -ETIMEDOUT;

	return ret;
}

static int wmt_i2c_write(struct wmt_i2c *i2c, struct i2c_msg *pmsg,
			 int last)
{
	u16 val, tcr_val = i2c->tcr;
	int ret;
	int xfer_len = 0;
	void __iomem *base = i2c->base;

	if (pmsg->len == 0) {
		/*
		 * We still need to run through the while (..) once, so
		 * start at -1 and break out early from the loop
		 */
		xfer_len = -1;
		writew(0, base + WMTI2C_REG_CDR);
	} else {
		writew(pmsg->buf[0] & 0xFF, base + WMTI2C_REG_CDR);
	}

	if (!(pmsg->flags & I2C_M_NOSTART)) {
		val = readw(base + WMTI2C_REG_CR);
		val &= ~WMTI2C_CR_TX_END;
		val |= WMTI2C_CR_CPU_RDY;
		writew(val, base + WMTI2C_REG_CR);
	}

	reinit_completion(&i2c->complete);

	tcr_val |= (WMTI2C_TCR_MASTER_WRITE
		| (pmsg->addr & WMTI2C_TCR_SLAVE_ADDR_MASK));

	writew(tcr_val, base + WMTI2C_REG_TCR);

	if (pmsg->flags & I2C_M_NOSTART) {
		val = readw(base + WMTI2C_REG_CR);
		val |= WMTI2C_CR_CPU_RDY;
		writew(val, base + WMTI2C_REG_CR);
	}

	while (xfer_len < pmsg->len) {
		ret = wmt_check_status(i2c);
		if (ret)
			return ret;

		xfer_len++;

		val = readw(base + WMTI2C_REG_CSR);
		if (val & WMTI2C_CSR_RCV_NOT_ACK) {
			dev_dbg(i2c->dev, "write RCV NACK error\n");
			return -EIO;
		}

		if (pmsg->len == 0) {
			val = WMTI2C_CR_TX_END | WMTI2C_CR_CPU_RDY
				| WMTI2C_CR_ENABLE;
			writew(val, base + WMTI2C_REG_CR);
			break;
		}

		if (xfer_len == pmsg->len) {
			if (last != 1)
				writew(WMTI2C_CR_ENABLE, base + WMTI2C_REG_CR);
		} else {
			writew(pmsg->buf[xfer_len] & 0xFF,
					base + WMTI2C_REG_CDR);
			writew(WMTI2C_CR_CPU_RDY | WMTI2C_CR_ENABLE,
					base + WMTI2C_REG_CR);
		}
	}

	return 0;
}

static int wmt_i2c_read(struct wmt_i2c *i2c, struct i2c_msg *pmsg)
{
	u16 val, tcr_val = i2c->tcr;
	int ret;
	u32 xfer_len = 0;
	void __iomem *base = i2c->base;

	val = readw(base + WMTI2C_REG_CR);
	val &= ~(WMTI2C_CR_TX_END | WMTI2C_CR_TX_NEXT_NO_ACK);

	if (!(pmsg->flags & I2C_M_NOSTART))
		val |= WMTI2C_CR_CPU_RDY;

	if (pmsg->len == 1)
		val |= WMTI2C_CR_TX_NEXT_NO_ACK;

	writew(val, base + WMTI2C_REG_CR);

	reinit_completion(&i2c->complete);

	tcr_val |= WMTI2C_TCR_MASTER_READ
		| (pmsg->addr & WMTI2C_TCR_SLAVE_ADDR_MASK);

	writew(tcr_val, base + WMTI2C_REG_TCR);

	if (pmsg->flags & I2C_M_NOSTART) {
		val = readw(base + WMTI2C_REG_CR);
		val |= WMTI2C_CR_CPU_RDY;
		writew(val, base + WMTI2C_REG_CR);
	}

	while (xfer_len < pmsg->len) {
		ret = wmt_check_status(i2c);
		if (ret)
			return ret;

		pmsg->buf[xfer_len] = readw(base + WMTI2C_REG_CDR) >> 8;
		xfer_len++;

		val = readw(base + WMTI2C_REG_CR) | WMTI2C_CR_CPU_RDY;
		if (xfer_len == pmsg->len - 1)
			val |= WMTI2C_CR_TX_NEXT_NO_ACK;
		writew(val, base + WMTI2C_REG_CR);
	}

	return 0;
}

static int wmt_i2c_xfer(struct i2c_adapter *adap,
			struct i2c_msg msgs[],
			int num)
{
	struct i2c_msg *pmsg;
	int i;
	int ret = 0;
	struct wmt_i2c *i2c = i2c_get_adapdata(adap);

	for (i = 0; ret >= 0 && i < num; i++) {
		pmsg = &msgs[i];
		if (!(pmsg->flags & I2C_M_NOSTART)) {
			ret = wmt_i2c_wait_bus_not_busy(i2c);
			if (ret < 0)
				return ret;
		}

		if (pmsg->flags & I2C_M_RD)
			ret = wmt_i2c_read(i2c, pmsg);
		else
			ret = wmt_i2c_write(i2c, pmsg, (i + 1) == num);
	}

	return (ret < 0) ? ret : i;
}

static u32 wmt_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_NOSTART;
}

static const struct i2c_algorithm wmt_i2c_algo = {
	.master_xfer	= wmt_i2c_xfer,
	.functionality	= wmt_i2c_func,
};

static irqreturn_t wmt_i2c_isr(int irq, void *data)
{
	struct wmt_i2c *i2c = data;

	/* save the status and write-clear it */
	i2c->cmd_status = readw(i2c->base + WMTI2C_REG_ISR);
	writew(i2c->cmd_status, i2c->base + WMTI2C_REG_ISR);

	complete(&i2c->complete);

	return IRQ_HANDLED;
}

int wmt_i2c_init(struct platform_device *pdev, struct wmt_i2c **pi2c)
{
	int err;
	int irq_flags;
	struct wmt_i2c *i2c;
	struct device_node *np = pdev->dev.of_node;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	if (np) {
		irq_flags = 0;
		i2c->irq = irq_of_parse_and_map(np, 0);
		if (!i2c->irq)
			return -EINVAL;
	} else {
		irq_flags = IRQF_SHARED;
		i2c->irq = platform_get_irq(pdev, 0);
		if (i2c->irq < 0)
			return i2c->irq;
	}

	err = devm_request_irq(&pdev->dev, i2c->irq, wmt_i2c_isr,
					irq_flags, pdev->name, i2c);
	if (err)
		return dev_err_probe(&pdev->dev, err,
				"failed to request irq %i\n", i2c->irq);

	i2c->dev = &pdev->dev;
	init_completion(&i2c->complete);
	platform_set_drvdata(pdev, i2c);

	*pi2c = i2c;
	return 0;
}

static int wmt_i2c_reset_hardware(struct wmt_i2c *i2c)
{
	int err;
	void __iomem *base = i2c->base;

	err = clk_prepare_enable(i2c->clk);
	if (err) {
		dev_err(i2c->dev, "failed to enable clock\n");
		return err;
	}

	err = clk_set_rate(i2c->clk, 20000000);
	if (err) {
		dev_err(i2c->dev, "failed to set clock = 20Mhz\n");
		clk_disable_unprepare(i2c->clk);
		return err;
	}

	writew(0, base + WMTI2C_REG_CR);
	writew(WMTI2C_MCR_APB_166M, base + WMTI2C_REG_MCR);
	writew(WMTI2C_ISR_WRITE_ALL, base + WMTI2C_REG_ISR);
	writew(WMTI2C_IMR_ENABLE_ALL, base + WMTI2C_REG_IMR);
	writew(WMTI2C_CR_ENABLE, base + WMTI2C_REG_CR);
	readw(base + WMTI2C_REG_CSR);		/* read clear */
	writew(WMTI2C_ISR_WRITE_ALL, base + WMTI2C_REG_ISR);

	if (i2c->tcr == WMTI2C_TCR_FAST_MODE)
		writew(WMTI2C_SCL_TIMEOUT(128) | WMTI2C_TR_HS,
				base + WMTI2C_REG_TR);
	else
		writew(WMTI2C_SCL_TIMEOUT(128) | WMTI2C_TR_STD,
				base + WMTI2C_REG_TR);

	return 0;
}

static int wmt_i2c_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct wmt_i2c *i2c;
	struct i2c_adapter *adap;
	int err;
	u32 clk_rate;

	err = wmt_i2c_init(pdev, &i2c);
	if (err)
		return err;

	i2c->clk = of_clk_get(np, 0);
	if (IS_ERR(i2c->clk)) {
		dev_err(&pdev->dev, "unable to request clock\n");
		return PTR_ERR(i2c->clk);
	}

	err = of_property_read_u32(np, "clock-frequency", &clk_rate);
	if (!err && (clk_rate == I2C_MAX_FAST_MODE_FREQ))
		i2c->tcr = WMTI2C_TCR_FAST_MODE;

	adap = &i2c->adapter;
	i2c_set_adapdata(adap, i2c);
	strscpy(adap->name, "WMT I2C adapter", sizeof(adap->name));
	adap->owner = THIS_MODULE;
	adap->algo = &wmt_i2c_algo;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = pdev->dev.of_node;

	err = wmt_i2c_reset_hardware(i2c);
	if (err) {
		dev_err(&pdev->dev, "error initializing hardware\n");
		return err;
	}

	return devm_i2c_add_adapter(&pdev->dev, &i2c->adapter);
}

static const struct of_device_id wmt_i2c_dt_ids[] = {
	{ .compatible = "wm,wm8505-i2c" },
	{ /* Sentinel */ },
};

static struct platform_driver wmt_i2c_driver = {
	.probe		= wmt_i2c_probe,
	.driver		= {
		.name	= "wmt-i2c",
		.of_match_table = wmt_i2c_dt_ids,
	},
};

module_platform_driver(wmt_i2c_driver);

MODULE_DESCRIPTION("Wondermedia I2C master-mode bus adapter");
MODULE_AUTHOR("Tony Prisk <linux@prisktech.co.nz>");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, wmt_i2c_dt_ids);
