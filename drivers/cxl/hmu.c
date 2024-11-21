// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for CXL Hotness Monitoring unit
 *
 * Based on hisi_ptt.c (Author Yicong Yang <yangyicong@hisilicon.com>)
 * Copyright (c) 2022-2024 HiSilicon Technologies Co., Ltd.
 *
 * TODO:
 * - Add capabilities attributes to help userspace know what can be set.
 * - Find out if timeouts are appropriate for real hardware. Currently
 *   assuming 0.1 seconds is enough for anything.
 */
#include <linux/dev_printk.h>
#include <linux/perf_event.h>
#include <linux/bitfield.h>
#include <linux/spinlock.h>
#include <linux/cleanup.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/math.h>
#include <linux/pci.h>

#include "cxlpci.h"
#include "cxl.h"
#include "hmu.h"

#define CHMU_COMMON_CAP0_REG				0x00
#define   CHMU_COMMON_CAP0_VER_MSK			GENMASK(3, 0)
#define   CHMU_COMMON_CAP0_NUMINST_MSK			GENMASK(15, 8)
#define CHMU_COMMON_CAP1_REG				0x08
#define   CHMU_COMMON_CAP1_INSTLEN_MSK			GENMASK(15, 0)

/* Register offsets within instance */
#define CHMU_INST0_CAP0_REG				0x00
#define   CHMU_INST0_CAP0_MSI_N_MSK			GENMASK(3, 0)
#define   CHMU_INST0_CAP0_OVRFLW_CAP			BIT(4)
#define   CHMU_INST0_CAP0_FILLTHRESH_CAP		BIT(5)
#define   CHMU_INST0_CAP0_EPOCH_TYPE_MSK		GENMASK(7, 6)
#define     CHMU_INST0_CAP0_EPOCH_TYPE_GLOBAL		0
#define     CHMU_INST0_CAP0_EPOCH_TYPE_PERCNT		1
#define   CHMU_INST0_CAP0_TRACK_NONTEE_R		BIT(8)
#define   CHMU_INST0_CAP0_TRACK_NONTEE_W		BIT(9)
#define   CHMU_INST0_CAP0_TRACK_NONTEE_RW		BIT(10)
#define   CHMU_INST0_CAP0_TRACK_R			BIT(11)
#define   CHMU_INST0_CAP0_TRACK_W			BIT(12)
#define   CHMU_INST0_CAP0_TRACK_RW			BIT(13)
/* Epoch defined as scale * multiplier */
#define   CHMU_INST0_CAP0_EPOCH_MAX_SCALE_MSK		GENMASK(19, 16)
#define     CHMU_EPOCH_SCALE_100US			1
#define     CHMU_EPOCH_SCALE_1MS			2
#define     CHMU_INST0_SCALE_10MS			3
#define     CHMU_INST0_SCALE_100MS			4
#define     CHMU_INST0_SCALE_1US			5
#define   CHMU_INST0_CAP0_EPOCH_MAX_MULT_MSK		GENMASK(31, 20)
#define   CHMU_INST0_CAP0_EPOCH_MIN_SCALE_MSK		GENMASK_ULL(35, 32)
#define   CHMU_INST0_CAP0_EPOCH_MIN_MULT_MSK		GENMASK_ULL(47, 36)
#define   CHMU_INST0_CAP0_HOTLIST_SIZE_MSK		GENMASK_ULL(63, 48)
#define CHMU_INST0_CAP1_REG				0x08
/* Power of 2 * 256 bits */
#define   CHMU_INST0_CAP1_UNIT_SIZE_MSK			GENMASK(31, 0)
/* Power of 2 */
#define   CHMU_INST0_CAP1_DOWNSAMP_MSK			GENMASK_ULL(47, 32)
#define   CHMU_INST0_CAP1_EPOCH_SUP			BIT_ULL(48)
#define   CHMU_INST0_CAP1_ALWAYS_ON_SUP			BIT_ULL(49)
#define   CHMU_INST0_CAP1_RAND_DOWNSAMP_SUP		BIT_ULL(50)
#define   CHMU_INST0_CAP1_ADDR_OVERLAP_SUP		BIT_ULL(51)
#define   CHMU_INST0_CAP1_POSTPONED_ON_OVRFLOW_SUP	BIT_ULL(52)

/*
 * In CXL r3.2 all defined as part of single giant CAP register.
 * Where a whole 64 bits is in one field just name after the field.
 */
#define CHMU_INST0_RANGE_BITMAP_OFFSET_REG		0x10
#define CHMU_INST0_HOTLIST_OFFSET_REG			0x18

