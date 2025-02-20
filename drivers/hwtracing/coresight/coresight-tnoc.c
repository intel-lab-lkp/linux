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

static ssize_t flush_req_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t size)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	struct coresight_device	*csdev = drvdata->csdev;
	unsigned long val;
	u32 reg;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	if (val != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (csdev->refcnt == 0) {
		spin_unlock(&drvdata->spinlock);
		return -EPERM;
	}

	reg = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);
	reg = reg | TRACE_NOC_CTRL_FLUSHREQ;
	writel_relaxed(reg, drvdata->base + TRACE_NOC_CTRL);

	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_WO(flush_req);

/*
 * flush-sequence status:
 * value 0: sequence in progress;
 * value 1: sequence has been completed.
 */
static ssize_t flush_status_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	struct coresight_device	*csdev = drvdata->csdev;
	u32 val;

	spin_lock(&drvdata->spinlock);
	if (csdev->refcnt == 0) {
		spin_unlock(&drvdata->spinlock);
		return -EPERM;
	}

	val = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);
	spin_unlock(&drvdata->spinlock);
	return sysfs_emit(buf, "%u\n", BMVAL(val, 2, 2));
}
static DEVICE_ATTR_RO(flush_status);

/*
 * Sets the type of issued ATB FLAG packets:
 * 0: 'FLAG' packets;
 * 1: 'FLAG_TS' packets.
 */
static ssize_t flag_type_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t size)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val != 1 && val != 0)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->flag_type = FLAG_TS;
	else
		drvdata->flag_type = FLAG;
	spin_unlock(&drvdata->spinlock);

	return size;
}

static ssize_t flag_type_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%u\n", drvdata->flag_type);
}
static DEVICE_ATTR_RW(flag_type);

static ssize_t freq_type_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%u\n", drvdata->freq_type);
}

static ssize_t freq_type_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t size)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	if (val != 1 && val != 0)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (val)
		drvdata->freq_type = FREQ_TS;
	else
		drvdata->freq_type = FREQ;
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_RW(freq_type);

static ssize_t freq_req_val_show(struct device *dev,
				 struct device_attribute *attr,
				char *buf)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);

	return sysfs_emit(buf, "%u\n", drvdata->freq_req_val);
}

static ssize_t freq_req_val_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t size)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	if (val) {
		spin_lock(&drvdata->spinlock);
		drvdata->freq_req_val = val;
		spin_unlock(&drvdata->spinlock);
	}

	return size;
}
static DEVICE_ATTR_RW(freq_req_val);

static ssize_t freq_ts_req_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,
				 size_t size)
{
	struct trace_noc_drvdata *drvdata = dev_get_drvdata(dev->parent);
	struct coresight_device	*csdev = drvdata->csdev;
	unsigned long val;
	u32 reg;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	if (csdev->refcnt == 0) {
		spin_unlock(&drvdata->spinlock);
		return -EPERM;
	}

	if (val) {
		reg = readl_relaxed(drvdata->base + TRACE_NOC_CTRL);
		reg = reg | TRACE_NOC_CTRL_FREQTSREQ;
		writel_relaxed(reg, drvdata->base + TRACE_NOC_CTRL);
	}
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_WO(freq_ts_req);

static struct attribute *trace_noc_attrs[] = {
	&dev_attr_flush_req.attr,
	&dev_attr_flush_status.attr,
	&dev_attr_flag_type.attr,
	&dev_attr_freq_type.attr,
	&dev_attr_freq_req_val.attr,
	&dev_attr_freq_ts_req.attr,
	NULL,
};

static struct attribute_group trace_noc_attr_grp = {
	.attrs = trace_noc_attrs,
};

static const struct attribute_group *trace_noc_attr_grps[] = {
	&trace_noc_attr_grp,
	NULL,
};

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
	desc.groups = trace_noc_attr_grps;
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
