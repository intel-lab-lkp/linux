// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Rockchip ISP1 Driver - Base driver
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *
 * Based on Rockchip ISP1 driver by Rockchip Electronics Co., Ltd.
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/minmax.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "rkisp1-common.h"
#include "rkisp1-regs.h"

struct rkisp1_debug_register {
	u32 reg;
	u32 shd;
	const char * const name;
};

struct rkisp1_debug_counter {
	const char * const name;
	unsigned long *value;
};

#define RKISP1_DEBUG_REG(name)		{ RKISP1_CIF_##name, 0, #name }
#define RKISP1_DEBUG_SHD_REG(name) { \
	RKISP1_CIF_##name, RKISP1_CIF_##name##_SHD, #name \
}

/* Keep this up-to-date when adding new registers. */
#define RKISP1_MAX_REG_LENGTH		21

static int rkisp1_debug_dump_regs(struct rkisp1_device *rkisp1,
				  struct seq_file *m, unsigned int offset,
				  const struct rkisp1_debug_register *regs)
{
	const int width = RKISP1_MAX_REG_LENGTH;
	u32 val, shd;
	int ret;

	ret = pm_runtime_get_if_in_use(rkisp1->dev);
	if (ret <= 0)
		return ret ? : -ENODATA;

	for (; regs->name; ++regs) {
		val = rkisp1_read(rkisp1, offset + regs->reg);

		if (regs->shd) {
			shd = rkisp1_read(rkisp1, offset + regs->shd);
			seq_printf(m, "%*s: 0x%08x/0x%08x\n", width, regs->name,
				   val, shd);
		} else {
			seq_printf(m, "%*s: 0x%08x\n", width, regs->name, val);
		}
	}

	pm_runtime_put(rkisp1->dev);

	return 0;
}

static int rkisp1_debug_dump_core_regs_show(struct seq_file *m, void *p)
{
	static const struct rkisp1_debug_register registers[] = {
		RKISP1_DEBUG_REG(VI_CCL),
		RKISP1_DEBUG_REG(VI_ICCL),
		RKISP1_DEBUG_REG(VI_IRCL),
		RKISP1_DEBUG_REG(VI_DPCL),
		RKISP1_DEBUG_REG(MI_CTRL),
		RKISP1_DEBUG_REG(MI_BYTE_CNT),
		RKISP1_DEBUG_REG(MI_CTRL_SHD),
		RKISP1_DEBUG_REG(MI_RIS),
		RKISP1_DEBUG_REG(MI_STATUS),
		RKISP1_DEBUG_REG(MI_DMA_CTRL),
		RKISP1_DEBUG_REG(MI_DMA_STATUS),
		{ /* Sentinel */ },
	};
	struct rkisp1_device *rkisp1 = m->private;

	return rkisp1_debug_dump_regs(rkisp1, m, 0, registers);
}
DEFINE_SHOW_ATTRIBUTE(rkisp1_debug_dump_core_regs);

static int rkisp1_debug_dump_isp_regs_show(struct seq_file *m, void *p)
{
	static const struct rkisp1_debug_register registers[] = {
		RKISP1_DEBUG_REG(ISP_CTRL),
		RKISP1_DEBUG_REG(ISP_ACQ_PROP),
		RKISP1_DEBUG_REG(ISP_FLAGS_SHD),
		RKISP1_DEBUG_REG(ISP_RIS),
		RKISP1_DEBUG_REG(ISP_ERR),
		RKISP1_DEBUG_SHD_REG(ISP_IS_H_OFFS),
		RKISP1_DEBUG_SHD_REG(ISP_IS_V_OFFS),
		RKISP1_DEBUG_SHD_REG(ISP_IS_H_SIZE),
		RKISP1_DEBUG_SHD_REG(ISP_IS_V_SIZE),
		{ /* Sentinel */ },
	};
	struct rkisp1_device *rkisp1 = m->private;

	return rkisp1_debug_dump_regs(rkisp1, m, 0, registers);
}
DEFINE_SHOW_ATTRIBUTE(rkisp1_debug_dump_isp_regs);