#define CHMU_INST0_CFG0_REG				0x40
#define   CHMU_INST0_CFG0_WHAT_MSK			GENMASK(7, 0)
#define      CHMU_INST0_CFG0_WHAT_NONTEE_R		1
#define      CHMU_INST0_CFG0_WHAT_NONTEE_W		2
#define      CHMU_INST0_CFG0_WHAT_NONTEE_RW		3
#define      CHMU_INST0_CFG0_WHAT_R			4
#define      CHMU_INST0_CFG0_WHAT_W			5
#define      CHMU_INST0_CFG0_WHAT_RW			6
#define   CHMU_INST0_CFG0_RAND_DOWNSAMP_EN		BIT(8)
#define   CHMU_INST0_CFG0_OVRFLW_INT_EN			BIT(9)
#define   CHMU_INST0_CFG0_FILLTHRESH_INT_EN		BIT(10)
#define   CHMU_INST0_CFG0_ENABLE			BIT(16)
#define   CHMU_INST0_CFG0_RESET_COUNTERS		BIT(17)
#define   CHMU_INST0_CFG0_HOTNESS_THRESH_MSK		GENMASK_ULL(63, 32)
#define CHMU_INST0_CFG1_REG				0x48
#define   CHMU_INST0_CFG1_UNIT_SIZE_MSK			GENMASK(31, 0)
#define   CHMU_INST0_CFG1_DS_FACTOR_MSK			GENMASK(35, 32)
#define   CHMU_INST0_CFG1_MODE_MSK			GENMASK_ULL(47, 40)
#define   CHMU_INST0_CFG1_EPOCH_SCALE_MSK		GENMASK_ULL(51, 48)
#define   CHMU_INST0_CFG1_EPOCH_MULT_MSK		GENMASK_ULL(63, 52)
#define CHMU_INST0_CFG2_REG				0x50
#define   CHMU_INST0_CFG2_FILLTHRESH_THRESHOLD_MSK	GENMASK(15, 0)

#define CHMU_INST0_STATUS_REG				0x60
#define   CHMU_INST0_STATUS_ENABLED			BIT(0)
#define   CHMU_INST0_STATUS_OP_INPROG_MSK		GENMASK(31, 16)
#define     CHMU_INST0_STATUS_OP_INPROG_NONE		0
#define     CHMU_INST0_STATUS_OP_INPROG_ENABLE		1
#define     CHMU_INST0_STATUS_OP_INPROG_DISABLE		2
#define     CHMU_INST0_STATUS_OP_INPROG_RESET		3
#define   CHMU_INST0_STATUS_COUNTER_WIDTH_MSK		GENMASK_ULL(39, 32)
#define   CHMU_INST0_STATUS_OVRFLW			BIT_ULL(40)
#define   CHMU_INST0_STATUS_FILLTHRESH			BIT_ULL(41)

/* 2 byte registers */
#define CHMU_INST0_HEAD_REG				0x68
#define CHMU_INST0_TAIL_REG				0x6A

/* CFG attribute bit mappings */
#define CXL_HMU_ATTR_CONFIG_EPOCH_TYPE_MASK GENMASK(1, 0)
#define CXL_HMU_ATTR_CONFIG_ACCESS_TYPE_MASK GENMASK(9, 2)
#define CXL_HMU_ATTR_CONFIG_EPOCH_SCALE_MASK GENMASK(13, 10)
#define CXL_HMU_ATTR_CONFIG_EPOCH_MULT_MASK GENMASK(25, 14)
#define CXL_HMU_ATTR_CONFIG_RANDOM_DS_MASK BIT(26)
#define CXL_HMU_ATTR_CONFIG_DS_FACTOR_MASK GENMASK_ULL(34, 27)

#define CXL_HMU_ATTR_CONFIG1_HOTNESS_THRESH_MASK GENMASK(31, 0)
#define CXL_HMU_ATTR_CONFIG1_HOTNESS_GRANUAL_MASK GENMASK_ULL(63, 32)

/* In multiples of 256MiB */
#define CXL_HMU_ATTR_CONFIG2_DPA_BASE_MASK GENMASK(31, 0)
#define CXL_HMU_ATTR_CONFIG2_DPA_SIZE_MASK GENMASK_ULL(63, 32)

/* Range bitmap registers at offset 0x10 + Range Config Bitmap offset */
/* Hotlist registers at offset 0x10 + Hotlist Register offset */
static int cxl_hmu_cpuhp_state_num;

enum cxl_hmu_reporting_mode {
	CHMU_MODE_EPOCH = 0,
	CHMU_MODE_ALWAYS_ON = 1,
};

struct cxl_hmu_info {
	struct pmu pmu;
	struct perf_output_handle handle;
	void __iomem *base;
	struct hlist_node node;
	int irq;
	int on_cpu;
	u32 hot_thresh;
	u32 hot_gran; /* power of 2, 256 to 2 GiB */
	/* For now use a range rather than a bitmap, chunks of 256MiB */
	u32 range_base;
	u32 range_num;
	enum cxl_hmu_reporting_mode reporting_mode;
	u8 m2s_requests_to_track;
	u8 ds_factor_pow2;
	u8 epoch_scale;
	u16 epoch_mult;
	bool randomized_ds;

