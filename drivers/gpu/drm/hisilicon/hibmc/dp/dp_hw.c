// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/io.h>
#include <linux/delay.h>
#include "dp_config.h"
#include "dp_comm.h"
#include "dp_reg.h"
#include "dp_hw.h"
#include "dp_link.h"
#include "dp_aux.h"

static int dp_link_init(struct dp_dev *dp)
{
	dp->link.cap.lanes = 2;
	dp->link.train_set = devm_kzalloc(dp->dev->dev,
					  dp->link.cap.lanes * sizeof(u8), GFP_KERNEL);
	if (!dp->link.train_set)
		return -ENOMEM;

	dp->link.cap.link_rate = 1;

	return 0;
}

static void dp_set_tu(struct dp_dev *dp, struct drm_display_mode *mode)
{
	u32 tu_symbol_frac_size;
	u32 tu_symbol_size;
	u64 rate_ks;
	u8 lane_num;
	u32 value;
	u32 bpp;

	lane_num = dp->link.cap.lanes;
	if (lane_num == 0) {
		drm_err(dp->dev, "set tu failed, lane num cannot be 0!\n");
		return;
	}

	bpp = DP_BPP;
	rate_ks = dp_get_link_rate(dp->link.cap.link_rate) * DP_LINK_RATE_CAL;
	value = (mode->clock * bpp * 5) / (61 * lane_num * rate_ks);

	if (value % 10 == 9) { /* 9 carry */
		tu_symbol_size = value / 10 + 1;
		tu_symbol_frac_size = 0;
	} else {
		tu_symbol_size = value / 10;
		tu_symbol_frac_size = value % 10 + 1;
	}

	drm_info(dp->dev, "tu value: %u.%u value: %u\n",
		 tu_symbol_size, tu_symbol_frac_size, value);

	dp_write_bits(dp->base + DP_VIDEO_PACKET,
		      DP_CFG_STREAM_TU_SYMBOL_SIZE, tu_symbol_size);
	dp_write_bits(dp->base + DP_VIDEO_PACKET,
		      DP_CFG_STREAM_TU_SYMBOL_FRAC_SIZE, tu_symbol_frac_size);
}

static void dp_set_sst(struct dp_dev *dp, struct drm_display_mode *mode)
{
	u32 hblank_size;
	u32 htotal_size;
	u32 htotal_int;
	u32 hblank_int;
	u32 fclk; /* flink_clock */

	fclk = dp_get_link_rate(dp->link.cap.link_rate) * DP_LINK_RATE_CAL;

	/* ssc: 9947 / 10000 = 0.9947 */
	htotal_int = mode->htotal * 9947 / 10000;
	htotal_size = (u32)((u64)htotal_int * fclk / (DP_SYMBOL_PER_FCLK * (mode->clock / 1000)));

	/* ssc: max effect bandwidth 53 / 10000 = 0.53% */
	hblank_int = (mode->htotal - mode->hdisplay) - mode->hdisplay * 53 / 10000;
	hblank_size = (u64)hblank_int * fclk * 9947 /
		      (mode->clock * 10 * DP_SYMBOL_PER_FCLK);

	drm_info(dp->dev, "h_active %u v_active %u htotal_size %u hblank_size %u",
		 mode->hdisplay, mode->vdisplay, htotal_size, hblank_size);
	drm_info(dp->dev, "flink_clock %u pixel_clock %d", fclk, (mode->clock / 1000));

	dp_write_bits(dp->base + DP_VIDEO_HORIZONTAL_SIZE, DP_CFG_STREAM_HTOTAL_SIZE, htotal_size);
	dp_write_bits(dp->base + DP_VIDEO_HORIZONTAL_SIZE, DP_CFG_STREAM_HBLANK_SIZE, hblank_size);
}

