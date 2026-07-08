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
#include <linux/cper.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_reserved_mem.h>
#include <linux/panic.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <acpi/ghes.h>
#include <acpi/ghes_cper.h>

static DEFINE_IDA(cper_esource_source_ids);
static DEFINE_MUTEX(cper_esource_pool_lock);
static bool cper_esource_pool_ready;

struct cper_esource_ack {
	void *addr;
	u64 preserve;
	u64 set;
	u8 width;
	bool present;
};

struct cper_esource {
	struct device *dev;
	void *status;
	size_t status_len;

	struct cper_esource_ack ack;

	struct acpi_hest_generic generic;
	struct acpi_hest_generic_status *estatus;

	int irq;
};

static void *cper_esource_map_region(struct device *dev, unsigned int index,
				     size_t *size)
{
	struct resource res;
	void *addr;

	if (of_reserved_mem_region_to_resource(dev->of_node, index, &res))
		return ERR_PTR(dev_err_probe(dev, -EINVAL,
					     "unable to resolve memory-region %u\n",
					     index));

	*size = resource_size(&res);
	if (!*size)
		return ERR_PTR(dev_err_probe(dev, -EINVAL,
					     "memory-region %u has zero length\n",
					     index));

	addr = devm_memremap(dev, res.start, *size, MEMREMAP_WB);
	if (!addr)
		return ERR_PTR(dev_err_probe(dev, -ENOMEM,
					     "failed to map memory-region %u\n",
					     index));

	return addr;
}

static void cper_esource_release_source_id(void *data)
{
	struct cper_esource *ctx = data;

	ida_free(&cper_esource_source_ids, ctx->generic.header.source_id);
}

static int cper_esource_init_pool(void)
{
	int rc = 0;

	mutex_lock(&cper_esource_pool_lock);
	if (!cper_esource_pool_ready) {
		rc = ghes_estatus_pool_init(1);
		if (!rc)
			cper_esource_pool_ready = true;
	}
	mutex_unlock(&cper_esource_pool_lock);

	return rc;
}

static size_t cper_esource_estatus_len(struct acpi_hest_generic_status *estatus)
{
	if (estatus->raw_data_length)
		return (size_t)estatus->raw_data_offset +
		       (size_t)estatus->raw_data_length;
	else
		return sizeof(*estatus) + (size_t)estatus->data_length;
}

static int cper_esource_validate_status(struct cper_esource *ctx)
{
	size_t estatus_len;

	if (!ctx->estatus->block_status)
		return -ENOENT;

	if (ctx->estatus->data_length >
	    ctx->status_len - sizeof(*ctx->estatus))
		return -EINVAL;

	if (cper_estatus_check_header(ctx->estatus))
		return -EINVAL;

	if (ctx->estatus->raw_data_length &&
	    (ctx->estatus->raw_data_offset > ctx->status_len ||
	     ctx->estatus->raw_data_length >
	     ctx->status_len - ctx->estatus->raw_data_offset))
		return -EINVAL;

	estatus_len = cper_esource_estatus_len(ctx->estatus);
	if (estatus_len < sizeof(*ctx->estatus) || estatus_len > ctx->status_len)
		return -EINVAL;

	if (cper_estatus_check(ctx->estatus))
		return -EINVAL;

	return 0;
}

static void cper_esource_ack(struct cper_esource *ctx)
{
	if (!ctx->ack.present)
		return;

	if (ctx->ack.width == 64) {
		u64 *addr = ctx->ack.addr;
		u64 val = READ_ONCE(*addr);

		/* Publish status-buffer updates before raising the ack bit. */
		wmb();
		val &= ctx->ack.preserve;
		val |= ctx->ack.set;
		WRITE_ONCE(*addr, val);
	} else {
		u32 *addr = ctx->ack.addr;
		u32 val = READ_ONCE(*addr);

		/* Publish status-buffer updates before raising the ack bit. */
		wmb();
		val &= (u32)ctx->ack.preserve;
		val |= (u32)ctx->ack.set;
		WRITE_ONCE(*addr, val);
	}
}

static void cper_esource_clear_status(struct cper_esource *ctx)
{
	ctx->estatus->block_status = 0;
	WRITE_ONCE(((struct acpi_hest_generic_status *)ctx->status)->block_status, 0);
}

static void cper_esource_fatal(struct cper_esource *ctx)
{
	__ghes_print_estatus(KERN_EMERG, &ctx->generic, ctx->estatus);
	add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);
	panic("GHES: fatal firmware-first CPER record from %s\n",
	      dev_name(ctx->dev));
}

static irqreturn_t cper_esource_process(struct cper_esource *ctx)
{
	int rc;
	int sev;

	memcpy(ctx->estatus, ctx->status, ctx->status_len);

	rc = cper_esource_validate_status(ctx);
	if (rc == -ENOENT)
		return IRQ_NONE;
	if (rc) {
		dev_warn_ratelimited(ctx->dev, FW_WARN GHES_PFX
				     "Invalid error status block\n");
		cper_esource_clear_status(ctx);
		cper_esource_ack(ctx);
		return IRQ_HANDLED;
	}

	sev = ghes_severity(ctx->estatus->error_severity);
	if (sev >= GHES_SEV_PANIC)
		cper_esource_fatal(ctx);

	ghes_print_estatus(NULL, &ctx->generic, ctx->estatus);

	ghes_cper_handle_status(ctx->dev, &ctx->generic, ctx->estatus, false);
	cper_esource_clear_status(ctx);
	cper_esource_ack(ctx);

	return IRQ_HANDLED;
}

static irqreturn_t cper_esource_irq(int irq, void *data)
{
	struct cper_esource *ctx = data;

	return cper_esource_process(ctx);
}

static int cper_esource_init_ack(struct cper_esource *ctx)
{
	struct device *dev = ctx->dev;
	size_t size;

	ctx->ack.addr = cper_esource_map_region(dev, 1, &size);
	if (IS_ERR(ctx->ack.addr))
		return PTR_ERR(ctx->ack.addr);

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
	size_t size;
	int source_id;
	int rc;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;

	ctx->status = cper_esource_map_region(dev, 0, &size);
	if (IS_ERR(ctx->status))
		return PTR_ERR(ctx->status);

	ctx->status_len = size;
	if (ctx->status_len < sizeof(*ctx->estatus))
		return dev_err_probe(dev, -EINVAL,
				     "status region is smaller than a CPER header\n");

	rc = cper_esource_init_ack(ctx);
	if (rc)
		return rc;

	rc = cper_esource_init_pool();
	if (rc)
		return rc;

	ctx->estatus = devm_kzalloc(dev, ctx->status_len, GFP_KERNEL);
	if (!ctx->estatus)
		return -ENOMEM;

	/* Keep source_id 0 unused so a zeroed header is never treated as valid. */
	source_id = ida_alloc_min(&cper_esource_source_ids, 1, GFP_KERNEL);
	if (source_id < 0)
		return source_id;
	if (source_id > U16_MAX) {
		ida_free(&cper_esource_source_ids, source_id);
		return -ENOSPC;
	}

	ctx->generic.header.type = ACPI_HEST_TYPE_GENERIC_ERROR;
	ctx->generic.header.source_id = source_id;

	rc = devm_add_action_or_reset(dev, cper_esource_release_source_id,
				      ctx);
	if (rc)
		return rc;

	ctx->generic.notify.type = ACPI_HEST_NOTIFY_EXTERNAL;
	ctx->generic.error_block_length = ctx->status_len;

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