	/* Protect both the device state for RMW and the pmu state */
	spinlock_t lock;
};

#define pmu_to_cxl_hmu(p) container_of(p, struct cxl_hmu_info, pmu)

/* destriptor for the aux buffer */
struct cxl_hmu_buf {
	size_t length;
	int nr_pages;
	void *base;
	long pos;
};

static ssize_t cpumask_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct cxl_hmu_info *hmu = dev_get_drvdata(dev);

	return cpumap_print_to_pagebuf(true, buf, cpumask_of(hmu->on_cpu));
}
static DEVICE_ATTR_RO(cpumask);

static struct attribute *cxl_hmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL
};

static const struct attribute_group cxl_hmu_cpumask_attr_group = {
	.attrs = cxl_hmu_cpumask_attrs,
};

/* Sized fields to future proof based on space in spec */
PMU_FORMAT_ATTR(epoch_type, "config:0-1"); /* 2 bits to future proof */
PMU_FORMAT_ATTR(access_type, "config:2-9");
PMU_FORMAT_ATTR(epoch_scale, "config:10-13");
PMU_FORMAT_ATTR(epoch_multiplier, "config:14-25");
PMU_FORMAT_ATTR(randomized_downsampling, "config:26-26");
PMU_FORMAT_ATTR(downsampling_factor, "config:27-34");

PMU_FORMAT_ATTR(hotness_threshold, "config1:0-31");
PMU_FORMAT_ATTR(hotness_granual, "config1:32-63");

/* RFC this is a bitmap can we control it better? */
PMU_FORMAT_ATTR(range_base, "config2:0-31");
PMU_FORMAT_ATTR(range_size, "config2:32-63");
static struct attribute *cxl_hmu_format_attrs[] = {
	&format_attr_epoch_type.attr,
	&format_attr_access_type.attr,
	&format_attr_epoch_scale.attr,
	&format_attr_epoch_multiplier.attr,
	&format_attr_randomized_downsampling.attr,
	&format_attr_downsampling_factor.attr,
	&format_attr_hotness_threshold.attr,
	&format_attr_hotness_granual.attr,
	&format_attr_range_base.attr,
	&format_attr_range_size.attr,
	NULL
};

static struct attribute_group cxl_hmu_format_attr_group = {
	.name = "format",
	.attrs = cxl_hmu_format_attrs,
};

static const struct attribute_group *cxl_hmu_groups[] = {
	&cxl_hmu_cpumask_attr_group,
	&cxl_hmu_format_attr_group,
	NULL
};

