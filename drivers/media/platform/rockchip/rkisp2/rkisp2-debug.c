// SPDX-License-Identifier: (GPL-2.0-or-later OR MIT)
/*
 * Rockchip ISP2 Driver - Base driver
 *
 * Copyright (C) 2019 Collabora, Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 *
 * Based on Rockchip ISP2 driver by Rockchip Electronics Co., Ltd.
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/minmax.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "rkisp2-common.h"

struct rkisp2_debug_register {
	u32 reg;
	u32 shd;
	const char * const name;
};

#define RKISP2_DEBUG_REG(name)		{ RKISP2_CIF_##name, 0, #name }
#define RKISP2_DEBUG_SHD_REG(name) { \
	RKISP2_CIF_##name, RKISP2_CIF_##name##_SHD, #name \
}

/* Keep this up-to-date when adding new registers. */
#define RKISP2_MAX_REG_LENGTH		21

static int rkisp2_debug_dump_regs(struct rkisp2_device *rkisp2,
				  struct seq_file *m, unsigned int offset,
				  const struct rkisp2_debug_register *regs)
{
	const int width = RKISP2_MAX_REG_LENGTH;
	u32 val, shd;
	int ret;

	ret = pm_runtime_get_if_in_use(rkisp2->dev);
	if (ret <= 0)
		return ret ? : -ENODATA;

	for (; regs->name; ++regs) {
		val = rkisp2_read(rkisp2, offset + regs->reg);

		if (regs->shd) {
			shd = rkisp2_read(rkisp2, offset + regs->shd);
			seq_printf(m, "%*s: 0x%08x/0x%08x\n", width, regs->name,
				   val, shd);
		} else {
			seq_printf(m, "%*s: 0x%08x\n", width, regs->name, val);
		}
	}

	pm_runtime_put(rkisp2->dev);

	return 0;
}

static int rkisp2_debug_dump_core_regs_show(struct seq_file *m, void *p)
{
	static const struct rkisp2_debug_register registers[] = {
		RKISP2_DEBUG_REG(VI_ICCL),
		RKISP2_DEBUG_REG(VI_IRCL),
		RKISP2_DEBUG_REG(VI_DPCL),
		RKISP2_DEBUG_REG(MI_CTRL),
		RKISP2_DEBUG_REG(MI_BYTE_CNT),
		RKISP2_DEBUG_REG(MI_CTRL_SHD),
		RKISP2_DEBUG_REG(MI_RIS),
		RKISP2_DEBUG_REG(MI_STATUS),
		RKISP2_DEBUG_REG(MI_DMA_CTRL),
		RKISP2_DEBUG_REG(MI_DMA_STATUS),
		{ /* Sentinel */ },
	};
	struct rkisp2_device *rkisp2 = m->private;

	return rkisp2_debug_dump_regs(rkisp2, m, 0, registers);
}
DEFINE_SHOW_ATTRIBUTE(rkisp2_debug_dump_core_regs);

static int rkisp2_debug_dump_isp_regs_show(struct seq_file *m, void *p)
{
	static const struct rkisp2_debug_register registers[] = {
		RKISP2_DEBUG_REG(ISP_CTRL),
		RKISP2_DEBUG_REG(ISP_ACQ_PROP),
		RKISP2_DEBUG_REG(ISP_FLAGS_SHD),
		RKISP2_DEBUG_REG(ISP_RIS),
		RKISP2_DEBUG_REG(ISP_ERR),
		RKISP2_DEBUG_SHD_REG(ISP_IS_H_OFFS),
		RKISP2_DEBUG_SHD_REG(ISP_IS_V_OFFS),
		RKISP2_DEBUG_SHD_REG(ISP_IS_H_SIZE),
		RKISP2_DEBUG_SHD_REG(ISP_IS_V_SIZE),
		{ /* Sentinel */ },
	};
	struct rkisp2_device *rkisp2 = m->private;

	return rkisp2_debug_dump_regs(rkisp2, m, 0, registers);
}
DEFINE_SHOW_ATTRIBUTE(rkisp2_debug_dump_isp_regs);

static int rkisp2_debug_dump_mi_mp_show(struct seq_file *m, void *p)
{
	static const struct rkisp2_debug_register registers[] = {
		RKISP2_DEBUG_REG(MI_MP_Y_BASE_AD_INIT),
		RKISP2_DEBUG_REG(MI_MP_Y_BASE_AD_INIT2),
		RKISP2_DEBUG_REG(MI_MP_Y_BASE_AD_SHD),
		RKISP2_DEBUG_REG(MI_MP_Y_SIZE_INIT),
		RKISP2_DEBUG_REG(MI_MP_Y_SIZE_INIT),
		RKISP2_DEBUG_REG(MI_MP_Y_SIZE_SHD),
		RKISP2_DEBUG_REG(MI_MP_Y_OFFS_CNT_SHD),
		{ /* Sentinel */ },
	};
	struct rkisp2_device *rkisp2 = m->private;

	return rkisp2_debug_dump_regs(rkisp2, m, 0, registers);
}
DEFINE_SHOW_ATTRIBUTE(rkisp2_debug_dump_mi_mp);

#define RKISP2_DEBUG_DATA_COUNT_BINS	32
#define RKISP2_DEBUG_DATA_COUNT_STEP	(4096 / RKISP2_DEBUG_DATA_COUNT_BINS)

