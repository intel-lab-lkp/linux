// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Platform Performance Measuring Unit (PPMU) driver
 *
 * Copyright (c) 2024-25 Samsung Electronics Co., Ltd.
 *
 * Authors: Vivek Yadav <vivek.2311@samsung.com>
 *          Ravi Patel <ravi.patel@samsung.com>
 */

#include <linux/idr.h>
#include <linux/of.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include "samsung_ppmu.h"

#define PPMU_CLEAR_FLAG				(GENMASK(3, 0) | BIT(31))

#define PPMU_PMCNT3_HIGH_VAL			(3)
#define PPMU_PMCNT2_HIGH_VAL			(2)
#define PPMU_RESET_CCNT				BIT(2)
#define PPMU_RESET_PMCNT			BIT(1)

#define PPMU_PMNC_OP_MODE_MASK			(GENMASK(21, 20))
#define PPMU_PMNC_OP_MODE_OFF			(20)
#define PPMU_MANUAL_MODE_VAL			(0x0)
#define PPMU_PMNC_GLB_CNT_EN_VAL		(BIT(0))
#define PPMU_PMNC_RESET_PMCNT_VAL		(BIT(1))
#define PPMU_PMNC_RESET_CCNT_VAL		(BIT(2))

#define PPMU_V24_IDENTIFIER			(0x45)

#define PPMU_CCNT_IDX				(4)
#define PPMU_CCNT_POS_OFF			(31)
#define PPMU_VERSION_CHECK			(GENMASK(19, 12))

#define PPMU_SM_ENABLE_ALL_CNT			(0xf)
#define PPMU_ENABLE_CCNT			BIT(31)
#define PPMU_FILTER_MASK			(0x7)

/* ID of event type */
enum {
	PPMU_EVENT_READ_CHANNEL_ACTIVE		= (0x00),
	PPMU_EVENT_WRITE_CHANNEL_ACTIVE		= (0x01),
	PPMU_EVENT_READ_REQUEST			= (0x02),
	PPMU_EVENT_WRITE_REQUEST		= (0x03),
	PPMU_EVENT_READ_DATA			= (0x04),
	PPMU_EVENT_WRITE_DATA			= (0x05),
	PPMU_EVENT_WRITE_RESPONSE		= (0x06),
	PPMU_EVENT_LAST_READ_DATA		= (0x07),
	PPMU_EVENT_LAST_WRITE_DATA		= (0x08),
	PPMU_EVENT_READ_REQUEST_BOLCKING	= (0x10),
	PPMU_EVENT_WRITE_REQUEST_BOLCKING	= (0x11),
	PPMU_EVENT_READ_DATA_BLOCKING		= (0x12),
	PPMU_EVENT_WRITE_DATA_BLOCKING		= (0x13),
	PPMU_EVENT_WRITE_RESPONSE_BLOCKING	= (0x14),
	PPMU_EVENT_READ_BURST_LENGTH		= (0x2a),
	PPMU_EVENT_WRITE_BURST_LENGTH		= (0x2b),
	PPMU_EVENT_CCNT				= (0xfe),
	PPMU_EVENT_MAX				= (0xff),
};

