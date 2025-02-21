// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
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

#include "coresight-ctcu.h"
#include "coresight-priv.h"

DEFINE_CORESIGHT_DEVLIST(ctcu_devs, "ctcu");

#define ctcu_writel(drvdata, val, offset)	__raw_writel((val), drvdata->base + offset)
#define ctcu_readl(drvdata, offset)		__raw_readl(drvdata->base + offset)

/*
 * The TMC Coresight Control Unit uses four ATID registers to control the data
 * filter function based on the trace ID for each TMC ETR sink. The length of
 * each ATID register is 32 bits. Therefore, the ETR has a related field in
 * CTCU that is 128 bits long. Each trace ID is represented by one bit in that
 * filed.
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
 */
#define CTCU_ATID_REG_OFFSET(traceid, atid_offset) \
		((traceid / 32) * 4 + atid_offset)

#define CTCU_ATID_REG_BIT(traceid)	(traceid % 32)
#define CTCU_ATID_REG_SIZE		0x10

struct ctcu_atid_config {
	const u32 atid_offset;
	const u32 port_num;
};

struct ctcu_config {
	const struct ctcu_atid_config *atid_config;
	int num_atid_config;
};

static const struct ctcu_atid_config sa8775p_atid_cfgs[] = {
	{0xf8,  0},
	{0x108, 1},
};

static const struct ctcu_config sa8775p_cfgs = {
	.atid_config		= sa8775p_atid_cfgs,
	.num_atid_config	= ARRAY_SIZE(sa8775p_atid_cfgs),
};

static void ctcu_program_atid_register(struct ctcu_drvdata *drvdata, u32 reg_offset,
				       u8 bit, bool enable)
{
	u32 val;

	CS_UNLOCK(drvdata->base);
	val = ctcu_readl(drvdata, reg_offset);
	val = enable? (val | BIT(bit)) : (val & ~BIT(bit));
	ctcu_writel(drvdata, val, reg_offset);
	CS_LOCK(drvdata->base);
}

/*
 * __ctcu_set_etr_traceid: Set bit in the ATID register based on trace ID when enable is true.
 * Reset the bit of the ATID register based on trace ID when enable is false.
 *
 * @csdev:	coresight_device struct related to the device
 * @traceid:	trace ID of the source tracer.
 * @port_num:	port number from TMC ETR sink.
 * @enable:	True for set bit and false for reset bit.
 *
 * Returns 0 indicates success. Non-zero result means failure.
 */
static int __ctcu_set_etr_traceid(struct coresight_device *csdev, u8 traceid, int port_num,
				  bool enable)
{
	struct ctcu_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	u32 atid_offset, reg_offset;
	u8 refcnt, bit;

	atid_offset = drvdata->atid_offset[port_num];
	if (atid_offset == 0)
		return -EINVAL;

	bit = CTCU_ATID_REG_BIT(traceid);
	reg_offset = CTCU_ATID_REG_OFFSET(traceid, atid_offset);
	if (reg_offset - atid_offset > CTCU_ATID_REG_SIZE)
		return -EINVAL;

	guard(raw_spinlock_irqsave)(&drvdata->spin_lock);
	refcnt = drvdata->traceid_refcnt[port_num][traceid];
	/* Only program the atid register when the refcnt value is 0 or 1 */
	if (enable && (++refcnt == 1))
		ctcu_program_atid_register(drvdata, reg_offset, bit, enable);
	else if (!enable && (--refcnt == 0))
		ctcu_program_atid_register(drvdata, reg_offset, bit, enable);

	drvdata->traceid_refcnt[port_num][traceid] = refcnt;

	return 0;
}

static int ctcu_get_active_port(struct coresight_device *sink, struct coresight_device *helper)
{
	int i;

	for (i = 0; i < sink->pdata->nr_outconns; ++i) {
		if (sink->pdata->out_conns[i]->dest_dev)
			return sink->pdata->out_conns[i]->dest_port;
	}

	return -EINVAL;
}