static int cxl_hmu_event_init(struct perf_event *event)
{
	struct cxl_hmu_info *hmu = pmu_to_cxl_hmu(event->pmu);
	struct device *dev = event->pmu->dev;
	u32 gran_sup;
	u16 ds_sup;
	u64 cap0, cap1;
	u64 epoch_min, epoch_max, epoch;
	u64 hotlist_offset = readq(hmu->base + CHMU_INST0_HOTLIST_OFFSET_REG);
	u64 bitmap_offset = readq(hmu->base + CHMU_INST0_RANGE_BITMAP_OFFSET_REG);

	if (event->attr.type != hmu->pmu.type)
		return -ENOENT;

	if (event->cpu < 0) {
		dev_info(dev, "Per-task mode not supported\n");
		return -EOPNOTSUPP;
	}

	if (event->attach_state & PERF_ATTACH_TASK)
		return -EOPNOTSUPP;

	cap0 = readq(hmu->base + CHMU_INST0_CAP0_REG);
	cap1 = readq(hmu->base + CHMU_INST0_CAP1_REG);

	switch (FIELD_GET(CXL_HMU_ATTR_CONFIG_EPOCH_TYPE_MASK,
		event->attr.config)) {
	case 0:
		if (!FIELD_GET(CHMU_INST0_CAP1_EPOCH_SUP, cap1))
			return -EOPNOTSUPP;
		hmu->reporting_mode = CHMU_MODE_EPOCH;
		break;
	case 1:
		if (!FIELD_GET(CHMU_INST0_CAP1_ALWAYS_ON_SUP, cap1))
			return -EOPNOTSUPP;
		hmu->reporting_mode = CHMU_MODE_ALWAYS_ON;
		break;
	default:
		dev_dbg(dev, "Tried for a non existent type\n");
		return -EINVAL;
	}
	hmu->randomized_ds = FIELD_GET(CXL_HMU_ATTR_CONFIG_RANDOM_DS_MASK,
				      event->attr.config);
	if (hmu->randomized_ds && !FIELD_GET(CHMU_INST0_CAP1_RAND_DOWNSAMP_SUP, cap1)) {
		dev_info(dev, "Randomized downsampling not supported\n");
		return -EOPNOTSUPP;
	}

	/* RFC: sanity check against currently defined or not? */
	hmu->m2s_requests_to_track = FIELD_GET(CXL_HMU_ATTR_CONFIG_ACCESS_TYPE_MASK,
					       event->attr.config);
	if (hmu->m2s_requests_to_track < CHMU_INST0_CFG0_WHAT_NONTEE_R ||
	    hmu->m2s_requests_to_track > CHMU_INST0_CFG0_WHAT_RW) {
		dev_dbg(dev, "Requested a reserved type to track\n");
		return -EINVAL;
	}

	hmu->hot_thresh = FIELD_GET(CXL_HMU_ATTR_CONFIG1_HOTNESS_THRESH_MASK,
					   event->attr.config1);

	hmu->hot_gran = FIELD_GET(CXL_HMU_ATTR_CONFIG1_HOTNESS_GRANUAL_MASK,
					 event->attr.config1);

	gran_sup = FIELD_GET(CHMU_INST0_CAP1_UNIT_SIZE_MSK, cap1);
	/* Default to smallest granual if not specified */
	if (hmu->hot_gran == 0 && gran_sup)
		hmu->hot_gran = 8 + ffs(gran_sup);

	if (hmu->hot_gran < 8) {
		dev_dbg(dev, "Granual less than 256 bytes, not valid in CXL 3.2\n");
		return -EINVAL;
	}

	if (!((1 << (hmu->hot_gran - 8)) & gran_sup)) {
		dev_dbg(dev, "Granual %d not supported, supported mask %x\n",
			hmu->hot_gran - 8, gran_sup);
		return -EOPNOTSUPP;
	}

	ds_sup = FIELD_GET(CHMU_INST0_CAP1_DOWNSAMP_MSK, cap1);
	hmu->ds_factor_pow2 = FIELD_GET(CXL_HMU_ATTR_CONFIG_DS_FACTOR_MASK,
					event->attr.config);
	if (!((1 << hmu->ds_factor_pow2) & ds_sup)) {
		/* Special case default of 0 if not supported as smallest DS possibe */
		if (hmu->ds_factor_pow2 == 0 && ds_sup) {
			hmu->ds_factor_pow2 = ffs(ds_sup);
			dev_dbg(dev, "Downsampling set to default min of %d\n",
				hmu->ds_factor_pow2);
		} else {
			dev_dbg(dev, "Downsampling %d no supported, supported mask %x\n",
				hmu->ds_factor_pow2, ds_sup);
			return -EOPNOTSUPP;
		}
	}

	hmu->epoch_scale = FIELD_GET(CXL_HMU_ATTR_CONFIG_EPOCH_SCALE_MASK,
				     event->attr.config);

	hmu->epoch_mult = FIELD_GET(CXL_HMU_ATTR_CONFIG_EPOCH_MULT_MASK,
				    event->attr.config);

	/* Default to what? */
	if (hmu->epoch_mult == 0 && hmu->epoch_scale == 0) {
		hmu->epoch_scale = FIELD_GET(CHMU_INST0_CAP0_EPOCH_MIN_SCALE_MSK, cap0);
		hmu->epoch_mult = FIELD_GET(CHMU_INST0_CAP0_EPOCH_MIN_MULT_MSK, cap0);
	}
	if (hmu->epoch_mult == 0)
		return -EINVAL;

	/* Units of 100ms */
	epoch_min = int_pow(10, FIELD_GET(CHMU_INST0_CAP0_EPOCH_MIN_SCALE_MSK, cap0)) *
		(u64)FIELD_GET(CHMU_INST0_CAP0_EPOCH_MIN_MULT_MSK, cap0);
	epoch_max = int_pow(10, FIELD_GET(CHMU_INST0_CAP0_EPOCH_MAX_SCALE_MSK, cap0)) *
		(u64)FIELD_GET(CHMU_INST0_CAP0_EPOCH_MAX_MULT_MSK, cap0);
	epoch = int_pow(10, hmu->epoch_scale) * (u64)hmu->epoch_mult;

	if (epoch > epoch_max || epoch < epoch_min) {
		dev_dbg(dev, "out of range %llu %llu %llu\n",
			epoch, epoch_max, epoch_min);
		return -EINVAL;
	}

	hmu->range_base = FIELD_GET(CXL_HMU_ATTR_CONFIG2_DPA_BASE_MASK,
				    event->attr.config2);
	hmu->range_num = FIELD_GET(CXL_HMU_ATTR_CONFIG2_DPA_SIZE_MASK,
				   event->attr.config2);

	if (hmu->range_num == 0) {
		/* Set a default of 'everything' */
		hmu->range_num = (hotlist_offset - bitmap_offset) * 8;
	}
	/* TODO - pass in better DPA range info from parent driver */
	if ((u64)hmu->range_base + hmu->range_num >
	    (hotlist_offset - bitmap_offset) * 8) {
		dev_dbg(dev, "Requested range that this HMU can't track Can track 0x%llx, asked for 0x%x to 0x%x\n",
			(hotlist_offset - bitmap_offset) * 8,
			hmu->range_base, hmu->range_base + hmu->range_num);
		return -EINVAL;
	}

	return 0;
}