/* Register offsets */
enum ppmu_reg {
	PPMU_VERSION				= (0x0000),
	PPMU_PMNC				= (0x0004),
	PPMU_CNTENS				= (0x0008),
	PPMU_CNTENC				= (0x000c),
	PPMU_INTENS				= (0x0010),
	PPMU_INTENC				= (0x0014),
	PPMU_FLAG				= (0x0018),
	PPMU_CIG_CFG0				= (0x001c),
	PPMU_CIG_CFG1				= (0x0020),
	PPMU_CIG_CFG2				= (0x0024),
	PPMU_CIG_RESULT				= (0x0028),
	PPMU_CNT_RESET				= (0x002c),
	PPMU_CNT_AUTO				= (0x0030),
	PPMU_PMCNT0				= (0x0034),
	PPMU_PMCNT1				= (0x0038),
	PPMU_PMCNT2				= (0x003c),
	PPMU_PMCNT3_LOW				= (0x0040),
	PPMU_PMCNT3_HIGH			= (0x0044),
	PPMU_CCNT				= (0x0048),
	PPMU_PMCNT2_HIGH			= (0x0054),
	PPMU_CONFIG_INFO			= (0X005c),
	PPMU_INTERRUPT_TEST			= (0x0060),
	PPMU_EVENT_EV0_TYPE			= (0x0200),
	PPMU_EVENT_EV1_TYPE			= (0x0204),
	PPMU_EVENT_EV2_TYPE			= (0x0208),
	PPMU_EVENT_EV3_TYPE			= (0x020c),
	PPMU_EVENT_SM_ID_MASK			= (0x0220),
	PPMU_EVENT_SM_ID_VALUE			= (0x0224),
	PPMU_EVENT_SM_ID_A			= (0x0228),
	PPMU_EVENT_SM_OTHERS_V			= (0x022c),
	PPMU_EVENT_SM_OTHERS_A			= (0x0230),
	PPMU_EVENT_SAMPLE_RD_ID_MASK		= (0x0234),
	PPMU_EVENT_SAMPLE_RD_ID_VALUE		= (0x0238),
	PPMU_EVENT_SAMPLE_WR_ID_MASK		= (0x023c),
	PPMU_EVENT_SAMPLE_WD_ID_VALUE		= (0x0240),
	PPMU_EVENT_AXI_CH_TYPE			= (0x0244),
	PPMU_EVENT_MO_INFO			= (0x0250),
	PPMU_EVENT_MO_INFO_SM_ID		= (0x0254),
	PPMU_EVENT_MO_INFO_SAMPLE		= (0x0258),
	PPMU_EVENT_IMPRECISE_ERR		= (0x0260),
};

static const u32 ppmu_pmcnt_offset[PPMU_MAX_COUNTERS][2] = {
	{ PPMU_PMCNT0,		PPMU_EVENT_EV0_TYPE },
	{ PPMU_PMCNT1,		PPMU_EVENT_EV1_TYPE },
	{ PPMU_PMCNT2,		PPMU_EVENT_EV2_TYPE },
	{ PPMU_PMCNT3_LOW,	PPMU_EVENT_EV3_TYPE },
	{ PPMU_CCNT,		0 },
};

static DEFINE_IDR(my_idr);

static void samsung_ppmu_write_evtype(struct samsung_ppmu *s_ppmu,
				      int idx, u32 type)
{
	/* Configure event */
	if (idx != PPMU_CCNT_IDX)
		writel(type, s_ppmu->base + ppmu_pmcnt_offset[idx][1]);
}

static int samsung_ppmu_get_event_idx(struct perf_event *event)
{
	struct samsung_ppmu *samsung_ppmu = to_samsung_ppmu(event->pmu);
	unsigned long *used_mask = samsung_ppmu->pmu_events.used_mask;
	u32 num_counters = samsung_ppmu->num_counters;
	DECLARE_BITMAP(buf, PPMU_MAX_COUNTERS);
	int idx;

	if (event->attr.config == PPMU_EVENT_CCNT)
		*buf = *used_mask | (~BIT(PPMU_CCNT_IDX));
	else
		*buf = *used_mask;

	idx = find_first_zero_bit(buf, num_counters);
	if (idx == num_counters)
		return -EAGAIN;

	set_bit(idx, used_mask);

	return idx;
}

static u64 samsung_ppmu_get_read_counter(struct samsung_ppmu *s_ppmu,
					 struct hw_perf_event *hwc)
{
	u64 counter_val, prev_overflow_val;
	u64 high_val = 0;

	counter_val = (u64)readl(s_ppmu->base + ppmu_pmcnt_offset[hwc->idx][0]);
	prev_overflow_val = (u64)s_ppmu->counter_overflow[hwc->idx] << 32;

	/*
	 * PMCNT2 and PMCNT3 are 40-bit counters
	 * its value are divided in two 32-bit registers
	 */
	if (hwc->idx == PPMU_PMCNT3_HIGH_VAL || hwc->idx == PPMU_PMCNT2_HIGH_VAL) {
		prev_overflow_val = prev_overflow_val << 8;

		if (hwc->idx == PPMU_PMCNT3_HIGH_VAL)
			high_val = (u64)(readl(s_ppmu->base + PPMU_PMCNT3_HIGH) & 0xFF);
		else
			high_val = (u64)(readl(s_ppmu->base + PPMU_PMCNT2_HIGH) & 0xFF);
	}

	counter_val = counter_val + (high_val << 32);

	/* Clear overflow status */
	s_ppmu->counter_overflow[hwc->idx] = 0;

	/* Read previous counter */
	s_ppmu->prev_counter[hwc->idx] = counter_val;

	return (counter_val | prev_overflow_val);
}

