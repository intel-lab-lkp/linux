// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Regmap i2c mux driver
 *
 * Copyright (C) 2026 Nvidia Technologies Ltd.
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
 * @pdata: platform data;
 */
struct i2c_mux_regmap {
	int last_val;
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

static void i2c_mux_regmap_notify_probe_abandon(struct platform_device *pdev,
						struct i2c_mux_regmap *mux,
						struct i2c_mux_core *muxc,
						struct i2c_adapter *parent,
						int err, bool user_notify_called)
{
	const struct i2c_mux_regmap_platform_data *pd;

	if (user_notify_called || err == -EPROBE_DEFER)
		return;
	pd = mux ? &mux->pdata : dev_get_platdata(&pdev->dev);
	if (!pd || !pd->completion_notify || !pd->handle)
		return;
	pd->completion_notify(pd->handle, muxc ? muxc->parent : parent, NULL);
}

/* Probe/remove functions */
static int i2c_mux_regmap_probe(struct platform_device *pdev)
{
	struct i2c_mux_regmap_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct i2c_mux_regmap *mux;
	struct i2c_adapter *parent;
	struct i2c_mux_core *muxc = NULL;
	bool user_notify_called = false;
	int num, ret;

	if (!pdata)
		return -EINVAL;

	parent = i2c_get_adapter(pdata->parent);
	if (!parent)
		return -EPROBE_DEFER;

	muxc = i2c_mux_alloc(parent, &pdev->dev, pdata->num_adaps, sizeof(*mux), 0,
			     i2c_mux_regmap_select_chan, i2c_mux_regmap_deselect);
	if (!muxc) {
		ret = -ENOMEM;
		goto err_put_parent;
	}

	mux = i2c_mux_priv(muxc);
	memcpy(&mux->pdata, pdata, sizeof(*pdata));
	platform_set_drvdata(pdev, muxc);
	mux->last_val = -1; /* force the first selection */

	/* Create an adapter for each channel. */
	for (num = 0; num < pdata->num_adaps; num++) {
		ret = i2c_mux_add_adapter(muxc, 0, pdata->chan_ids[num]);
		if (ret)
			goto err_i2c_mux_add_adapter;
	}

	/* Notify caller when all channels' adapters are created. */
	if (pdata->completion_notify) {
		ret = pdata->completion_notify(pdata->handle, muxc->parent,
					       muxc->adapter);
		user_notify_called = true;
		if (ret)
			goto err_i2c_mux_add_adapter;
	}

	return 0;

err_i2c_mux_add_adapter:
	i2c_mux_regmap_notify_probe_abandon(pdev, mux, muxc, parent, ret,
					    user_notify_called);
	i2c_mux_del_adapters(muxc);
	goto put_parent;
err_put_parent:
	i2c_mux_regmap_notify_probe_abandon(pdev, mux, muxc, parent, ret,
					    user_notify_called);
put_parent:
	i2c_put_adapter(parent);

	return ret;
}

static void i2c_mux_regmap_remove(struct platform_device *pdev)
{
	struct i2c_mux_core *muxc = platform_get_drvdata(pdev);

	i2c_mux_del_adapters(muxc);
	i2c_put_adapter(muxc->parent);
}

static struct platform_driver i2c_mux_regmap_driver = {
	.driver = {
		.name = "i2c-mux-regmap",
	},
	.probe = i2c_mux_regmap_probe,
	.remove = i2c_mux_regmap_remove,
};

module_platform_driver(i2c_mux_regmap_driver);

MODULE_AUTHOR("Vadim Pasternak (vadimp@nvidia.com)");
MODULE_DESCRIPTION("Regmap I2C multiplexer driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:i2c-mux-regmap");