static int cxl_hmu_update_aux(struct cxl_hmu_info *hmu, bool stop)
{
	struct perf_output_handle *handle = &hmu->handle;
	struct cxl_hmu_buf *buf = perf_get_aux(handle);
	struct perf_event *event = handle->event;
	size_t size = 0;
	size_t tocopy, tocopy2;

	u64 offset = readq(hmu->base + CHMU_INST0_HOTLIST_OFFSET_REG);
	u16 head = readw(hmu->base + CHMU_INST0_HEAD_REG);
	u16 tail = readw(hmu->base + CHMU_INST0_TAIL_REG);
	u8 count_width = FIELD_GET(CHMU_INST0_STATUS_COUNTER_WIDTH_MSK,
				   readq(hmu->base + CHMU_INST0_STATUS_REG));
	u16 top = FIELD_GET(CHMU_INST0_CAP0_HOTLIST_SIZE_MSK,
			    readq(hmu->base + CHMU_INST0_CAP0_REG));
	/* 16 bytes of header - arbitrary choice! */
#define CHMU_HEADER0_SIZE_MASK GENMASK(15, 0)
#define CHMU_HEADER0_COUNT_WIDTH GENMASK(23, 16)
	u64 header[2];

	if (tail > head) {
		tocopy = min_t(size_t, (tail - head) * 8,
			       buf->length - buf->pos - sizeof(header));
		header[0] = FIELD_PREP(CHMU_HEADER0_SIZE_MASK, tocopy / 8) |
			    FIELD_PREP(CHMU_HEADER0_COUNT_WIDTH, count_width);
		header[1] = 0xDEADBEEF;
		if (tocopy) {
			memcpy(buf->base + buf->pos, header, sizeof(header));
			size += sizeof(header);
			buf->pos += sizeof(header);
			memcpy_fromio(buf->base + buf->pos,
				      hmu->base + offset + head * 8, tocopy);
			size += tocopy;
			buf->pos += tocopy;
		}

	} else if (tail < head) { /* wrap around */
		tocopy = min_t(size_t, (top - head) * 8,
			       buf->length - buf->pos - sizeof(header));
		tocopy2 = min_t(size_t, tail * 8,
				buf->length - buf->pos - tocopy - sizeof(header));
		header[0] = FIELD_PREP(CHMU_HEADER0_SIZE_MASK, (tocopy + tocopy2) / 8) |
			    FIELD_PREP(CHMU_HEADER0_COUNT_WIDTH, count_width);
		header[1] = 0xDEADBEEF;
		if (tocopy) {
			memcpy(buf->base + buf->pos, header, sizeof(header));
			size += sizeof(header);
			buf->pos += sizeof(header);
			memcpy_fromio(buf->base + buf->pos,
				      hmu->base + offset + head * 8, tocopy);
			size += tocopy;
			buf->pos += tocopy;

		}

		if (tocopy2) {
			memcpy_fromio(buf->base + buf->pos,
				      hmu->base + offset, tocopy2);
			size += tocopy2;
			buf->pos += tocopy2;
		}
	} /* may be no data */

	perf_aux_output_end(handle, size);
	if (buf->pos == buf->length)
		return -EINVAL; /* FULL */

	/* Do this after the space check, so the buffer on device will not overwrite */
	writew(tail, hmu->base + CHMU_INST0_HEAD_REG);

	if (!stop) {
		buf = perf_aux_output_begin(handle, event);
		if (!buf)
			return -EINVAL;
		buf->pos = handle->head % buf->length;
	}
	return 0;
}