static void samsung_ppmu_get_enable_counter(struct samsung_ppmu *s_ppmu,
					    struct hw_perf_event *hwc)
{
	u32 val;
	int idx = hwc->idx;

	if (idx == PPMU_CCNT_IDX)
		idx = PPMU_CCNT_POS_OFF;

	/* Enabling counters */
	writel(BIT(idx), s_ppmu->base + PPMU_CNTENS);

	/* Enabling interrupt */
	writel(BIT(idx), s_ppmu->base + PPMU_INTENS);

	/* Manual mode enabled */
	val = readl(s_ppmu->base + PPMU_PMNC);
	val &= (~PPMU_PMNC_OP_MODE_MASK);
	val |= (PPMU_MANUAL_MODE_VAL << PPMU_PMNC_OP_MODE_OFF);
	writel(val, s_ppmu->base + PPMU_PMNC);
}

static void samsung_ppmu_get_disable_counter(struct samsung_ppmu *s_ppmu,
					     struct hw_perf_event *hwc)
{
	int idx = hwc->idx;

	if (idx == PPMU_CCNT_IDX)
		idx = PPMU_CCNT_POS_OFF;

	/* Disabling interrupt */
	writel(BIT(idx), s_ppmu->base + PPMU_INTENC);

	/* Disabling counter */
	writel(BIT(idx), s_ppmu->base + PPMU_CNTENC);

	/* Reset counter */
	writel(BIT(idx), s_ppmu->base + PPMU_CNT_RESET);
}

static void samsung_ppmu_get_start_counters(struct samsung_ppmu *s_ppmu)
{
	u32 val;

	/* Clearing all overflow status */
	writel(PPMU_CLEAR_FLAG, s_ppmu->base + PPMU_FLAG);

	/* Resetting all counters */
	val = readl(s_ppmu->base + PPMU_PMNC);
	val |= (PPMU_PMNC_RESET_PMCNT_VAL | PPMU_PMNC_RESET_CCNT_VAL);
	writel(val, s_ppmu->base + PPMU_PMNC);

	/* Start counters */
	val = readl(s_ppmu->base + PPMU_PMNC);
	val |= PPMU_PMNC_GLB_CNT_EN_VAL;
	writel(val, s_ppmu->base + PPMU_PMNC);

	s_ppmu->status = PPMU_START;
}

static void samsung_ppmu_get_stop_counters(struct samsung_ppmu *s_ppmu)
{
	u32 val;

	/* Stop counters */
	val = readl(s_ppmu->base + PPMU_PMNC);
	val &= (~PPMU_PMNC_GLB_CNT_EN_VAL);
	writel(val, s_ppmu->base + PPMU_PMNC);

	s_ppmu->status = PPMU_STOP;
}

static u32 samsung_ppmu_get_int_status(struct samsung_ppmu *s_ppmu)
{
	u32 regvalue, i;

	/* Read status register */
	regvalue = readl(s_ppmu->base + PPMU_FLAG);

	for (i = 0; i < PPMU_MAX_COUNTERS; i++) {
		if (regvalue & BIT(i))
			s_ppmu->counter_overflow[i] += 1;
	}

	if (regvalue & BIT(PPMU_CCNT_POS_OFF)) {
		s_ppmu->counter_overflow[PPMU_CCNT_IDX] += 1;
		regvalue &= (~BIT(PPMU_CCNT_POS_OFF));
		regvalue |= BIT(PPMU_CCNT_IDX);
	}

	return regvalue;
}

static void samsung_ppmu_clear_int_status(struct samsung_ppmu *s_ppmu, int idx)
{
	if (idx == PPMU_CCNT_IDX)
		idx = PPMU_CCNT_POS_OFF;

	/* Clear the interrupt status */
	writel(BIT(idx), s_ppmu->base + PPMU_FLAG);
}

static struct attribute *samsung_ppmu_format_attr[] = {
	SAMSUNG_PPMU_FORMAT_ATTR(event, "config:0-7"),
	NULL
};

static const struct attribute_group samsung_ppmu_format_group = {
	.name = "format",
	.attrs = samsung_ppmu_format_attr,
};

