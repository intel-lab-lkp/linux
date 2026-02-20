// SPDX-License-Identifier: GPL-2.0-only
/*
 * DeviceTree provider for firmware-first CPER error source block.
 *
 * This driver shares the GHES CPER helpers so we keep the reporting and
 * notifier behaviour identical to ACPI GHES
 *
 * Copyright (C) 2025 ARM Ltd.
 * Author: Ahmed Tiba <ahmed.tiba@arm.com>
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/panic.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <acpi/ghes.h>
#include <acpi/ghes_cper.h>

static atomic_t ghes_ffh_source_ids = ATOMIC_INIT(0);

struct ghes_ffh_ack {
	void __iomem *addr;
	u64 preserve;
	u64 set;
	u8 width;
	bool present;
};

struct ghes_ffh {
	struct device *dev;
	void __iomem *status;
	size_t status_len;

	struct ghes_ffh_ack ack;

	struct acpi_hest_generic *generic;
	struct acpi_hest_generic_status *estatus;

	bool sync;
	int irq;

	/* Serializes access to the firmware-owned buffer. */
	spinlock_t lock;
};

static int ghes_ffh_init_pool(void)
{
	if (ghes_estatus_pool)
		return 0;

	return ghes_estatus_pool_init(1);
}

static int ghes_ffh_copy_status(struct ghes_ffh *ctx)
{
	memcpy_fromio(ctx->estatus, ctx->status, ctx->status_len);
	return 0;
}

static void ghes_ffh_ack(struct ghes_ffh *ctx)
{
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

static void ghes_ffh_fatal(struct ghes_ffh *ctx)
{
	__ghes_print_estatus(KERN_EMERG, ctx->generic, ctx->estatus);
	add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);
	panic("GHES: fatal firmware-first CPER record from %s\n",
	      dev_name(ctx->dev));
}

static void ghes_ffh_process(struct ghes_ffh *ctx)
{
	unsigned long flags;
	int sev;

	spin_lock_irqsave(&ctx->lock, flags);

	if (ghes_ffh_copy_status(ctx))
		goto out;

	sev = ghes_severity(ctx->estatus->error_severity);
	if (sev >= GHES_SEV_PANIC)
		ghes_ffh_fatal(ctx);

	if (!ghes_estatus_cached(ctx->estatus)) {
		if (ghes_print_estatus(NULL, ctx->generic, ctx->estatus))
			ghes_estatus_cache_add(ctx->generic, ctx->estatus);
	}

	ghes_cper_handle_status(ctx->dev, ctx->generic, ctx->estatus, ctx->sync);

	ghes_ffh_ack(ctx);

out:
	spin_unlock_irqrestore(&ctx->lock, flags);
}

static irqreturn_t ghes_ffh_irq(int irq, void *data)
{
	struct ghes_ffh *ctx = data;

	ghes_ffh_process(ctx);

	return IRQ_HANDLED;
}

static int ghes_ffh_init_ack(struct platform_device *pdev,
			     struct ghes_ffh *ctx)
{
	struct resource *res;
	size_t size;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return 0;

	ctx->ack.addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ctx->ack.addr))
		return PTR_ERR(ctx->ack.addr);

	size = resource_size(res);
	switch (size) {
	case 4:
		ctx->ack.width = 32;
		ctx->ack.preserve = ~0U;
		break;
	case 8:
		ctx->ack.width = 64;
		ctx->ack.preserve = ~0ULL;
		break;
	default:
		dev_err(&pdev->dev, "Unsupported ack resource size %zu\n", size);
		return -EINVAL;
	}

	ctx->ack.set = BIT_ULL(0);
	ctx->ack.present = true;
	return 0;
}

static int ghes_ffh_probe(struct platform_device *pdev)
{
	struct ghes_ffh *ctx;
	struct resource *res;
	int rc;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	spin_lock_init(&ctx->lock);
	ctx->dev = &pdev->dev;
	ctx->sync = of_property_read_bool(pdev->dev.of_node, "arm,sea-notify");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "status region missing\n");
		return -EINVAL;
	}

	ctx->status_len = resource_size(res);
	if (!ctx->status_len) {
		dev_err(&pdev->dev, "Status region has zero length\n");
		return -EINVAL;
	}

	ctx->status = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ctx->status))
		return PTR_ERR(ctx->status);

	rc = ghes_ffh_init_ack(pdev, ctx);
	if (rc)
		return rc;

	rc = ghes_ffh_init_pool();
	if (rc)
		return rc;

	ctx->estatus = devm_kzalloc(&pdev->dev, ctx->status_len, GFP_KERNEL);
	if (!ctx->estatus)
		return -ENOMEM;

	ctx->generic = devm_kzalloc(&pdev->dev, sizeof(*ctx->generic), GFP_KERNEL);
	if (!ctx->generic)
		return -ENOMEM;

	ctx->generic->header.type = ACPI_HEST_TYPE_GENERIC_ERROR;
	ctx->generic->header.source_id =
		atomic_inc_return(&ghes_ffh_source_ids);
	ctx->generic->notify.type = ctx->sync ?
		ACPI_HEST_NOTIFY_SEA : ACPI_HEST_NOTIFY_EXTERNAL;
	ctx->generic->error_block_length = ctx->status_len;

	ctx->irq = platform_get_irq_optional(pdev, 0);
	if (ctx->irq <= 0) {
		if (ctx->irq == -EPROBE_DEFER)
			return ctx->irq;
		dev_err(&pdev->dev, "interrupt is required (%d)\n", ctx->irq);
		return -EINVAL;
	}

	rc = devm_request_threaded_irq(&pdev->dev, ctx->irq,
				       NULL, ghes_ffh_irq,
				       IRQF_ONESHOT,
				       dev_name(&pdev->dev), ctx);
	if (rc)
		return rc;

	platform_set_drvdata(pdev, ctx);
	dev_info(&pdev->dev, "Firmware-first CPER status provider (interrupt)\n");
	return 0;
}

static void ghes_ffh_remove(struct platform_device *pdev)
{
}

static const struct of_device_id ghes_ffh_of_match[] = {
	{ .compatible = "arm,ras-ffh" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ghes_ffh_of_match);

static struct platform_driver ghes_ffh_driver = {
	.driver = {
		.name = "esource-dt",
		.of_match_table = ghes_ffh_of_match,
	},
	.probe = ghes_ffh_probe,
	.remove = ghes_ffh_remove,
};

module_platform_driver(ghes_ffh_driver);

MODULE_AUTHOR("Ahmed Tiba <ahmed.tiba@arm.com>");
MODULE_DESCRIPTION("Firmware-first CPER provider for DeviceTree platforms");
MODULE_LICENSE("GPL");
