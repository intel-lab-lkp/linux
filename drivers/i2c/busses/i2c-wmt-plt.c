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
#include <linux/of.h>
#include <linux/of_address.h>
#include "i2c-viai2c-common.h"

#define WMTI2C_REG_SLAVE_CR	0x10
#define WMTI2C_REG_SLAVE_SR	0x12
#define WMTI2C_REG_SLAVE_ISR	0x14
#define WMTI2C_REG_SLAVE_IMR	0x16
#define WMTI2C_REG_SLAVE_DR	0x18
#define WMTI2C_REG_SLAVE_TR	0x1A

/* REG_TR */
#define WMTI2C_SCL_TIMEOUT(x)		(((x) & 0xFF) << 8)
#define WMTI2C_TR_STD			0x0064
#define WMTI2C_TR_HS			0x0019

/* REG_MCR */
#define WMTI2C_MCR_APB_96M		7
#define WMTI2C_MCR_APB_166M		12

#define wmt_i2c viai2c

static u32 wmt_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_NOSTART;
}

static const struct i2c_algorithm wmt_i2c_algo = {
	.master_xfer	= viai2c_xfer,
	.functionality	= wmt_i2c_func,
};

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

	writew(0, base + VIAI2C_REG_CR);
	writew(WMTI2C_MCR_APB_166M, base + VIAI2C_REG_MCR);
	writew(VIAI2C_ISR_MASK_ALL, base + VIAI2C_REG_ISR);
	writew(VIAI2C_IMR_ENABLE_ALL, base + VIAI2C_REG_IMR);
	writew(VIAI2C_CR_ENABLE, base + VIAI2C_REG_CR);
	readw(base + VIAI2C_REG_CSR);		/* read clear */
	writew(VIAI2C_ISR_MASK_ALL, base + VIAI2C_REG_ISR);

	if (i2c->tcr == VIAI2C_TCR_FAST)
		writew(WMTI2C_SCL_TIMEOUT(128) | WMTI2C_TR_HS,
				base + VIAI2C_REG_TR);
	else
		writew(WMTI2C_SCL_TIMEOUT(128) | WMTI2C_TR_STD,
				base + VIAI2C_REG_TR);

	return 0;
}

static int wmt_i2c_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct wmt_i2c *i2c;
	struct i2c_adapter *adap;
	int err;
	u32 clk_rate;

	err = viai2c_init(pdev, &i2c);
	if (err)
		return err;

	i2c->platform = VIAI2C_PLAT_WMT;

	i2c->clk = of_clk_get(np, 0);
	if (IS_ERR(i2c->clk)) {
		dev_err(&pdev->dev, "unable to request clock\n");
		return PTR_ERR(i2c->clk);
	}

	err = of_property_read_u32(np, "clock-frequency", &clk_rate);
	if (!err && (clk_rate == I2C_MAX_FAST_MODE_FREQ))
		i2c->tcr = VIAI2C_TCR_FAST;

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