static int __cxl_hmu_start(struct perf_event *event, int flags)
{
	struct cxl_hmu_info *hmu = pmu_to_cxl_hmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct device *dev = event->pmu->dev;
	struct cxl_hmu_buf *buf;
	int cpu = event->cpu;
	u64 val, status, bitmap_base;
	int ret, i;
	u16 list_len = FIELD_GET(CHMU_INST0_CAP0_HOTLIST_SIZE_MSK,
				 readq(hmu->base + CHMU_INST0_CAP0_REG));

	hwc->state = 0;
	status = readq(hmu->base + CHMU_INST0_STATUS_REG);
	if (FIELD_GET(CHMU_INST0_STATUS_ENABLED, status)) {
		dev_dbg(dev, "trace already started\n");
		return -EBUSY;
	}
	/* TODO: Figure out what to do as very likely this is shared
	 *  - Hopefully only with other HMU instances
	 */
	ret = irq_set_affinity(hmu->irq, cpumask_of(cpu));
	if (ret)
		dev_warn(dev, "failed to affinity of HMU interrupt\n");

	hmu->on_cpu = cpu;

	buf = perf_aux_output_begin(&hmu->handle, event);
	if (!buf) {
		dev_dbg(event->pmu->dev, "aux output begin failed\n");
		return -EINVAL;
	}

	buf->pos = hmu->handle.head % buf->length;
	/* Reset here disrupts samping with -F, should we avoid doing so? */
	writeq(FIELD_PREP(CHMU_INST0_CFG0_RESET_COUNTERS, 1),
		hmu->base + CHMU_INST0_CFG0_REG);

	ret = readq_poll_timeout_atomic(hmu->base + CHMU_INST0_STATUS_REG, status,
		(FIELD_GET(CHMU_INST0_STATUS_OP_INPROG_MSK, status) == 0),
		10, 100000);
	if (ret) {
		dev_dbg(event->pmu->dev, "Reset timed out\n");
		return ret;
	}
	/* Setup what is being capured */
	/* Type of capture, granularity etc */

	val = FIELD_PREP(CHMU_INST0_CFG1_UNIT_SIZE_MSK, hmu->hot_gran) |
		FIELD_PREP(CHMU_INST0_CFG1_DS_FACTOR_MSK, hmu->ds_factor_pow2) |
		FIELD_PREP(CHMU_INST0_CFG1_MODE_MSK, hmu->reporting_mode) |
		FIELD_PREP(CHMU_INST0_CFG1_EPOCH_SCALE_MSK, hmu->epoch_scale) |
		FIELD_PREP(CHMU_INST0_CFG1_EPOCH_MULT_MSK, hmu->epoch_mult);
	writeq(val, hmu->base + CHMU_INST0_CFG1_REG);

	val = 0;
	bitmap_base = readq(hmu->base + CHMU_INST0_RANGE_BITMAP_OFFSET_REG);
	for (i = hmu->range_base; i < hmu->range_base + hmu->range_num; i++) {
		val |= BIT(i % 64);
		if (i % 64 == 63) {
			writeq(val, hmu->base + bitmap_base + (i / 64) * 8);
			val = 0;
		}
	}
	/* Potential duplicate write that doesn't matter */
	writeq(val, hmu->base + bitmap_base + (i / 64) * 8);

	/* Set notificaiton threshold to half of buffer */
	val = FIELD_PREP(CHMU_INST0_CFG2_FILLTHRESH_THRESHOLD_MSK,
			 list_len / 2);
	writeq(val, hmu->base + CHMU_INST0_CFG2_REG);

	/*
	 * RFC: Only after granual is set can the width be known - so can only check here,
	 * or program granual size earlier just to see if it will work here.
	 */
	status = readq(hmu->base + CHMU_INST0_STATUS_REG);
	if (hmu->hot_thresh >= (1 << (64 - FIELD_GET(CHMU_INST0_STATUS_COUNTER_WIDTH_MSK, status))))
		return -EINVAL;
	/* Start the unit up */
	val = FIELD_PREP(CHMU_INST0_CFG0_WHAT_MSK, hmu->m2s_requests_to_track) |
		FIELD_PREP(CHMU_INST0_CFG0_RAND_DOWNSAMP_EN,
			   hmu->randomized_ds ? 1 : 0) |
		FIELD_PREP(CHMU_INST0_CFG0_OVRFLW_INT_EN, 1) |
		FIELD_PREP(CHMU_INST0_CFG0_FILLTHRESH_INT_EN, 1) |
		FIELD_PREP(CHMU_INST0_CFG0_ENABLE, 1) |
		FIELD_PREP(CHMU_INST0_CFG0_HOTNESS_THRESH_MSK, hmu->hot_thresh);
	writeq(val, hmu->base + CHMU_INST0_CFG0_REG);

	/* Poll status register for enablement to complete */
	ret = readq_poll_timeout_atomic(hmu->base + CHMU_INST0_STATUS_REG, status,
		(FIELD_GET(CHMU_INST0_STATUS_OP_INPROG_MSK, status) == 0),
		10, 100000);
	if (ret) {
		dev_info(event->pmu->dev, "Enable timed out\n");
		return ret;
	}

	return 0;
}

static void cxl_hmu_start(struct perf_event *event, int flags)
{
	struct cxl_hmu_info *hmu = pmu_to_cxl_hmu(event->pmu);
	int ret;

	guard(spinlock)(&hmu->lock);

	ret = __cxl_hmu_start(event, flags);
	if (ret)
		event->hw.state |= PERF_HES_STOPPED;
}

