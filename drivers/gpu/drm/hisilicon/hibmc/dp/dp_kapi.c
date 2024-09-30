// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/io.h>
#include <linux/delay.h>
#include "dp_config.h"
#include "dp_comm.h"
#include "dp_reg.h"
#include "dp_kapi.h"
#include "dp_link.h"

#define DP_LINK_RATE_HBR 0xa

static int hibmc_dp_link_init(struct hibmc_dp_dev *dp)
{
	dp->link.cap.lanes = 2;
	dp->link.train_set = devm_kzalloc(dp->dev->dev,
					  dp->link.cap.lanes * sizeof(u8), GFP_KERNEL);
	if (!dp->link.train_set)
		return -ENOMEM;

	dp->link.cap.link_rate = DP_LINK_RATE_HBR;

	return 0;
}

static void hibmc_dp_aux_init(struct hibmc_dp_dev *dp)
{
	struct hibmc_dp_aux *aux = &dp->aux;

	aux->addr = dp->base;
	/* aux read/write lock */
	mutex_init(&aux->lock);
	dp_write_bits(aux->addr + DP_AUX_REQ, DP_CFG_AUX_SYNC_LEN_SEL, 0x0);
	dp_write_bits(aux->addr + DP_AUX_REQ, DP_CFG_AUX_TIMER_TIMEOUT, 0x1);
	dp_write_bits(aux->addr + DP_AUX_REQ, DP_CFG_AUX_MIN_PULSE_NUM, DP_MIN_PULSE_NUM);
}

static void hibmc_dp_aux_uninit(struct hibmc_dp_dev *dp)
{
	struct hibmc_dp_aux *aux = &dp->aux;

	mutex_destroy(&aux->lock);
}

static void hibmc_dp_set_tu(struct hibmc_dp_dev *dp, struct dp_mode *mode)
{
	u32 tu_symbol_frac_size;
	u32 tu_symbol_size;
	u64 pixel_clock;
	u64 rate_ks;
	u8 lane_num;
	u32 value;
	u32 bpp;

	pixel_clock = mode->pixel_clock;
	lane_num = dp->link.cap.lanes;
	if (lane_num == 0) {
		drm_err(dp->dev, "set tu failed, lane num cannot be 0!\n");
		return;
	}

	bpp = DP_BPP;
	rate_ks = dp->link.cap.link_rate * DP_LINK_RATE_CAL;
	value = (pixel_clock * bpp * 5000) / (61 * lane_num * rate_ks);

	if (value % 10 == 9) { /* 10: div, 9: carry */
		tu_symbol_size = value / 10 + 1; /* 10: div */
		tu_symbol_frac_size = 0;
	} else {
		tu_symbol_size = value / 10; /* 10: div */
		tu_symbol_frac_size = value % 10 + 1; /* 10: div */
	}

	drm_info(dp->dev, "tu value: %u.%u value: %u\n",
		 tu_symbol_size, tu_symbol_frac_size, value);

	dp_write_bits(dp->base + DP_VIDEO_PACKET,
		      DP_CFG_STREAM_TU_SYMBOL_SIZE, tu_symbol_size);
	dp_write_bits(dp->base + DP_VIDEO_PACKET,
		      DP_CFG_STREAM_TU_SYMBOL_FRAC_SIZE, tu_symbol_frac_size);
}

static void hibmc_dp_set_sst(struct hibmc_dp_dev *dp, struct dp_mode *mode)
{
	u32 hblank_size;
	u32 htotal_size;
	u32 htotal_int;
	u32 hblank_int;
	u32 fclk; /* flink_clock */

	fclk = dp->link.cap.link_rate * DP_LINK_RATE_CAL;

	/* ssc: 9947 / 10000 = 0.9947 */
	htotal_int = mode->h_total * 9947 / 10000;
	htotal_size = (u32)((u64)htotal_int * fclk / (DP_SYMBOL_PER_FCLK * mode->pixel_clock));

	/* ssc: max effect bandwidth 53 / 10000 = 0.53% */
	hblank_int = mode->h_blank - mode->h_active * 53 / 10000;
	hblank_size = (u64)hblank_int * fclk * 9947 /
		      ((u64)mode->pixel_clock * 10000 * DP_SYMBOL_PER_FCLK);

	drm_info(dp->dev, "h_active %u v_active %u htotal_size %u hblank_size %u",
		 mode->h_active, mode->v_active, htotal_size, hblank_size);
	drm_info(dp->dev, "flink_clock %u pixel_clock %u", fclk, mode->pixel_clock);

	dp_write_bits(dp->base + DP_VIDEO_HORIZONTAL_SIZE, DP_CFG_STREAM_HTOTAL_SIZE, htotal_size);
	dp_write_bits(dp->base + DP_VIDEO_HORIZONTAL_SIZE, DP_CFG_STREAM_HBLANK_SIZE, hblank_size);
}

