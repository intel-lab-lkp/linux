// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Regmap i2c mux driver
 *
 * Copyright (C) 2023 Nvidia Technologies Ltd.
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_data/i2c-mux-regmap.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

/* i2c_mux_regmap - mux control structure:
 * @last_val - last selected register value or -1 if mux deselected;
 * @parent - parent I2C adapter;
 * @pdata: platform data;
 */
struct i2c_mux_regmap {
	int last_val;
	struct i2c_adapter *parent;
	struct i2c_mux_regmap_platform_data pdata;
};

static int i2c_mux_regmap_select_chan(struct i2c_mux_core *muxc, u32 chan)
{
	struct i2c_mux_regmap *mux = i2c_mux_priv(muxc);
	int err = 0;

	/* Only select the channel if its different from the last channel */
	if (mux->last_val != chan) {
		err = regmap_write(mux->pdata.regmap, mux->pdata.sel_reg_addr, chan);
		mux->last_val = err < 0 ? -1 : chan;
	}

	return err;
}

static int i2c_mux_regmap_deselect(struct i2c_mux_core *muxc, u32 chan)
{
	struct i2c_mux_regmap *mux = i2c_mux_priv(muxc);

	/* Deselect active channel */
	mux->last_val = -1;

	return regmap_write(mux->pdata.regmap, mux->pdata.sel_reg_addr, 0);
}

/* Probe/reomove functions */
static int i2c_mux_regmap_probe(struct platform_device *pdev)
{
	struct i2c_mux_regmap_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct i2c_mux_regmap *mux;
	struct i2c_mux_core *muxc;
	int num, err;

	if (!pdata)
		return -EINVAL;

	mux = devm_kzalloc(&pdev->dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	memcpy(&mux->pdata, pdata, sizeof(*pdata));
	mux->parent = i2c_get_adapter(mux->pdata.parent);
	if (!mux->parent)
		return -EPROBE_DEFER;

	muxc = i2c_mux_alloc(mux->parent, &pdev->dev, pdata->num_adaps, sizeof(*mux), 0,
			     i2c_mux_regmap_select_chan, i2c_mux_regmap_deselect);
	if (!muxc) {
		err = -ENOMEM;
		goto err_i2c_mux_alloc;
	}

	platform_set_drvdata(pdev, muxc);
	muxc->priv = mux;
	mux->last_val = -1; /* force the first selection */

	/* Create an adapter for each channel. */
	for (num = 0; num < pdata->num_adaps; num++) {
		err = i2c_mux_add_adapter(muxc, 0, pdata->chan_ids[num], 0);
		if (err)
			goto err_i2c_mux_add_adapter;
	}

	/* Notify caller when all channels' adapters are created. */
	if (pdata->completion_notify)
		pdata->completion_notify(pdata->handle, muxc->parent, muxc->adapter);

	return 0;

err_i2c_mux_add_adapter:
	i2c_mux_del_adapters(muxc);
err_i2c_mux_alloc:
	i2c_put_adapter(mux->parent);
	return err;
}

static void i2c_mux_regmap_remove(struct platform_device *pdev)
{
	struct i2c_mux_core *muxc = platform_get_drvdata(pdev);
	struct i2c_mux_regmap *mux = muxc->priv;

	i2c_mux_del_adapters(muxc);
	i2c_put_adapter(mux->parent);
}

static struct platform_driver i2c_mux_regmap_driver = {
	.driver = {
		.name = "i2c-mux-regmap",
	},
	.probe = i2c_mux_regmap_probe,
	.remove_new = i2c_mux_regmap_remove,
};

module_platform_driver(i2c_mux_regmap_driver);

MODULE_AUTHOR("Vadim Pasternak (vadimp@nvidia.com)");
MODULE_DESCRIPTION("Regmap I2C multiplexer driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:i2c-mux-regmap");
