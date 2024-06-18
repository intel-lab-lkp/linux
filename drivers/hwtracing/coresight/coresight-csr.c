// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/coresight.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/coresight-stm.h>

#include "coresight-csr.h"
#include "coresight-etm4x.h"
#include "coresight-priv.h"
#include "coresight-tmc.h"
#include "coresight-trace-id.h"
#include "coresight-tpda.h"

#define TPDA_KEY	"tpda"

DEFINE_CORESIGHT_DEVLIST(csr_devs, "csr");

#define csr_writel(drvdata, val, offset)	__raw_writel((val), drvdata->base + offset)
#define csr_readl(drvdata, offset)		__raw_readl(drvdata->base + offset)

/* The Coresight Slave Register uses four ATID registers to control the data filter function based
 * on the trace ID for each TMC ETR sink. The length of each ATID register is 32 bits. Therefore,
 * the ETR has a related field in CSR that is 128 bits long. Each trace ID is represented by one bit in that filed.
 * e.g. ETR0ATID0 layout, set bit 5 for traceid 5
 *                                           bit5
 * ------------------------------------------------------
 * |   |28|   |24|   |20|   |16|   |12|   |8|  1|4|   |0|
 * ------------------------------------------------------
 *
 * e.g. ETR0:
 * 127                     0 from ATID_offset for ETR0ATID0
 * -------------------------
 * |ATID3|ATID2|ATID1|ATID0|
 *
 */
#define CSR_ATID_REG_OFFSET(traceid, atid_offset) \
		((traceid / 32) * 4 + atid_offset)

#define CSR_ATID_REG_BIT(traceid)	(traceid % 32)
#define CSR_ATID_REG_SIZE		0x10

/*
 * __csr_set_etr_traceid: Set bit in the ATID register based on trace id when enable is ture.
 * Reset all bits of the ATID register when enable is false.
 *
 * Returns 0 indicates success. Non-zero result means failure.
 */
static int __csr_set_etr_traceid(struct coresight_device *csdev,
			uint32_t atid_offset, uint32_t traceid,
			bool enable)
{
	struct csr_drvdata *drvdata;
	unsigned long flags;
	uint32_t reg_offset;
	int bit;
	uint32_t val;

	drvdata = dev_get_drvdata(csdev->dev.parent);
	if (IS_ERR_OR_NULL(drvdata))
		return -EINVAL;

	if (((traceid < 0) && (traceid >= CORESIGHT_TRACE_IDS_MAX)) || atid_offset <= 0)
		return -EINVAL;

	spin_lock_irqsave(&drvdata->spin_lock, flags);
	CS_UNLOCK(drvdata->base);

	reg_offset = CSR_ATID_REG_OFFSET(traceid, atid_offset);
	bit = CSR_ATID_REG_BIT(traceid);
	if (reg_offset - atid_offset >= CSR_ATID_REG_SIZE
		|| bit >= CORESIGHT_TRACE_IDS_MAX) {
		CS_LOCK(drvdata);
		spin_unlock_irqrestore(&drvdata->spin_lock, flags);
		return -EINVAL;
	}

	val = csr_readl(drvdata, reg_offset);
	if (enable)
		val = val | BIT(bit);
	else
		val = 0;
	csr_writel(drvdata, val, reg_offset);

	CS_LOCK(drvdata->base);
	spin_unlock_irqrestore(&drvdata->spin_lock, flags);
	return 0;
}

/*
 * of_get_csr_atid_offset: Get the offset of the CSR ATID register for the sink device.
 *
 * Returns the csr atid offset. If the result is less than zero, it means
 * failure.
 */
static int of_get_csr_atid_offset(struct coresight_device *csdev,
				u32 *atid_offset)
{
	return of_property_read_u32(csdev->dev.parent->of_node,
					"qcom,csr-atid-offset", atid_offset);
}

/*
 * csr_set_etr_traceid: Get atid_offset and traceid from TMC ETR's driver data.
 *
 * Returns 0 indicates success. None-zero result means failure.
 */
static int csr_set_etr_traceid(struct coresight_device *csdev, struct coresight_device *sink, bool enable)
{
	int atid_offset, traceid;
	struct tmc_drvdata *etr_drvdata;

	if (!sink)
		return -EINVAL;

	etr_drvdata = dev_get_drvdata(sink->dev.parent);
	traceid = etr_drvdata->traceid;

	if (of_get_csr_atid_offset(sink, &atid_offset))
		return -EINVAL;

	return __csr_set_etr_traceid(csdev, atid_offset, traceid, enable);
}

/** csr_is_tpda_device - Check the current device whether it is a TPDA device or not.
 *  @csdev:	the device structure for current device.
 *
 * Find the traceid of the TPDA device has already fully met the requirement because every
 * TPDM device is connected to the TPDA device.
 */
