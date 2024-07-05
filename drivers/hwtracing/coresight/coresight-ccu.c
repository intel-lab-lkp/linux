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

#include "coresight-ccu.h"
#include "coresight-priv.h"
#include "coresight-tmc.h"
#include "coresight-trace-id.h"

DEFINE_CORESIGHT_DEVLIST(ccu_devs, "ccu");

#define ccu_writel(drvdata, val, offset)	__raw_writel((val), drvdata->base + offset)
#define ccu_readl(drvdata, offset)		__raw_readl(drvdata->base + offset)

/* The Coresight Control Unit uses four ATID registers to control the data filter function based
 * on the trace ID for each TMC ETR sink. The length of each ATID register is 32 bits. Therefore,
 * the ETR has a related field in CCU that is 128 bits long. Each trace ID is represented by one bit in that filed.
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
#define CCU_ATID_REG_OFFSET(traceid, atid_offset) \
		((traceid / 32) * 4 + atid_offset)

#define CCU_ATID_REG_BIT(traceid)	(traceid % 32)
#define CCU_ATID_REG_SIZE		0x10

/*
 * __ccu_set_etr_traceid: Set bit in the ATID register based on trace ID when enable is true.
 * Reset the bit of the ATID register based on trace ID when enable is false.
 *
 * @csdev:	coresight_device struct related to the device
 * @traceid:	trace ID of the source tracer.
 * @enable:	True for set bit and false for reset bit.
 *
 * Returns 0 indicates success. Non-zero result means failure.
 */
static int __ccu_set_etr_traceid(struct coresight_device *csdev,
			uint32_t traceid, bool enable)
{
	uint32_t atid_offset = 0;
	struct ccu_drvdata *drvdata;
	unsigned long flags;
	uint32_t reg_offset;
	int bit;
	uint32_t val;

	drvdata = dev_get_drvdata(csdev->dev.parent);
	if (IS_ERR_OR_NULL(drvdata))
		return -EINVAL;

	atid_offset = drvdata->atid_offset;
	if (((traceid < 0) && (traceid >= CORESIGHT_TRACE_IDS_MAX)) || atid_offset <= 0)
		return -EINVAL;

	spin_lock_irqsave(&drvdata->spin_lock, flags);
	CS_UNLOCK(drvdata->base);

	reg_offset = CCU_ATID_REG_OFFSET(traceid, atid_offset);
	bit = CCU_ATID_REG_BIT(traceid);
	if (reg_offset - atid_offset >= CCU_ATID_REG_SIZE
		|| bit >= CORESIGHT_TRACE_IDS_MAX) {
		CS_LOCK(drvdata);
		spin_unlock_irqrestore(&drvdata->spin_lock, flags);
		return -EINVAL;
	}

	val = ccu_readl(drvdata, reg_offset);
	if (enable)
		val = val | BIT(bit);
	else
		val = val & ~BIT(bit);
	ccu_writel(drvdata, val, reg_offset);

	CS_LOCK(drvdata->base);
	spin_unlock_irqrestore(&drvdata->spin_lock, flags);
	return 0;
}

/*
 * ccu_set_atid_offset: Retrieve the offset of the CCU ATID register and store it in the driver data of ETR.
 *
 * Returns 0 indicates success. If the result is less than zero, it means
 * failure.
 */
static int __ccu_set_atid_offset(struct device_node *helper_node, struct coresight_device *helper, int port)
{
	int atid_offset = 0;
	struct device_node *node = helper_node;
	struct device_node *child_node = NULL;
	struct fwnode_handle *child_fwnode = NULL;
	struct ccu_drvdata *drvdata;

	if (!helper_node || !helper)
		return -EINVAL;

	drvdata = dev_get_drvdata(helper->dev.parent);
	if (IS_ERR_OR_NULL(drvdata))
		return -EINVAL;

	child_node = of_get_child_by_name(node, "in-ports");
	if (!child_node)
		return -EINVAL;

	child_fwnode = fwnode_graph_get_endpoint_by_id(&child_node->fwnode, port, 0, 0);
	if (!child_fwnode)
		return -EINVAL;

	fwnode_property_read_u32(child_fwnode, "qcom,ccu-atid-offset", &atid_offset);
	drvdata->atid_offset = atid_offset;
	dev_dbg(&helper->dev, "atid_offset:0x%x\n", atid_offset);

	return 0;
}