static void dp_link_cfg(struct dp_dev *dp, struct drm_display_mode *mode)
{
	u32 timing_delay;
	u32 vblank;
	u32 hstart;
	u32 vstart;

	vblank = mode->vtotal - mode->vdisplay;
	timing_delay = mode->htotal - mode->hsync_start;
	hstart = mode->htotal - mode->hsync_start;
	vstart = mode->vtotal - mode->vsync_start;

	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG0,
		      DP_CFG_TIMING_GEN0_HBLANK, (mode->htotal - mode->hdisplay));
	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG0,
		      DP_CFG_TIMING_GEN0_HACTIVE, mode->hdisplay);

	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG2,
		      DP_CFG_TIMING_GEN0_VBLANK, vblank);
	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG2,
		      DP_CFG_TIMING_GEN0_VACTIVE, mode->vdisplay);
	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG3,
		      DP_CFG_TIMING_GEN0_VFRONT_PORCH, (mode->vsync_start - mode->vdisplay));

	dp_write_bits(dp->base + DP_VIDEO_CONFIG0,
		      DP_CFG_STREAM_HACTIVE, mode->hdisplay);
	dp_write_bits(dp->base + DP_VIDEO_CONFIG0,
		      DP_CFG_STREAM_HBLANK, (mode->htotal - mode->hdisplay));
	dp_write_bits(dp->base + DP_VIDEO_CONFIG2,
		      DP_CFG_STREAM_HSYNC_WIDTH, (mode->hsync_end - mode->hsync_start));

	dp_write_bits(dp->base + DP_VIDEO_CONFIG1,
		      DP_CFG_STREAM_VACTIVE, mode->vdisplay);
	dp_write_bits(dp->base + DP_VIDEO_CONFIG1,
		      DP_CFG_STREAM_VBLANK, vblank);
	dp_write_bits(dp->base + DP_VIDEO_CONFIG3,
		      DP_CFG_STREAM_VFRONT_PORCH, (mode->vsync_start - mode->vdisplay));
	dp_write_bits(dp->base + DP_VIDEO_CONFIG3,
		      DP_CFG_STREAM_VSYNC_WIDTH, (mode->vsync_end - mode->vsync_start));

	dp_write_bits(dp->base + DP_VIDEO_MSA0,
		      DP_CFG_STREAM_VSTART, vstart);
	dp_write_bits(dp->base + DP_VIDEO_MSA0,
		      DP_CFG_STREAM_HSTART, hstart);

	dp_write_bits(dp->base + DP_VIDEO_CTRL,
		      DP_CFG_STREAM_VSYNC_POLARITY,
		      mode->flags & DRM_MODE_FLAG_PVSYNC ? 1 : 0);
	dp_write_bits(dp->base + DP_VIDEO_CTRL,
		      DP_CFG_STREAM_HSYNC_POLARITY,
		      mode->flags & DRM_MODE_FLAG_PHSYNC ? 1 : 0);

	/* MSA mic 0 and 1 */
	writel(DP_MSA1, dp->base + DP_VIDEO_MSA1);
	writel(DP_MSA2, dp->base + DP_VIDEO_MSA2);

	dp_set_tu(dp, mode);

	dp_write_bits(dp->base + DP_VIDEO_CTRL, DP_CFG_STREAM_RGB_ENABLE, 0x1);
	dp_write_bits(dp->base + DP_VIDEO_CTRL, DP_CFG_STREAM_VIDEO_MAPPING, 0);

	/* divide 2: up even */
	if (timing_delay - timing_delay / 2 * 2)
		timing_delay++;

	dp_write_bits(dp->base + DP_TIMING_MODEL_CTRL,
		      DP_CFG_PIXEL_NUM_TIMING_MODE_SEL1, timing_delay);

	dp_set_sst(dp, mode);
}

int dp_hw_init(struct hibmc_dp *dp)
{
	struct drm_device *drm_dev = dp->drm_dev;
	struct dp_dev *dp_dev;
	int ret;

	dp_dev = devm_kzalloc(drm_dev->dev, sizeof(struct dp_dev), GFP_KERNEL);
	if (!dp_dev)
		return -ENOMEM;

	dp->dp_dev = dp_dev;

	dp_dev->dev = drm_dev;
	dp_dev->base = dp->mmio + DP_OFFSET;

	dp_aux_init(dp_dev);

	ret = dp_link_init(dp_dev);
	if (ret) {
		drm_err(drm_dev, "dp link init failed\n");
		return ret;
	}

	/* hdcp data */
	writel(DP_HDCP, dp_dev->base + DP_HDCP_CFG);
	/* int init */
	writel(0, dp_dev->base + DP_INTR_ENABLE);
	writel(DP_INT_RST, dp_dev->base + DP_INTR_ORIGINAL_STATUS);
	/* rst */
	writel(DP_DPTX_RST, dp_dev->base + DP_DPTX_RST_CTRL);
	/* clock enable */
	writel(DP_CLK_EN, dp_dev->base + DP_DPTX_CLK_CTRL);

	return 0;
}

void dp_hw_uninit(struct hibmc_dp *dp)
{
	// keep this uninit interface in the future use
}

void dp_display_en(struct hibmc_dp *dp, bool enable)
{
	struct dp_dev *dp_dev = dp->dp_dev;

	if (enable) {
		dp_write_bits(dp_dev->base + DP_VIDEO_CTRL, BIT(0), 0x1);
		writel(DP_SYNC_EN_MASK, dp_dev->base + DP_TIMING_SYNC_CTRL);
		dp_write_bits(dp_dev->base + DP_DPTX_GCTL0, BIT(10), 0x1);
		writel(DP_SYNC_EN_MASK, dp_dev->base + DP_TIMING_SYNC_CTRL);
	} else {
		dp_write_bits(dp_dev->base + DP_DPTX_GCTL0, BIT(10), 0);
		writel(DP_SYNC_EN_MASK, dp_dev->base + DP_TIMING_SYNC_CTRL);
		dp_write_bits(dp_dev->base + DP_VIDEO_CTRL, BIT(0), 0);
		writel(DP_SYNC_EN_MASK, dp_dev->base + DP_TIMING_SYNC_CTRL);
	}

	msleep(50);
}

int dp_mode_set(struct hibmc_dp *dp, struct drm_display_mode *mode)
{
	struct dp_dev *dp_dev = dp->dp_dev;
	int ret;

	if (!dp_dev->link.status.channel_equalized) {
		ret = dp_link_training(dp_dev);
		if (ret) {
			drm_err(dp->drm_dev, "dp link training failed, ret: %d\n", ret);
			return ret;
		}
	}

	dp_display_en(dp, false);
	dp_link_cfg(dp_dev, mode);

	return 0;
}