static struct attribute *samsung_ppmu_events_attr[] = {
	SAMSUNG_PPMU_EVENT_ATTR(rd_channel_active,	PPMU_EVENT_READ_CHANNEL_ACTIVE),
	SAMSUNG_PPMU_EVENT_ATTR(wr_channel_active,	PPMU_EVENT_WRITE_CHANNEL_ACTIVE),
	SAMSUNG_PPMU_EVENT_ATTR(read_request,		PPMU_EVENT_READ_REQUEST),
	SAMSUNG_PPMU_EVENT_ATTR(write_request,		PPMU_EVENT_WRITE_REQUEST),
	SAMSUNG_PPMU_EVENT_ATTR(read_data,		PPMU_EVENT_READ_DATA),
	SAMSUNG_PPMU_EVENT_ATTR(write_data,		PPMU_EVENT_WRITE_DATA),
	SAMSUNG_PPMU_EVENT_ATTR(wr_response,		PPMU_EVENT_WRITE_RESPONSE),
	SAMSUNG_PPMU_EVENT_ATTR(last_rd_data,		PPMU_EVENT_LAST_READ_DATA),
	SAMSUNG_PPMU_EVENT_ATTR(last_wr_data,		PPMU_EVENT_LAST_WRITE_DATA),
	SAMSUNG_PPMU_EVENT_ATTR(rd_request_blk,		PPMU_EVENT_READ_REQUEST_BOLCKING),
	SAMSUNG_PPMU_EVENT_ATTR(wr_request_blk,		PPMU_EVENT_WRITE_REQUEST_BOLCKING),
	SAMSUNG_PPMU_EVENT_ATTR(rd_data_blk,		PPMU_EVENT_READ_DATA_BLOCKING),
	SAMSUNG_PPMU_EVENT_ATTR(wr_data_blk,		PPMU_EVENT_WRITE_DATA_BLOCKING),
	SAMSUNG_PPMU_EVENT_ATTR(wr_response_blk,	PPMU_EVENT_WRITE_RESPONSE_BLOCKING),
	SAMSUNG_PPMU_EVENT_ATTR(rd_burst_length,	PPMU_EVENT_READ_BURST_LENGTH),
	SAMSUNG_PPMU_EVENT_ATTR(wr_burst_length,	PPMU_EVENT_WRITE_BURST_LENGTH),
	SAMSUNG_PPMU_EVENT_ATTR(ccnt,			PPMU_EVENT_CCNT),
	NULL
};

static const struct attribute_group samsung_ppmu_events_group = {
	.name = "events",
	.attrs = samsung_ppmu_events_attr,
};

struct device_attribute dev_attr_cpumask =
	__ATTR(cpumask, 0444, samsung_ppmu_cpumask_sysfs_show, NULL);

static struct attribute *samsung_ppmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group samsung_ppmu_cpumask_attr_group = {
	.attrs = samsung_ppmu_cpumask_attrs,
};

static struct device_attribute samsung_ppmu_identifier_attr =
	__ATTR(identifier, 0444, samsung_ppmu_identifier_attr_show, NULL);

static struct attribute *samsung_ppmu_identifier_attrs[] = {
	&samsung_ppmu_identifier_attr.attr,
	NULL
};

static const struct attribute_group samsung_ppmu_identifier_group = {
	.attrs = samsung_ppmu_identifier_attrs,
};

static const struct attribute_group *samsung_ppmu_v24_attr_groups[] = {
	&samsung_ppmu_format_group,
	&samsung_ppmu_events_group,
	&samsung_ppmu_cpumask_attr_group,
	&samsung_ppmu_identifier_group,
	NULL
};

static struct samsung_ppmu_drv_data samsung_ppmu_v24_data = {
	.ppmu_attr_group = samsung_ppmu_v24_attr_groups
};

static const struct samsung_ppmu_ops samsung_ppmu_plat_ops = {
	.write_evtype		= samsung_ppmu_write_evtype,
	.get_event_idx		= samsung_ppmu_get_event_idx,
	.read_counter		= samsung_ppmu_get_read_counter,
	.enable_counter		= samsung_ppmu_get_enable_counter,
	.disable_counter	= samsung_ppmu_get_disable_counter,
	.start_counters		= samsung_ppmu_get_start_counters,
	.stop_counters		= samsung_ppmu_get_stop_counters,
	.get_int_status		= samsung_ppmu_get_int_status,
	.clear_int_status	= samsung_ppmu_clear_int_status,
};