static int rkisp2_debug_input_status_show(struct seq_file *m, void *p)
{
	struct rkisp2_device *rkisp2 = m->private;
	u16 data_count[RKISP2_DEBUG_DATA_COUNT_BINS] = { };
	unsigned int hsync_count = 0;
	unsigned int vsync_count = 0;
	unsigned int i;
	u32 data;
	u32 val;
	int ret;

	ret = pm_runtime_get_if_in_use(rkisp2->dev);
	if (ret <= 0)
		return ret ? : -ENODATA;

	/* Sample the ISP input port status 10000 times with a 1µs interval. */
	for (i = 0; i < 10000; ++i) {
		val = rkisp2_read(rkisp2, RKISP2_CIF_ISP_FLAGS_SHD);

		data = (val & RKISP2_CIF_ISP_FLAGS_SHD_S_DATA_MASK)
		     >> RKISP2_CIF_ISP_FLAGS_SHD_S_DATA_SHIFT;
		data_count[data / RKISP2_DEBUG_DATA_COUNT_STEP]++;

		if (val & RKISP2_CIF_ISP_FLAGS_SHD_S_HSYNC)
			hsync_count++;
		if (val & RKISP2_CIF_ISP_FLAGS_SHD_S_VSYNC)
			vsync_count++;

		udelay(1);
	}

	pm_runtime_put(rkisp2->dev);

	seq_printf(m, "vsync: %u, hsync: %u\n", vsync_count, hsync_count);
	seq_puts(m, "data:\n");
	for (i = 0; i < ARRAY_SIZE(data_count); ++i)
		seq_printf(m, "- [%04u:%04u]: %u\n",
			   i * RKISP2_DEBUG_DATA_COUNT_STEP,
			   (i + 1) * RKISP2_DEBUG_DATA_COUNT_STEP - 1,
			   data_count[i]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rkisp2_debug_input_status);

void rkisp2_debug_init(struct rkisp2_device *rkisp2)
{
	struct rkisp2_debug *debug = &rkisp2->debug;
	struct dentry *regs_dir;

	debug->debugfs_dir = debugfs_create_dir(dev_name(rkisp2->dev), NULL);

	debugfs_create_ulong("data_loss", 0444, debug->debugfs_dir,
			     &debug->data_loss);
	debugfs_create_ulong("outform_size_err", 0444,  debug->debugfs_dir,
			     &debug->outform_size_error);
	debugfs_create_ulong("img_stabilization_size_error", 0444,
			     debug->debugfs_dir,
			     &debug->img_stabilization_size_error);
	debugfs_create_ulong("inform_size_error", 0444,  debug->debugfs_dir,
			     &debug->inform_size_error);
	debugfs_create_ulong("irq_delay", 0444,  debug->debugfs_dir,
			     &debug->irq_delay);
	debugfs_create_ulong("mipi_error", 0444, debug->debugfs_dir,
			     &debug->mipi_error);
	debugfs_create_ulong("stats_error", 0444, debug->debugfs_dir,
			     &debug->stats_error);
	debugfs_create_ulong("stats3a_irq", 0444, debug->debugfs_dir,
			     &debug->stats3a_irq);
	debugfs_create_ulong("mp_stop_timeout", 0444, debug->debugfs_dir,
			     &debug->stop_timeout[RKISP2_MAINPATH]);
	debugfs_create_ulong("sp_stop_timeout", 0444, debug->debugfs_dir,
			     &debug->stop_timeout[RKISP2_SELFPATH]);
	debugfs_create_ulong("mp_frame_drop", 0444, debug->debugfs_dir,
			     &debug->frame_drop[RKISP2_MAINPATH]);
	debugfs_create_ulong("sp_frame_drop", 0444, debug->debugfs_dir,
			     &debug->frame_drop[RKISP2_SELFPATH]);
	debugfs_create_ulong("complete_frames", 0444, debug->debugfs_dir,
			     &debug->complete_frames);
	debugfs_create_ulong("stats3a_hist_ch0_count", 0444, debug->debugfs_dir,
			     &debug->stats3a_hist_ch0_count);
	debugfs_create_ulong("stats3a_hist_ch1_count", 0444, debug->debugfs_dir,
			     &debug->stats3a_hist_ch1_count);
	debugfs_create_ulong("stats3a_hist_ch2_count", 0444, debug->debugfs_dir,
			     &debug->stats3a_hist_ch2_count);
	debugfs_create_ulong("stats3a_hist_big_count", 0444, debug->debugfs_dir,
			     &debug->stats3a_hist_big_count);
	debugfs_create_ulong("stats3a_awb_count", 0444, debug->debugfs_dir,
			     &debug->stats3a_awb_count);
	debugfs_create_ulong("stats3a_awb_done_count", 0444, debug->debugfs_dir,
			     &debug->stats3a_awb_done_count);
	debugfs_create_file("input_status", 0444, debug->debugfs_dir, rkisp2,
			    &rkisp2_debug_input_status_fops);

	regs_dir = debugfs_create_dir("regs", debug->debugfs_dir);

	debugfs_create_file("core", 0444, regs_dir, rkisp2,
			    &rkisp2_debug_dump_core_regs_fops);
	debugfs_create_file("isp", 0444, regs_dir, rkisp2,
			    &rkisp2_debug_dump_isp_regs_fops);
	debugfs_create_file("mi_mp", 0444, regs_dir, rkisp2,
			    &rkisp2_debug_dump_mi_mp_fops);
}

void rkisp2_debug_cleanup(struct rkisp2_device *rkisp2)
{
	debugfs_remove_recursive(rkisp2->debug.debugfs_dir);
}