static int rkisp1_debug_dump_rsz_regs_show(struct seq_file *m, void *p)
{
	static const struct rkisp1_debug_register registers[] = {
		RKISP1_DEBUG_SHD_REG(RSZ_CTRL),
		RKISP1_DEBUG_SHD_REG(RSZ_SCALE_HY),
		RKISP1_DEBUG_SHD_REG(RSZ_SCALE_HCB),
		RKISP1_DEBUG_SHD_REG(RSZ_SCALE_HCR),
		RKISP1_DEBUG_SHD_REG(RSZ_SCALE_VY),
		RKISP1_DEBUG_SHD_REG(RSZ_SCALE_VC),
		RKISP1_DEBUG_SHD_REG(RSZ_PHASE_HY),
		RKISP1_DEBUG_SHD_REG(RSZ_PHASE_HC),
		RKISP1_DEBUG_SHD_REG(RSZ_PHASE_VY),
		RKISP1_DEBUG_SHD_REG(RSZ_PHASE_VC),
		{ /* Sentinel */ },
	};
	struct rkisp1_resizer *rsz = m->private;

	return rkisp1_debug_dump_regs(rsz->rkisp1, m, rsz->regs_base, registers);
}
DEFINE_SHOW_ATTRIBUTE(rkisp1_debug_dump_rsz_regs);

static int rkisp1_debug_dump_mi_mp_show(struct seq_file *m, void *p)
{
	static const struct rkisp1_debug_register registers[] = {
		RKISP1_DEBUG_REG(MI_MP_Y_BASE_AD_INIT),
		RKISP1_DEBUG_REG(MI_MP_Y_BASE_AD_INIT2),
		RKISP1_DEBUG_REG(MI_MP_Y_BASE_AD_SHD),
		RKISP1_DEBUG_REG(MI_MP_Y_SIZE_INIT),
		RKISP1_DEBUG_REG(MI_MP_Y_SIZE_INIT),
		RKISP1_DEBUG_REG(MI_MP_Y_SIZE_SHD),
		RKISP1_DEBUG_REG(MI_MP_Y_OFFS_CNT_SHD),
		{ /* Sentinel */ },
	};
	struct rkisp1_device *rkisp1 = m->private;

	return rkisp1_debug_dump_regs(rkisp1, m, 0, registers);
}
DEFINE_SHOW_ATTRIBUTE(rkisp1_debug_dump_mi_mp);

#define RKISP1_DEBUG_DATA_COUNT_BINS	32
#define RKISP1_DEBUG_DATA_COUNT_STEP	(4096 / RKISP1_DEBUG_DATA_COUNT_BINS)