static int ccu_set_atid_offset(struct coresight_device *sink, struct coresight_device *helper)
{
	int port, i, ret = 0;
	struct device_node *node;

	for (i = 0; i < sink->pdata->nr_outconns; ++i) {
		if (sink->pdata->out_conns[i]->dest_dev) {
			port = sink->pdata->out_conns[i]->dest_port;
			node = sink->pdata->out_conns[i]->dest_fwnode->dev->of_node;
			ret = __ccu_set_atid_offset(node, helper, port);
			return ret;
		}
	}

	return -EINVAL;
}

/*
 * ccu_set_etr_traceid: Retrieve the ATID offset and trace ID.
 *
 * Returns 0 indicates success. None-zero result means failure.
 */
static int ccu_set_etr_traceid(struct coresight_device *csdev, struct cs_sink_data *sink_data, bool enable)
{
	if ((sink_data->traceid < 0) || (csdev == NULL) || (sink_data->sink == NULL)) {
		dev_dbg(&csdev->dev, "Invalid parameters\n");
		return -EINVAL;
	}

	if (ccu_set_atid_offset(sink_data->sink, csdev))
		return -EINVAL;

	dev_dbg(&csdev->dev, "traceid is %d\n", sink_data->traceid);

	return __ccu_set_etr_traceid(csdev, sink_data->traceid, enable);
}

static int ccu_enable(struct coresight_device *csdev, enum cs_mode mode,
		       void *data)
{
	int ret = 0;
	struct cs_sink_data *sink_data = (struct cs_sink_data *)data;

	ret = ccu_set_etr_traceid(csdev, sink_data, true);
	if (ret)
		dev_dbg(&csdev->dev,"enable data filter failed\n");

	return 0;
}

static int ccu_disable(struct coresight_device *csdev, void *data)
{
	int ret = 0;
	struct cs_sink_data *sink_data = (struct cs_sink_data *)data;

	ret = ccu_set_etr_traceid(csdev, sink_data, false);
	if (ret)
		dev_dbg(&csdev->dev,"disable data filter failed\n");

	return 0;
}

static const struct coresight_ops_helper ccu_helper_ops = {
	.enable = ccu_enable,
	.disable = ccu_disable,
};

static const struct coresight_ops ccu_ops = {
	.helper_ops = &ccu_helper_ops,
};

static int ccu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct coresight_platform_data *pdata;
	struct ccu_drvdata *drvdata;
	struct coresight_desc desc = { 0 };
	struct resource *res;

	desc.name = coresight_alloc_device_name(&ccu_devs, dev);
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
	drvdata->atid_offset = 0;
	platform_set_drvdata(pdev, drvdata);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ccu-base");
	if (!res)
		return -ENODEV;
	drvdata->pbase = res->start;

	drvdata->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!drvdata->base)
		return -ENOMEM;

	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.pdata = pdev->dev.platform_data;
	desc.dev = &pdev->dev;
	desc.ops = &ccu_ops;

	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev))
		return PTR_ERR(drvdata->csdev);

	dev_dbg(dev, "CCU initialized: %s\n", desc.name);
	return 0;
}

static void ccu_remove(struct platform_device *pdev)
{
	struct ccu_drvdata *drvdata = platform_get_drvdata(pdev);

	coresight_unregister(drvdata->csdev);
}

static const struct of_device_id ccu_match[] = {
	{.compatible = "qcom,coresight-ccu"},
	{}
};

static struct platform_driver ccu_driver = {
	.probe          = ccu_probe,
	.remove         = ccu_remove,
	.driver         = {
		.name   = "coresight-ccu",
		.of_match_table = ccu_match,
		.suppress_bind_attrs = true,
	},
};

static int __init ccu_init(void)
{
	return platform_driver_register(&ccu_driver);
}
module_init(ccu_init);

static void __exit ccu_exit(void)
{
	platform_driver_unregister(&ccu_driver);
}
module_exit(ccu_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight Control Unit driver");