static bool csr_is_tpda_device(struct coresight_device *csdev)
{
	if (strnstr(dev_name(&csdev->dev), TPDA_KEY, strlen(dev_name(&csdev->dev))))
		return true;

	return false;
}

/** csr_get_traceid - Get trace id from the source that is enabling.
 *  @csdev:	the device structure.
 *
 * Get STM&ETM device's traceid from its driver data.
 * Get TPDM device's traceid from the TPDA device that is connected to the TPDM device.
 *
 * Returns traceid value if found.
 * Returns 0 indicates failure.
 */
int csr_get_traceid(struct coresight_device *csdev)
{
	int i, trace_id = 0;
	u32 type, subtype;
	struct etmv4_drvdata *etmv4_drvdata = NULL;
	struct stm_drvdata *stm_drvdata = NULL;
	struct tpda_drvdata *tpda_drvdata = NULL;

	type = csdev->type;
	subtype = csdev->subtype.source_subtype;

	if ((type == CORESIGHT_DEV_TYPE_SOURCE) && (subtype == CORESIGHT_DEV_SUBTYPE_SOURCE_PROC)) {
		etmv4_drvdata = dev_get_drvdata(csdev->dev.parent);
		trace_id = etmv4_drvdata->trcid;
		return trace_id;

	} else if ((type == CORESIGHT_DEV_TYPE_SOURCE) && subtype == CORESIGHT_DEV_SUBTYPE_SOURCE_SOFTWARE) {
		stm_drvdata = dev_get_drvdata(csdev->dev.parent);
		trace_id = stm_drvdata->traceid;
		return trace_id;

	} else if (csr_is_tpda_device(csdev)) {
		tpda_drvdata = dev_get_drvdata(csdev->dev.parent);
		trace_id = tpda_drvdata->atid;
		return trace_id;
	}

	for (i = 0; i < csdev->pdata->nr_outconns; i++) {
		struct coresight_device *child_dev;

		child_dev = csdev->pdata->out_conns[i]->dest_dev;
		if (child_dev)
			trace_id = csr_get_traceid(child_dev);
		if (trace_id)
			return trace_id;
	}

	return trace_id;
}
EXPORT_SYMBOL_GPL(csr_get_traceid);

static int csr_enable(struct coresight_device *csdev, enum cs_mode mode,
		       void *data)
{
	int ret = 0;
	struct coresight_device *sink_csdev = (struct coresight_device *)data;

	ret = csr_set_etr_traceid(csdev, sink_csdev, true);
	if (ret)
		return -EINVAL;

	return 0;
}

static int csr_disable(struct coresight_device *csdev, void *data)
{
	int ret = 0;
	struct coresight_device *sink_csdev = (struct coresight_device *)data;

	ret = csr_set_etr_traceid(csdev, sink_csdev, false);
	if (ret)
		return -EINVAL;

	return 0;
}

static const struct coresight_ops_helper csr_helper_ops = {
	.enable = csr_enable,
	.disable = csr_disable,
};

static const struct coresight_ops csr_ops = {
	.helper_ops = &csr_helper_ops,
};

static int csr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct coresight_platform_data *pdata;
	struct csr_drvdata *drvdata;
	struct coresight_desc desc = { 0 };
	struct resource *res;

	desc.name = coresight_alloc_device_name(&csr_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	pdev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;
	drvdata->dev = &pdev->dev;
	platform_set_drvdata(pdev, drvdata);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "csr-base");
	if (!res)
		return -ENODEV;
	drvdata->pbase = res->start;

	drvdata->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!drvdata->base)
		return -ENOMEM;

	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.pdata = pdev->dev.platform_data;
	desc.dev = &pdev->dev;
	desc.ops = &csr_ops;

	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev))
		return PTR_ERR(drvdata->csdev);

	dev_dbg(dev, "CSR initialized: %s\n", desc.name);
	return 0;
}

static int csr_remove(struct platform_device *pdev)
{
	struct csr_drvdata *drvdata = platform_get_drvdata(pdev);

	coresight_unregister(drvdata->csdev);
	return 0;
}

static const struct of_device_id csr_match[] = {
	{.compatible = "qcom,coresight-csr"},
	{}
};

static struct platform_driver csr_driver = {
	.probe          = csr_probe,
	.remove         = csr_remove,
	.driver         = {
		.name   = "coresight-csr",
		.of_match_table = csr_match,
		.suppress_bind_attrs = true,
	},
};

static int __init csr_init(void)
{
	return platform_driver_register(&csr_driver);
}
module_init(csr_init);

static void __exit csr_exit(void)
{
	platform_driver_unregister(&csr_driver);
}
module_exit(csr_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight Slave Register driver");