static void cxl_hmu_stop(struct perf_event *event, int flags)
{
	struct cxl_hmu_info *hmu = pmu_to_cxl_hmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 status, val;
	int ret;

	if (hwc->state & PERF_HES_STOPPED)
		return;

	guard(spinlock)(&hmu->lock);
	status = readq(hmu->base + CHMU_INST0_STATUS_REG);
	if (FIELD_GET(CHMU_INST0_STATUS_ENABLED, status)) {
		/* Stop the HMU instance */
		val = readq(hmu->base + CHMU_INST0_CFG0_REG);
		val &= ~CHMU_INST0_CFG0_ENABLE;
		writeq(val, hmu->base + CHMU_INST0_CFG0_REG);

		ret = readq_poll_timeout_atomic(hmu->base + CHMU_INST0_STATUS_REG, status,
			(FIELD_GET(CHMU_INST0_STATUS_OP_INPROG_MSK, status) == 0),
			10, 100000);
		if (ret) {
			dev_info(event->pmu->dev, "Disable timed out\n");
			return;
		}

		cxl_hmu_update_aux(hmu, true);
	}

}
static void cxl_hmu_read(struct perf_event *event)
{
	/* Nothing to do */
}

static int cxl_hmu_add(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	if (flags & PERF_EF_START) {
		cxl_hmu_start(event, PERF_EF_RELOAD);
		if (hwc->state & PERF_HES_STOPPED)
			return -EINVAL;
	}
	return 0;
}

/*
 * There is a lot to do in here, but using a thread is not
 * currently possible for a perf PMU driver.
 */
static irqreturn_t cxl_hmu_irq(int irq, void *data)
{
	struct cxl_hmu_info *info = data;
	u64 status;
	int ret;

	status = readq(info->base + CHMU_INST0_STATUS_REG);
	if (!FIELD_GET(CHMU_INST0_STATUS_OVRFLW, status) &&
	    !FIELD_GET(CHMU_INST0_STATUS_FILLTHRESH, status))
		return IRQ_NONE;

	ret = cxl_hmu_update_aux(info, false);
	if (ret)
		dev_err(info->pmu.dev, "interrupt update failed\n");

	/*
	 * They are level interrupts so should trigger on next fill
	 * hence should be no problem with races.
	 */
	writeq(status, info->base + CHMU_INST0_STATUS_REG);

	return IRQ_HANDLED;
}

static void cxl_hmu_del(struct perf_event *event, int flags)
{
	cxl_hmu_stop(event, PERF_EF_UPDATE);
}

static void *cxl_hmu_setup_aux(struct perf_event *event, void **pages,
			int nr_pages, bool overwrite)
{
	int i;

	if (overwrite) {
		dev_warn(event->pmu->dev, "Overwrite mode is not supported\n");
		return NULL;
	}

	if (nr_pages < 1)
		return NULL;

	struct cxl_hmu_buf *buf __free(kfree) =
		kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return NULL;

	struct page **pagelist __free(kfree) =
		kcalloc(nr_pages, sizeof(*pagelist), GFP_KERNEL);
	if (!pagelist)
		return NULL;

	for (i = 0; i < nr_pages; i++)
		pagelist[i] = virt_to_page(pages[i]);

	buf->base = vmap(pagelist, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!buf->base)
		return NULL;

	buf->nr_pages = nr_pages;
	buf->length = nr_pages * PAGE_SIZE;
	buf->pos = 0;

	return_ptr(buf);
}

static void cxl_hmu_free_aux(void *aux)
{
	struct cxl_hmu_buf *buf = aux;

	vunmap(buf->base);
	kfree(buf);
}

static void cxl_hmu_perf_unregister(void *_info)
{
	struct cxl_hmu_info *info = _info;

	perf_pmu_unregister(&info->pmu);
}

static void cxl_hmu_cpuhp_remove(void *_info)
{
	struct cxl_hmu_info *info = _info;

	cpuhp_state_remove_instance_nocalls(cxl_hmu_cpuhp_state_num,
					    &info->node);
}

