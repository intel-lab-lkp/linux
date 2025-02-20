// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/amba/bus.h>
#include <linux/io.h>
#include <linux/coresight.h>
#include <linux/of.h>

#include "coresight-priv.h"
#include "coresight-tnoc.h"
#include "coresight-trace-id.h"

static void trace_noc_enable_hw(struct trace_noc_drvdata *drvdata)
{
	u32 val;

	/* Set ATID */
	writel_relaxed(drvdata->atid, drvdata->base + TRACE_NOC_XLD);

	/* Config sync CR */
	writel_relaxed(0xffff, drvdata->base + TRACE_NOC_SYNCR);

	/* Set frequency value */
	writel_relaxed(drvdata->freq_req_val, drvdata->base + TRACE_NOC_FREQVAL);

	/* Set Ctrl register */
	val = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);

	if (drvdata->flag_type == FLAG_TS)
		val = val | TRACE_NOC_CTRL_FLAGTYPE;
	else
		val = val & ~TRACE_NOC_CTRL_FLAGTYPE;

	if (drvdata->freq_type == FREQ_TS)
		val = val | TRACE_NOC_CTRL_FREQTYPE;
	else
		val = val & ~TRACE_NOC_CTRL_FREQTYPE;

	val = val | TRACE_NOC_CTRL_PORTEN;
	writel_relaxed(val, drvdata->base + TRACE_NOC_CTRL);

	dev_dbg(drvdata->dev, "Trace NOC is enabled\n");
}

static int trace_noc_enable(struct coresight_device *csdev, struct coresight_connection *inport,
			    struct coresight_connection *outport)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock(&drvdata->spinlock);
	if (csdev->refcnt == 0)
		trace_noc_enable_hw(drvdata);

	csdev->refcnt++;
	spin_unlock(&drvdata->spinlock);

	return 0;
}

static void trace_noc_disable_hw(struct trace_noc_drvdata *drvdata)
{
	writel_relaxed(0x0, drvdata->base + TRACE_NOC_CTRL);
	dev_dbg(drvdata->dev, "Trace NOC is disabled\n");
}

static void trace_noc_disable(struct coresight_device *csdev, struct coresight_connection *inport,
			      struct coresight_connection *outport)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock(&drvdata->spinlock);
	if (--csdev->refcnt == 0)
		trace_noc_disable_hw(drvdata);

	spin_unlock(&drvdata->spinlock);
	dev_info(drvdata->dev, "Trace NOC is disabled\n");
}

static const struct coresight_ops_link trace_noc_link_ops = {
	.enable		= trace_noc_enable,
	.disable	= trace_noc_disable,
};

static const struct coresight_ops trace_noc_cs_ops = {
	.link_ops	= &trace_noc_link_ops,
};

static int trace_noc_init_default_data(struct trace_noc_drvdata *drvdata)
{
	int atid;

	atid = coresight_trace_id_get_system_id();
	if (atid < 0)
		return atid;

	drvdata->atid = atid;

	drvdata->freq_type = FREQ_TS;
	drvdata->flag_type = FLAG;
	drvdata->freq_req_val = 0;

	return 0;
}

static int trace_noc_probe(struct amba_device *adev, const struct amba_id *id)
{
	struct device *dev = &adev->dev;
	struct coresight_platform_data *pdata;
	struct trace_noc_drvdata *drvdata;
	struct coresight_desc desc = { 0 };
	int ret;

	desc.name = coresight_alloc_device_name(&trace_noc_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	adev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &adev->dev;
	dev_set_drvdata(dev, drvdata);

	drvdata->base = devm_ioremap_resource(dev, &adev->res);
	if (!drvdata->base)
		return -ENOMEM;

	spin_lock_init(&drvdata->spinlock);

	ret = trace_noc_init_default_data(drvdata);
	if (ret)
		return ret;

	desc.ops = &trace_noc_cs_ops;
	desc.type = CORESIGHT_DEV_TYPE_LINK;
	desc.subtype.link_subtype = CORESIGHT_DEV_SUBTYPE_LINK_MERG;
	desc.pdata = adev->dev.platform_data;
	desc.dev = &adev->dev;
	desc.access = CSDEV_ACCESS_IOMEM(drvdata->base);
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev))
		return PTR_ERR(drvdata->csdev);

	pm_runtime_put(&adev->dev);

	dev_dbg(drvdata->dev, "Trace Noc initialized\n");
	return 0;
}

static void trace_noc_remove(struct amba_device *adev)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(&adev->dev);

	coresight_trace_id_put_system_id(drvdata->atid);
	coresight_unregister(drvdata->csdev);
}

static struct amba_id trace_noc_ids[] = {
	{
		.id     = 0x000f0c00,
		.mask   = 0x000fff00,
	},
	{},
};
MODULE_DEVICE_TABLE(amba, trace_noc_ids);

static struct amba_driver trace_noc_driver = {
	.drv = {
		.name   = "coresight-trace-noc",
		.owner	= THIS_MODULE,
		.suppress_bind_attrs = true,
	},
	.probe          = trace_noc_probe,
	.remove		= trace_noc_remove,
	.id_table	= trace_noc_ids,
};

module_amba_driver(trace_noc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Trace NOC driver");