static int rkisp1_debug_input_status_show(struct seq_file *m, void *p)
{
	struct rkisp1_device *rkisp1 = m->private;
	u16 data_count[RKISP1_DEBUG_DATA_COUNT_BINS] = { };
	unsigned int hsync_count = 0;
	unsigned int vsync_count = 0;
	unsigned int i;
	u32 data;
	u32 val;
	int ret;

	ret = pm_runtime_get_if_in_use(rkisp1->dev);
	if (ret <= 0)
		return ret ? : -ENODATA;

	/* Sample the ISP input port status 10000 times with a 1µs interval. */
	for (i = 0; i < 10000; ++i) {
		val = rkisp1_read(rkisp1, RKISP1_CIF_ISP_FLAGS_SHD);

		data = (val & RKISP1_CIF_ISP_FLAGS_SHD_S_DATA_MASK)
		     >> RKISP1_CIF_ISP_FLAGS_SHD_S_DATA_SHIFT;
		data_count[data / RKISP1_DEBUG_DATA_COUNT_STEP]++;

		if (val & RKISP1_CIF_ISP_FLAGS_SHD_S_HSYNC)
			hsync_count++;
		if (val & RKISP1_CIF_ISP_FLAGS_SHD_S_VSYNC)
			vsync_count++;

		udelay(1);
	}

	pm_runtime_put(rkisp1->dev);

	seq_printf(m, "vsync: %u, hsync: %u\n", vsync_count, hsync_count);
	seq_puts(m, "data:\n");
	for (i = 0; i < ARRAY_SIZE(data_count); ++i)
		seq_printf(m, "- [%04u:%04u]: %u\n",
			   i * RKISP1_DEBUG_DATA_COUNT_STEP,
			   (i + 1) * RKISP1_DEBUG_DATA_COUNT_STEP - 1,
			   data_count[i]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rkisp1_debug_input_status);

static int rkisp1_debug_counters_show(struct seq_file *m, void *p)
{
	struct rkisp1_device *rkisp1 = m->private;
	struct rkisp1_debug *debug = &rkisp1->debug;

	const struct rkisp1_debug_counter counters[] = {
		{ "data_loss", &debug->data_loss },
		{ "outform_size_err", &debug->outform_size_error },
		{ "img_stabilization_size_error", &debug->img_stabilization_size_error },
		{ "inform_size_error", &debug->inform_size_error },
		{ "irq_delay", &debug->irq_delay },
		{ "mipi_error", &debug->mipi_error },
		{ "stats_error", &debug->stats_error },
		{ "mp_stop_timeout", &debug->stop_timeout[RKISP1_MAINPATH] },
		{ "sp_stop_timeout", &debug->stop_timeout[RKISP1_SELFPATH] },
		{ "mp_frame_drop", &debug->frame_drop[RKISP1_MAINPATH] },
		{ "sp_frame_drop", &debug->frame_drop[RKISP1_SELFPATH] },
		{ "complete_frames", &debug->complete_frames },
		{ /* Sentinel */ },
	};

	const struct rkisp1_debug_counter *counter = counters;

	for (; counter->name; ++counter)
		seq_printf(m, "%s: %lu\n", counter->name, *counter->value);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rkisp1_debug_counters);

void rkisp1_debug_reset_counters(struct rkisp1_device *rkisp1)
{
	struct dentry *debugfs_dir = rkisp1->debug.debugfs_dir;
	memset(&rkisp1->debug, 0, sizeof(rkisp1->debug));
	rkisp1->debug.debugfs_dir = debugfs_dir;
}

void rkisp1_debug_init(struct rkisp1_device *rkisp1)
{
	struct rkisp1_debug *debug = &rkisp1->debug;
	struct dentry *regs_dir;

	debug->debugfs_dir = debugfs_create_dir(dev_name(rkisp1->dev), NULL);

	debugfs_create_file("counters", 0444, debug->debugfs_dir, rkisp1,
			    &rkisp1_debug_counters_fops);
	debugfs_create_file("input_status", 0444, debug->debugfs_dir, rkisp1,
			    &rkisp1_debug_input_status_fops);

	regs_dir = debugfs_create_dir("regs", debug->debugfs_dir);

	debugfs_create_file("core", 0444, regs_dir, rkisp1,
			    &rkisp1_debug_dump_core_regs_fops);
	debugfs_create_file("isp", 0444, regs_dir, rkisp1,
			    &rkisp1_debug_dump_isp_regs_fops);
	debugfs_create_file("mrsz", 0444, regs_dir,
			    &rkisp1->resizer_devs[RKISP1_MAINPATH],
			    &rkisp1_debug_dump_rsz_regs_fops);
	debugfs_create_file("srsz", 0444, regs_dir,
			    &rkisp1->resizer_devs[RKISP1_SELFPATH],
			    &rkisp1_debug_dump_rsz_regs_fops);

	debugfs_create_file("mi_mp", 0444, regs_dir, rkisp1,
			    &rkisp1_debug_dump_mi_mp_fops);
}

void rkisp1_debug_cleanup(struct rkisp1_device *rkisp1)
{
	debugfs_remove_recursive(rkisp1->debug.debugfs_dir);
}