static int cxl_hmu_probe(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev->parent);
	struct cxl_hmu *hmu = to_cxl_hmu(dev);
	int i, rc;

	int num_inst = FIELD_GET(CHMU_COMMON_CAP0_NUMINST_MSK,
				 readq(hmu->base + CHMU_COMMON_CAP0_REG));
	int inst_len = FIELD_GET(CHMU_COMMON_CAP1_INSTLEN_MSK,
				 readq(hmu->base + CHMU_COMMON_CAP1_REG));

	for (i = 0; i < num_inst; i++) {
		struct cxl_hmu_info *info;
		char *pmu_name;
		int msg_num;
		u64 val;

		info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
		if (!info)
			return -ENOMEM;

		dev_set_drvdata(dev, info);
		info->on_cpu = -1;
		info->base = hmu->base + 0x10 + inst_len * i;

		val = readq(info->base + CHMU_INST0_CAP0_REG);
		msg_num = FIELD_GET(CHMU_INST0_CAP0_MSI_N_MSK, val);

		/* TODO add polling support - for now require threshold */
		if (!FIELD_GET(CHMU_INST0_CAP0_FILLTHRESH_CAP, val)) {
			devm_kfree(dev, info);
			continue;
		}

		spin_lock_init(&info->lock);

		pmu_name = devm_kasprintf(dev, GFP_KERNEL,
					  "cxl_hmu_mem%d.%d.%d",
					  hmu->assoc_id, hmu->index, i);
		if (!pmu_name)
			return -ENOMEM;

		info->pmu = (struct pmu) {
			.name = pmu_name,
			.parent = dev,
			.module = THIS_MODULE,
			.capabilities = PERF_PMU_CAP_EXCLUSIVE |
					PERF_PMU_CAP_NO_EXCLUDE,
			.task_ctx_nr = perf_sw_context,
			.attr_groups = cxl_hmu_groups,
			.event_init = cxl_hmu_event_init,
			.setup_aux = cxl_hmu_setup_aux,
			.free_aux = cxl_hmu_free_aux,
			.start = cxl_hmu_start,
			.stop = cxl_hmu_stop,
			.add = cxl_hmu_add,
			.del = cxl_hmu_del,
			.read = cxl_hmu_read,
		};
		rc = pci_irq_vector(pdev, msg_num);
		if (rc < 0)
			return rc;
		info->irq = rc;

		/*
		 * Whilst there is a 'strong' recomendation that the interrupt
		 * should not be shared it is not a requirement.
		 * Can we support IRQF_SHARED on a PMU?
		 */
		rc = devm_request_irq(dev, info->irq, cxl_hmu_irq,
				      IRQF_NO_THREAD | IRQF_NOBALANCING,
				      pmu_name, info);
		if (rc)
			return rc;

		rc = cpuhp_state_add_instance(cxl_hmu_cpuhp_state_num,
					      &info->node);
		if (rc)
			return rc;

		rc = devm_add_action_or_reset(dev, cxl_hmu_cpuhp_remove, info);
		if (rc)
			return rc;

		rc = perf_pmu_register(&info->pmu, info->pmu.name, -1);
		if (rc)
			return rc;

		rc = devm_add_action_or_reset(dev, cxl_hmu_perf_unregister,
					      info);
		if (rc)
			return rc;
	}
	return 0;
}

static struct cxl_driver cxl_hmu_driver = {
	.name = "cxl_hmu",
	.probe = cxl_hmu_probe,
	.id = CXL_DEVICE_HMU,
};

static int cxl_hmu_online_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct cxl_hmu_info *info =
		hlist_entry_safe(node, struct cxl_hmu_info, node);

	if (info->on_cpu != -1)
		return 0;

	info->on_cpu = cpu;

	WARN_ON(irq_set_affinity(info->irq, cpumask_of(cpu)));

	return 0;
}

static int cxl_hmu_offline_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct cxl_hmu_info *info =
		hlist_entry_safe(node, struct cxl_hmu_info, node);
	unsigned int target;

	if (info->on_cpu != cpu)
		return 0;

	info->on_cpu = -1;
	target = cpumask_any_but(cpu_online_mask, cpu);
	if (target >= nr_cpu_ids) {
		dev_err(info->pmu.dev, "Unable to find a suitable CPU\n");
		return 0;
	}

	perf_pmu_migrate_context(&info->pmu, cpu, target);
	info->on_cpu = target;
	/*
	 * CPU HP lock is held so we should be guaranteed that this CPU hasn't
	 * yet gone away.
	 */
	WARN_ON(irq_set_affinity(info->irq, cpumask_of(target)));
	return 0;
}

static __init int cxl_hmu_init(void)
{
	int rc;

	rc = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN,
				     "AP_PERF_CXL_HMU_ONLINE",
				     cxl_hmu_online_cpu, cxl_hmu_offline_cpu);
	if (rc < 0)
		return rc;
	cxl_hmu_cpuhp_state_num = rc;

	rc = cxl_driver_register(&cxl_hmu_driver);
	if (rc)
		cpuhp_remove_multi_state(cxl_hmu_cpuhp_state_num);

	return rc;
}

static __exit void cxl_hmu_exit(void)
{
	cxl_driver_unregister(&cxl_hmu_driver);
	cpuhp_remove_multi_state(cxl_hmu_cpuhp_state_num);
}

MODULE_AUTHOR("Jonathan Cameron <Jonathan.Cameron@huawei.com>");
MODULE_DESCRIPTION("CXL Hotness Monitor Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(CXL);
module_init(cxl_hmu_init);
module_exit(cxl_hmu_exit);
MODULE_ALIAS_CXL(CXL_DEVICE_HMU);