static int ppmu_clock_init(struct samsung_ppmu *s_ppmu)
{
	int ret = 0;
	struct device *dev = s_ppmu->dev;

	s_ppmu->clks[PPMU_ACLK].id = "aclk";
	s_ppmu->clks[PPMU_PCLK].id = "pclk";

	ret = devm_clk_bulk_get(dev, PPMU_CLK_COUNT, s_ppmu->clks);
	if (ret) {
		dev_err(dev, "Failed to get clocks. Err %d\n", ret);
		return ret;
	}

	ret = clk_bulk_prepare_enable(PPMU_CLK_COUNT, s_ppmu->clks);
	if (ret)
		dev_err(dev, "Clock enable failed. Err %d\n", ret);

	return ret;
}

static int samsung_ppmu_probe(struct platform_device *pdev)
{
	struct samsung_ppmu *s_ppmu;
	struct device *dev = &pdev->dev;
	u32 version;
	char *name;
	int ret;

	s_ppmu = devm_kzalloc(dev, sizeof(*s_ppmu), GFP_KERNEL);
	if (!s_ppmu)
		return -ENOMEM;

	s_ppmu->dev = &pdev->dev;

	s_ppmu->id = idr_alloc(&my_idr, dev, 0, 2, GFP_KERNEL);
	if (s_ppmu->id < 0) {
		dev_err(dev, "Failed to allocate ID dynamically\n");
		return s_ppmu->id;
	}

	/* Register base address */
	s_ppmu->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s_ppmu->base)) {
		dev_err(dev, "IO remap failed\n");
		return PTR_ERR(s_ppmu->base);
	}

	/* Register IRQ */
	ret = samsung_ppmu_init_irq(s_ppmu, pdev);
	if (ret)
		return ret;

	s_ppmu->check_event = PPMU_EVENT_MAX;
	s_ppmu->num_counters = PPMU_MAX_COUNTERS;
	s_ppmu->on_cpu = 0;
	s_ppmu->identifier = PPMU_V24_IDENTIFIER;

	s_ppmu->ppmu_data = device_get_match_data(&pdev->dev);
	if (!s_ppmu->ppmu_data) {
		dev_err(&pdev->dev, "No matching device data found\n");
		return -ENODEV;
	}

	/* Register PPMU driver ops */
	s_ppmu->pmu_events.attr_groups = s_ppmu->ppmu_data->ppmu_attr_group;
	s_ppmu->ops = &samsung_ppmu_plat_ops;

	/* Set private data to platform_device structure */
	platform_set_drvdata(pdev, s_ppmu);

	/* Initialize the PPMU */
	samsung_ppmu_init(s_ppmu, THIS_MODULE);

	ret = ppmu_clock_init(s_ppmu);
	if (ret)
		return ret;

	version = readl(s_ppmu->base + PPMU_VERSION);
	version &= PPMU_VERSION_CHECK;
	version >>= 12;
	s_ppmu->samsung_ppmu_version = version;

	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "ppmu_v_%x_%d", version, s_ppmu->id);
	if (!name)
		return -ENOMEM;

	ret = perf_pmu_register(&s_ppmu->pmu, name, -1);
	if (ret) {
		clk_bulk_disable_unprepare(PPMU_CLK_COUNT, s_ppmu->clks);
		dev_err(dev, "Failed to register PPMU in perf. Err %d\n", ret);
	}

	return ret;
}

static void samsung_ppmu_remove(struct platform_device *pdev)
{
	struct samsung_ppmu *s_ppmu = platform_get_drvdata(pdev);

	clk_bulk_disable_unprepare(PPMU_CLK_COUNT, s_ppmu->clks);

	perf_pmu_unregister(&s_ppmu->pmu);

	idr_remove(&my_idr, s_ppmu->id);
}

static const struct of_device_id ppmu_of_match[] = {
	{ .compatible = "samsung,ppmu-v2", .data = &samsung_ppmu_v24_data },
	{}
};
MODULE_DEVICE_TABLE(of, ppmu_of_match);

static struct platform_driver samsung_ppmu_driver = {
	.probe = samsung_ppmu_probe,
	.remove = samsung_ppmu_remove,
	.driver	= {
		.name = "samsung-ppmu",
		.of_match_table	= ppmu_of_match,
	},
};

module_platform_driver(samsung_ppmu_driver);

MODULE_ALIAS("perf:samsung-ppmu");
MODULE_DESCRIPTION("Samsung Platform Performance Measuring Unit (PPMU) driver");
MODULE_AUTHOR("Vivek Yadav <vivek.2311@samsung.com>");
MODULE_AUTHOR("Ravi Patel <ravi.patel@samsung.com>");
MODULE_LICENSE("GPL");