static void hibmc_dp_link_cfg(struct hibmc_dp_dev *dp, struct dp_mode *mode)
{
	u32 timing_delay;
	u32 vblank;
	u32 hstart;
	u32 vstart;

	vblank = mode->v_total - mode->v_active;
	timing_delay = mode->h_sync + mode->h_back;
	hstart = mode->h_sync + mode->h_back;
	vstart = mode->v_sync + mode->v_back;

	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG0,
		      DP_CFG_TIMING_GEN0_HBLANK, mode->h_blank);
	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG0,
		      DP_CFG_TIMING_GEN0_HACTIVE, mode->h_active);

	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG2,
		      DP_CFG_TIMING_GEN0_VBLANK, vblank);
	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG2,
		      DP_CFG_TIMING_GEN0_VACTIVE, mode->v_active);
	dp_write_bits(dp->base + DP_TIMING_GEN_CONFIG3,
		      DP_CFG_TIMING_GEN0_VFRONT_PORCH, mode->v_front);

	dp_write_bits(dp->base + DP_VIDEO_CONFIG0,
		      DP_CFG_STREAM_HACTIVE, mode->h_active);
	dp_write_bits(dp->base + DP_VIDEO_CONFIG0,
		      DP_CFG_STREAM_HBLANK, mode->h_blank);
	dp_write_bits(dp->base + DP_VIDEO_CONFIG2,
		      DP_CFG_STREAM_HSYNC_WIDTH, mode->h_sync);

	dp_write_bits(dp->base + DP_VIDEO_CONFIG1,
		      DP_CFG_STREAM_VACTIVE, mode->v_active);
	dp_write_bits(dp->base + DP_VIDEO_CONFIG1,
		      DP_CFG_STREAM_VBLANK, vblank);
	dp_write_bits(dp->base + DP_VIDEO_CONFIG3,
		      DP_CFG_STREAM_VFRONT_PORCH, mode->v_front);
	dp_write_bits(dp->base + DP_VIDEO_CONFIG3,
		      DP_CFG_STREAM_VSYNC_WIDTH, mode->v_sync);

	dp_write_bits(dp->base + DP_VIDEO_MSA0,
		      DP_CFG_STREAM_VSTART, vstart);
	dp_write_bits(dp->base + DP_VIDEO_MSA0,
		      DP_CFG_STREAM_HSTART, hstart);

	dp_write_bits(dp->base + DP_VIDEO_CTRL,
		      DP_CFG_STREAM_VSYNC_POLARITY, mode->v_pol);
	dp_write_bits(dp->base + DP_VIDEO_CTRL,
		      DP_CFG_STREAM_HSYNC_POLARITY, mode->h_pol);

	/* MSA mic 0 and 1*/
	writel(DP_MSA1, dp->base + DP_VIDEO_MSA1);
	writel(DP_MSA2, dp->base + DP_VIDEO_MSA2);

	hibmc_dp_set_tu(dp, mode);

	dp_write_bits(dp->base + DP_VIDEO_CTRL, DP_CFG_STREAM_RGB_ENABLE, 0x1);
	dp_write_bits(dp->base + DP_VIDEO_CTRL, DP_CFG_STREAM_VIDEO_MAPPING, 0);

	/*divide 2: up even */
	if (timing_delay % 2)
		timing_delay++;

	dp_write_bits(dp->base + DP_TIMING_MODEL_CTRL,
		      DP_CFG_PIXEL_NUM_TIMING_MODE_SEL1, timing_delay);

	hibmc_dp_set_sst(dp, mode);
}

int hibmc_dp_kapi_init(struct hibmc_dp *dp)
{
	struct drm_device *drm_dev = dp->drm_dev;
	struct hibmc_dp_dev *dp_dev;
	int ret;

	dp_dev = devm_kzalloc(drm_dev->dev, sizeof(struct hibmc_dp_dev), GFP_KERNEL);
	if (!dp_dev)
		return -ENOMEM;

	dp->dp_dev = dp_dev;

	dp_dev->dev = drm_dev;
	dp_dev->base = dp->mmio + DP_OFFSET;

	hibmc_dp_aux_init(dp_dev);

	ret = hibmc_dp_link_init(dp_dev);
	if (ret) {
		drm_err(drm_dev, "dp link init failed\n");
		hibmc_dp_aux_uninit(dp_dev);
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

void hibmc_dp_kapi_uninit(struct hibmc_dp *dp)
{
	hibmc_dp_aux_uninit(dp->dp_dev);
}

void hibmc_dp_display_en(struct hibmc_dp *dp, bool enable)
{
	struct hibmc_dp_dev *dp_dev = dp->dp_dev;

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

int hibmc_dp_mode_set(struct hibmc_dp *dp, struct dp_mode *mode)
{
	struct hibmc_dp_dev *dp_dev = dp->dp_dev;
	int ret;

	if (!dp_dev->link.status.channel_equalized) {
		ret = dp_link_training(dp_dev);
		if (ret) {
			drm_err(dp->drm_dev, "dp link training failed, ret: %d\n", ret);
			return ret;
		}
	}

	hibmc_dp_display_en(dp, false);
	hibmc_dp_link_cfg(dp_dev, mode);

	return 0;
}
