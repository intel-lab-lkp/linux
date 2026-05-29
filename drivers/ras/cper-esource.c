// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware-first CPER error source provider.
 *
 * This driver shares the GHES CPER helpers so we keep the reporting and
 * notifier behaviour identical to ACPI GHES.
 *
 * Copyright (C) 2026 ARM Ltd.
 * Author: Ahmed Tiba <ahmed.tiba@arm.com>
 */

#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/panic.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <acpi/ghes.h>
#include <acpi/ghes_cper.h>

static DEFINE_IDA(cper_esource_source_ids);

struct cper_esource_ack {
	void __iomem *addr;
	u64 preserve;
	u64 set;
	u8 width;
	bool present;
};

struct cper_esource {
	struct device *dev;
	void __iomem *status;
	size_t status_len;

	struct cper_esource_ack ack;

	struct acpi_hest_generic *generic;
	struct acpi_hest_generic_status *estatus;

	bool sync;
	int irq;

	/* Serializes access while firmware and the OS share the status buffer. */
	spinlock_t lock;
};

static void cper_esource_release_source_id(void *data)
{
	struct acpi_hest_generic *generic = data;

	ida_free(&cper_esource_source_ids, generic->header.source_id);
}

static int cper_esource_init_pool(void)
{
	if (ghes_estatus_pool)
		return 0;

	return ghes_estatus_pool_init(1);
}

static int cper_esource_copy_status(struct cper_esource *ctx)
{
	memcpy_fromio(ctx->estatus, ctx->status, ctx->status_len);
	return 0;
}

static void cper_esource_ack(struct cper_esource *ctx)
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

static void cper_esource_fatal(struct cper_esource *ctx)
{
	__ghes_print_estatus(KERN_EMERG, ctx->generic, ctx->estatus);
	add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);
	panic("GHES: fatal firmware-first CPER record from %s\n",
	      dev_name(ctx->dev));
}

static void cper_esource_process(struct cper_esource *ctx)
{
	int sev;

	guard(spinlock_irqsave)(&ctx->lock);

	if (cper_esource_copy_status(ctx))
		return;

	sev = ghes_severity(ctx->estatus->error_severity);
	if (sev >= GHES_SEV_PANIC)
		cper_esource_fatal(ctx);

	if (!ghes_estatus_cached(ctx->estatus) &&
	    ghes_print_estatus(NULL, ctx->generic, ctx->estatus))
		ghes_estatus_cache_add(ctx->generic, ctx->estatus);

	ghes_cper_handle_status(ctx->dev, ctx->generic, ctx->estatus, ctx->sync);
	cper_esource_ack(ctx);
}

static irqreturn_t cper_esource_irq(int irq, void *data)
{
	struct cper_esource *ctx = data;

	cper_esource_process(ctx);

	return IRQ_HANDLED;
}

static int cper_esource_init_ack(struct platform_device *pdev,
				 struct cper_esource *ctx)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	size_t size;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return 0;

	ctx->ack.addr = devm_platform_get_and_ioremap_resource(pdev, 1, &res);
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
		return dev_err_probe(dev, -EINVAL,
				     "unsupported ack resource size %zu\n", size);
	}

	ctx->ack.set = BIT_ULL(0);
	ctx->ack.present = true;
	return 0;
}

static int cper_esource_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cper_esource *ctx;
	struct resource *res;
	int source_id;
	int rc;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	spin_lock_init(&ctx->lock);
	ctx->dev = dev;
	ctx->sync = device_property_read_bool(dev, "arm,sea-notify");

	ctx->status = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(ctx->status))
		return dev_err_probe(dev, PTR_ERR(ctx->status),
				     "failed to map status region\n");

	ctx->status_len = resource_size(res);
	if (!ctx->status_len)
		return dev_err_probe(dev, -EINVAL, "status region has zero length\n");

	rc = cper_esource_init_ack(pdev, ctx);
	if (rc)
		return rc;

	rc = cper_esource_init_pool();
	if (rc)
		return rc;

	ctx->estatus = devm_kzalloc(dev, ctx->status_len, GFP_KERNEL);
	if (!ctx->estatus)
		return -ENOMEM;

	ctx->generic = devm_kzalloc(dev, sizeof(*ctx->generic), GFP_KERNEL);
	if (!ctx->generic)
		return -ENOMEM;

	source_id = ida_alloc_min(&cper_esource_source_ids, 1, GFP_KERNEL);
	if (source_id < 0)
		return source_id;

	ctx->generic->header.type = ACPI_HEST_TYPE_GENERIC_ERROR;
	ctx->generic->header.source_id = source_id;

	rc = devm_add_action_or_reset(dev, cper_esource_release_source_id,
				      ctx->generic);
	if (rc)
		return rc;

	ctx->generic->notify.type = ctx->sync ?
		ACPI_HEST_NOTIFY_SEA : ACPI_HEST_NOTIFY_EXTERNAL;
	ctx->generic->error_block_length = ctx->status_len;

	ctx->irq = platform_get_irq(pdev, 0);
	if (ctx->irq < 0)
		return ctx->irq;

	rc = devm_request_threaded_irq(dev, ctx->irq, NULL, cper_esource_irq,
				       IRQF_ONESHOT,
				       dev_name(dev), ctx);
	if (rc)
		return dev_err_probe(dev, rc, "failed to request interrupt\n");

	return 0;
}

static const struct of_device_id cper_esource_of_match[] = {
	{ .compatible = "arm,ras-cper" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cper_esource_of_match);

static struct platform_driver cper_esource_driver = {
	.driver = {
		.name = "cper-esource",
		.of_match_table = cper_esource_of_match,
	},
	.probe = cper_esource_probe,
};

module_platform_driver(cper_esource_driver);

MODULE_AUTHOR("Ahmed Tiba <ahmed.tiba@arm.com>");
MODULE_DESCRIPTION("Firmware-first CPER provider");
MODULE_LICENSE("GPL");
