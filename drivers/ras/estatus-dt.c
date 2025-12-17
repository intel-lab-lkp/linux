// SPDX-License-Identifier: GPL-2.0-only
/*
 * DeviceTree provider for firmware-first estatus error records
 *
 * Copyright (C) 2025 ARM Ltd.
 * Author: Ahmed Tiba <ahmed.tiba@arm.com>
 */

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/timer.h>

#include <linux/estatus.h>
#include <asm/fixmap.h>

struct estatus_dt_ack {
	void __iomem *addr;
	u64 preserve;
	u64 set;
	u8 width;
	bool present;
};

struct estatus_dt {
	struct device *dev;
	void __iomem *base;
	phys_addr_t phys;
	size_t block_size;

	struct estatus_dt_ack ack;

	struct estatus_source source;
	int irq;

	struct timer_list poll_timer;
	u32 poll_interval_ms;
	bool polling;

	bool sea_notify;
};

static int estatus_dt_get_phys(struct estatus_source *source, phys_addr_t *addr)
{
	struct estatus_dt *ctx = source->priv;

	*addr = ctx->phys;
	return 0;
}

static int estatus_dt_read(struct estatus_source *source, phys_addr_t addr,
			   void *buf, size_t len,
			   enum fixed_addresses fixmap_idx)
{
	struct estatus_dt *ctx = source->priv;

	(void)addr;
	(void)fixmap_idx;

	if (WARN_ON(len > ctx->block_size))
		len = ctx->block_size;

	memcpy_fromio(buf, ctx->base, len);

	return 0;
}

static int estatus_dt_write(struct estatus_source *source, phys_addr_t addr,
			    const void *buf, size_t len,
			    enum fixed_addresses fixmap_idx)
{
	struct estatus_dt *ctx = source->priv;

	(void)addr;
	(void)fixmap_idx;

	if (WARN_ON(len > ctx->block_size))
		len = ctx->block_size;

	memcpy_toio(ctx->base, buf, len);

	return 0;
}

static void estatus_dt_ack(struct estatus_source *source)
{
	struct estatus_dt *ctx = source->priv;
	u64 val;

	if (!ctx->ack.present)
		return;

	if (ctx->ack.width == 64) {
		val = readq(ctx->ack.addr);
		val &= ctx->ack.preserve;
		val |= ctx->ack.set;
		writeq(val, ctx->ack.addr);
	} else {
		val = readl(ctx->ack.addr);
		val &= (u32)ctx->ack.preserve;
		val |= (u32)ctx->ack.set;
		writel(val, ctx->ack.addr);
	}
}

static size_t estatus_dt_get_max_len(struct estatus_source *source)
{
	struct estatus_dt *ctx = source->priv;

	return ctx->block_size;
}

static enum estatus_notify_mode
estatus_dt_get_notify_mode(struct estatus_source *source)
{
	struct estatus_dt *ctx = source->priv;

	if (ctx->sea_notify)
		return ESTATUS_NOTIFY_SEA;

	return ESTATUS_NOTIFY_ASYNC;
}

static const char *estatus_dt_get_name(struct estatus_source *source)
{
	struct estatus_dt *ctx = source->priv;

	return dev_name(ctx->dev);
}

static const struct estatus_ops estatus_dt_ops = {
	.get_phys	= estatus_dt_get_phys,
	.read		= estatus_dt_read,
	.write		= estatus_dt_write,
	.ack		= estatus_dt_ack,
	.get_max_len	= estatus_dt_get_max_len,
	.get_notify_mode = estatus_dt_get_notify_mode,
	.get_name	= estatus_dt_get_name,
};

static irqreturn_t estatus_dt_irq(int irq, void *data)
{
	struct estatus_dt *ctx = data;

	if (estatus_proc(&ctx->source))
		return IRQ_NONE;

	return IRQ_HANDLED;
}

static void estatus_dt_poll(struct timer_list *t)
{
	struct estatus_dt *ctx = timer_container_of(ctx, t, poll_timer);

	estatus_proc(&ctx->source);
	mod_timer(&ctx->poll_timer,
		  jiffies + msecs_to_jiffies(ctx->poll_interval_ms));
}

