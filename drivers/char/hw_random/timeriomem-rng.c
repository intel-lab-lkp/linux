// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/char/hw_random/timeriomem-rng.c
 *
 * Copyright (C) 2009 Alexander Clouter <alex@digriz.org.uk>
 *
 * Derived from drivers/char/hw_random/omap-rng.c
 *   Copyright 2005 (c) MontaVista Software, Inc.
 *   Author: Deepak Saxena <dsaxena@plexity.net>
 *
 * Overview:
 *   This driver is useful for platforms that have an IO range that provides
 *   periodic random data from a single IO memory address.  All the platform
 *   has to do is provide the address and 'wait time' that new data becomes
 *   available.
 *
 * The read width (8, 16, or 32 bits) and an optional data mask can be
 * configured through platform data or device tree properties.  Default is
 * 32-bit reads with no mask.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/timeriomem-rng.h>

struct timeriomem_rng_private {
	void __iomem		*io_base;
	ktime_t			period;
	unsigned int		present:1;
	unsigned int		reg_io_width;
	u32			mask;

	struct hrtimer		timer;
	struct completion	completion;

	struct hwrng		rng_ops;
};

static int timeriomem_rng_read(struct hwrng *hwrng, void *data,
				size_t max, bool wait)
{
	struct timeriomem_rng_private *priv =
		container_of(hwrng, struct timeriomem_rng_private, rng_ops);
	int retval = 0;
	int period_us = ktime_to_us(priv->period);
	int chunk = priv->reg_io_width;

	/*
	 * There may not have been enough time for new data to be generated
	 * since the last request.  If the caller doesn't want to wait, let them
	 * bail out.  Otherwise, wait for the completion.  If the new data has
	 * already been generated, the completion should already be available.
	 */
	if (!wait && !priv->present)
		return 0;

	wait_for_completion(&priv->completion);

	do {
		/*
		 * After the first read, all additional reads will need to wait
		 * for the RNG to generate new data.  Since the period can have
		 * a wide range of values (1us to 1s have been observed), allow
		 * for 1% tolerance in the sleep time rather than a fixed value.
		 */
		if (retval > 0)
			usleep_range(period_us,
					period_us + max(1, period_us / 100));

		switch (priv->reg_io_width) {
		case 1: {
			u8 val = readb(priv->io_base) & priv->mask;
			*(u8 *)data = val;
			break;
		}
		case 2: {
			u16 val = readw(priv->io_base) & priv->mask;
			*(u16 *)data = val;
			break;
		}
		case 4: {
			u32 val = readl(priv->io_base) & priv->mask;
			*(u32 *)data = val;
			break;
		}
		}

		retval += chunk;
		data += chunk;
		max -= chunk;
	} while (wait && max > chunk);

	/*
	 * Block any new callers until the RNG has had time to generate new
	 * data.
	 */
	priv->present = 0;
	reinit_completion(&priv->completion);
	hrtimer_forward_now(&priv->timer, priv->period);
	hrtimer_restart(&priv->timer);

	return retval;
}

static enum hrtimer_restart timeriomem_rng_trigger(struct hrtimer *timer)
{
	struct timeriomem_rng_private *priv
		= container_of(timer, struct timeriomem_rng_private, timer);

	priv->present = 1;
	complete(&priv->completion);

	return HRTIMER_NORESTART;
}

static int timeriomem_rng_probe(struct platform_device *pdev)
{
	struct timeriomem_rng_data *pdata = pdev->dev.platform_data;
	struct timeriomem_rng_private *priv;
	struct resource *res;
	int err = 0;
	int period;

	if (!pdev->dev.of_node && !pdata) {
		dev_err(&pdev->dev, "timeriomem_rng_data is missing\n");
		return -EINVAL;
	}

	/* Allocate memory for the device structure (and zero it) */
	priv = devm_kzalloc(&pdev->dev,
			sizeof(struct timeriomem_rng_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->io_base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(priv->io_base))
		return PTR_ERR(priv->io_base);

	priv->reg_io_width = 4;
	priv->mask = 0xFFFFFFFF;

	if (pdev->dev.of_node) {
		int i;

		if (!of_property_read_u32(pdev->dev.of_node,
						"period", &i))
			period = i;
		else {
			dev_err(&pdev->dev, "missing period\n");
			return -EINVAL;
		}

		if (!of_property_read_u32(pdev->dev.of_node,
						"quality", &i))
			priv->rng_ops.quality = i;

		of_property_read_u32(pdev->dev.of_node,
				     "reg-io-width", &priv->reg_io_width);
		of_property_read_u32(pdev->dev.of_node,
				     "mask", &priv->mask);
	} else {
		period = pdata->period;
		priv->rng_ops.quality = pdata->quality;

		if (pdata->reg_io_width_set)
			priv->reg_io_width = pdata->reg_io_width;
		if (pdata->mask_set)
			priv->mask = pdata->mask;
	}

	if (priv->reg_io_width == 0)
		priv->reg_io_width = 4;

	switch (priv->reg_io_width) {
	case 1:
	case 2:
	case 4:
		break;
	default:
		dev_err(&pdev->dev, "invalid reg-io-width %u, must be 1, 2, or 4\n",
			priv->reg_io_width);
		return -EINVAL;
	}

	if (!IS_ALIGNED(res->start, priv->reg_io_width) ||
	    resource_size(res) < priv->reg_io_width) {
		dev_err(&pdev->dev,
			"address must be %u-byte aligned\n",
			priv->reg_io_width);
		return -EINVAL;
	}

	priv->period = us_to_ktime(period);
	init_completion(&priv->completion);
	hrtimer_setup(&priv->timer, timeriomem_rng_trigger, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);

	priv->rng_ops.name = dev_name(&pdev->dev);
	priv->rng_ops.read = timeriomem_rng_read;

	/* Assume random data is already available. */
	priv->present = 1;
	complete(&priv->completion);

	err = devm_hwrng_register(&pdev->dev, &priv->rng_ops);
	if (err) {
		dev_err(&pdev->dev, "problem registering\n");
		return err;
	}

	dev_info(&pdev->dev, "%u-byte from %p @ %dus\n",
		 priv->reg_io_width, priv->io_base, period);

	return 0;
}

static void timeriomem_rng_remove(struct platform_device *pdev)
{
	struct timeriomem_rng_private *priv = platform_get_drvdata(pdev);

	hrtimer_cancel(&priv->timer);
}

static const struct of_device_id timeriomem_rng_match[] = {
	{ .compatible = "timeriomem_rng" },
	{},
};
MODULE_DEVICE_TABLE(of, timeriomem_rng_match);

static struct platform_driver timeriomem_rng_driver = {
	.driver = {
		.name		= "timeriomem_rng",
		.of_match_table	= timeriomem_rng_match,
	},
	.probe		= timeriomem_rng_probe,
	.remove		= timeriomem_rng_remove,
};

module_platform_driver(timeriomem_rng_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexander Clouter <alex@digriz.org.uk>");
MODULE_DESCRIPTION("Timer IOMEM H/W RNG driver");