static int ctcu_set_etr_traceid(struct coresight_device *csdev, struct coresight_path *path,
				bool enable)
{
	struct coresight_device *sink = coresight_get_sink(path);
	u8 traceid = path->trace_id;
	int port_num;

	if ((sink == NULL) || !IS_VALID_CS_TRACE_ID(traceid)) {
		dev_err(&csdev->dev, "Invalid parameters\n");
		return -EINVAL;
	}

	port_num = ctcu_get_active_port(sink, csdev);
	if (port_num < 0)
		return -EINVAL;

	dev_dbg(&csdev->dev, "traceid is %d\n", traceid);

	return __ctcu_set_etr_traceid(csdev, traceid, port_num, enable);
}

static int ctcu_enable(struct coresight_device *csdev, enum cs_mode mode,
		       void *data)
{
	struct coresight_path *path = (struct coresight_path *)data;

	return ctcu_set_etr_traceid(csdev, path, true);
}

static int ctcu_disable(struct coresight_device *csdev, void *data)
{
	struct coresight_path *path = (struct coresight_path *)data;

	return ctcu_set_etr_traceid(csdev, path, false);
}

static const struct coresight_ops_helper ctcu_helper_ops = {
	.enable = ctcu_enable,
	.disable = ctcu_disable,
};

static const struct coresight_ops ctcu_ops = {
	.helper_ops = &ctcu_helper_ops,
};

static int ctcu_probe(struct platform_device *pdev)
{
	int i;
	void __iomem *base;
	struct device *dev = &pdev->dev;
	struct coresight_platform_data *pdata;
	struct ctcu_drvdata *drvdata;
	struct coresight_desc desc = { 0 };
	const struct ctcu_config *cfgs;
	const struct ctcu_atid_config *atid_cfg;

	desc.name = coresight_alloc_device_name(&ctcu_devs, dev);
	if (!desc.name)
		return -ENOMEM;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	dev->platform_data = pdata;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (!base)
		return -ENOMEM;

	drvdata->apb_clk = coresight_get_enable_apb_pclk(dev);
	if (IS_ERR(drvdata->apb_clk))
		return -ENODEV;

	cfgs = of_device_get_match_data(dev);
	if (cfgs) {
		if (cfgs->num_atid_config <= ATID_MAX_NUM) {
			for (i = 0; i < cfgs->num_atid_config; i++) {
				atid_cfg = &cfgs->atid_config[i];
				drvdata->atid_offset[i] = atid_cfg->atid_offset;
			}
		}
	}

	drvdata->base = base;
	drvdata->dev = dev;
	platform_set_drvdata(pdev, drvdata);

	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.subtype.helper_subtype = CORESIGHT_DEV_SUBTYPE_HELPER_CTCU;
	desc.pdata = pdata;
	desc.dev = dev;
	desc.ops = &ctcu_ops;

	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		if (!IS_ERR_OR_NULL(drvdata->apb_clk))
			clk_put(drvdata->apb_clk);

		return PTR_ERR(drvdata->csdev);
	}

	return 0;
}

static void ctcu_remove(struct platform_device *pdev)
{
	struct ctcu_drvdata *drvdata = platform_get_drvdata(pdev);

	coresight_unregister(drvdata->csdev);
	if (!IS_ERR_OR_NULL(drvdata->apb_clk))
		clk_put(drvdata->apb_clk);
}

static const struct of_device_id ctcu_match[] = {
	{.compatible = "qcom,sa8775p-ctcu", .data = &sa8775p_cfgs},
	{}
};

static struct platform_driver ctcu_driver = {
	.probe          = ctcu_probe,
	.remove         = ctcu_remove,
	.driver         = {
		.name   = "coresight-ctcu",
		.of_match_table = ctcu_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ctcu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight TMC Control Unit driver");