static int estatus_dt_init_ack(struct platform_device *pdev,
			       struct estatus_dt *ctx)
{
	struct resource *res;
	u64 preserve;
	size_t size;
	u32 width;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ack");
	if (!res)
		return 0;

	ctx->ack.addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ctx->ack.addr))
		return PTR_ERR(ctx->ack.addr);

	size = resource_size(res);
	if (size == 4)
		width = 32;
	else if (size == 8)
		width = 64;
	else {
		dev_err(&pdev->dev, "Unsupported ack resource size %zu\n", size);
		return -EINVAL;
	}
	ctx->ack.width = width;

	preserve = width == 64 ? ~0ULL : ~0U;
	ctx->ack.preserve = preserve;
	ctx->ack.set = BIT_ULL(0);

	ctx->ack.present = true;

	return 0;
}

static int estatus_dt_probe(struct platform_device *pdev)
{
	struct estatus_dt *ctx;
	struct resource *res;
	size_t block_size;
	int rc;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	block_size = resource_size(res);
	if (!block_size) {
		dev_err(&pdev->dev, "Status block region has zero size\n");
		return -EINVAL;
	}

	rc = estatus_pool_init(1);
	if (rc)
		return rc;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = &pdev->dev;
	ctx->sea_notify = of_property_read_bool(pdev->dev.of_node,
						"arm,sea-notify");
	ctx->poll_interval_ms = 0;
	of_property_read_u32(pdev->dev.of_node,
			     "poll-interval", &ctx->poll_interval_ms);
	if (ctx->poll_interval_ms)
		ctx->polling = true;

	ctx->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ctx->base))
		return PTR_ERR(ctx->base);

	ctx->phys = res->start;
	ctx->block_size = block_size;

	ctx->source.ops = &estatus_dt_ops;
	ctx->source.priv = ctx;
	ctx->source.estatus = devm_kzalloc(&pdev->dev, block_size, GFP_KERNEL);
	if (!ctx->source.estatus)
		return -ENOMEM;
	ctx->source.fixmap_idx = FIX_ESTATUS_IRQ;

	rc = estatus_dt_init_ack(pdev, ctx);
	if (rc)
		return rc;

	ctx->irq = platform_get_irq_optional(pdev, 0);
	if (ctx->irq < 0) {
		if (ctx->irq != -ENXIO && ctx->irq != -EINVAL)
			return ctx->irq;
		ctx->irq = 0;
	}

	if (ctx->irq > 0) {
		rc = devm_request_threaded_irq(&pdev->dev, ctx->irq,
					       NULL, estatus_dt_irq,
					       IRQF_ONESHOT,
					       dev_name(&pdev->dev), ctx);
		if (rc)
			return rc;
	}

	if (!ctx->polling && ctx->irq <= 0) {
		dev_err(&pdev->dev,
			"Either poll-interval or an interrupt is required\n");
		return -EINVAL;
	}

	if (ctx->polling) {
		timer_setup(&ctx->poll_timer, estatus_dt_poll, 0);
		mod_timer(&ctx->poll_timer,
			  jiffies + msecs_to_jiffies(ctx->poll_interval_ms));
	}

	platform_set_drvdata(pdev, ctx);

	dev_info(&pdev->dev, "Registered estatus provider (%s)\n",
		 ctx->polling ? "polling" : "interrupt");

	return 0;
}

static void estatus_dt_remove(struct platform_device *pdev)
{
	struct estatus_dt *ctx = platform_get_drvdata(pdev);

	if (ctx->polling)
		timer_delete_sync(&ctx->poll_timer);
}

static const struct of_device_id estatus_dt_of_match[] = {
	{ .compatible = "arm,ras-ffh", },
	{}
};
MODULE_DEVICE_TABLE(of, estatus_dt_of_match);

static struct platform_driver estatus_dt_driver = {
	.probe = estatus_dt_probe,
	.remove = estatus_dt_remove,
	.driver = {
		.name = "estatus-dt",
		.of_match_table = estatus_dt_of_match,
	},
};

module_platform_driver(estatus_dt_driver);

MODULE_AUTHOR("Ahmed Tiba <ahmed.tiba@arm.com>");
MODULE_DESCRIPTION("DeviceTree estatus provider");
MODULE_LICENSE("GPL");
