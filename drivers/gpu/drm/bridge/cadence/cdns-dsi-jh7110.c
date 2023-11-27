// SPDX-License-Identifier: GPL-2.0
/*
 * jh7110 soc Cadence DSI wrapper
 *
 * Copyright (C) 2023 StarFive Technology Co., Ltd.
 */

#include <linux/io.h>

#include "cdns-dsi-jh7110.h"
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/iopoll.h>

static int cdns_dsi_clock_enable(struct cdns_dsi *dsi, struct device *dev)
{
	int ret;

	ret = clk_prepare_enable(dsi->apb_clk);
	if (ret) {
		dev_err(dev, "failed to prepare/enable apb_clk\n");
		return ret;
	}
	ret = clk_prepare_enable(dsi->txesc_clk);
	if (ret) {
		dev_err(dev, "failed to prepare/enable txesc_clk\n");
		return ret;
	}

	return ret;
}

static void  cdns_dsi_clock_disable(struct cdns_dsi *dsi)
{
	clk_disable_unprepare(dsi->apb_clk);
	clk_disable_unprepare(dsi->txesc_clk);
}

static int cdns_dsi_resets_deassert(struct cdns_dsi *dsi, struct device *dev)
{
	int ret;

	ret = reset_control_deassert(dsi->apb_rst);
	if (ret < 0) {
		dev_err(dev, "failed to deassert apb_rst\n");
		return ret;
	}

	ret = reset_control_deassert(dsi->txesc_rst);
	if (ret < 0) {
		dev_err(dev, "failed to deassert txesc_rst\n");
		return ret;
	}

	ret = reset_control_deassert(dsi->dpi_rst);
	if (ret < 0) {
		dev_err(dev, "failed to deassert dpi_rst\n");
		return ret;
	}

	return ret;
}

static int cdns_dsi_resets_assert(struct cdns_dsi *dsi, struct device *dev)
{
	int ret;

	ret = reset_control_assert(dsi->apb_rst);
	if (ret < 0) {
		dev_err(dev, "failed to assert apb_rst\n");
		return ret;
	}
	ret = reset_control_assert(dsi->txesc_rst);
	if (ret < 0) {
		dev_err(dev, "failed to assert txesc_rst\n");
		return ret;
	}

	ret = reset_control_assert(dsi->dpi_rst);
	if (ret < 0) {
		dev_err(dev, "failed to assert dpi_rst\n");
		return ret;
	}

	return ret;
}

static int cdns_dsi_get_clock(struct device *dev, struct cdns_dsi *dsi)
{
	dsi->apb_clk = devm_clk_get(dev, "apb");
	if (IS_ERR(dsi->apb_clk))
		return PTR_ERR(dsi->apb_clk);

	dsi->txesc_clk = devm_clk_get(dev, "txesc");
	if (IS_ERR(dsi->txesc_clk))
		return PTR_ERR(dsi->txesc_clk);

	return 0;
}

static int cdns_dsi_get_reset(struct device *dev, struct cdns_dsi *dsi)
{
	dsi->dpi_rst = devm_reset_control_get(dev, "dsi_dpi");
	if (IS_ERR(dsi->dpi_rst))
		return PTR_ERR(dsi->dpi_rst);

	dsi->apb_rst = devm_reset_control_get(dev, "dsi_apb");
	if (IS_ERR(dsi->apb_rst))
		return PTR_ERR(dsi->apb_rst);

	dsi->txesc_rst = devm_reset_control_get(dev, "dsi_txesc");
	if (IS_ERR(dsi->txesc_rst))
		return PTR_ERR(dsi->txesc_rst);

	dsi->txbytehs_rst = devm_reset_control_get(dev, "dsi_txbytehs");
	if (IS_ERR(dsi->txbytehs_rst))
		return PTR_ERR(dsi->txbytehs_rst);

	return 0;
}

static int cdns_dsi_jh7110_init(struct cdns_dsi *dsi)
{
	int ret;

	ret = cdns_dsi_get_clock(dsi->base.dev, dsi);
	if (ret)
		return ret;

	ret = cdns_dsi_get_reset(dsi->base.dev, dsi);
	return ret;
}

static void cdns_dsi_jh7110_resume(struct cdns_dsi *dsi)
{
	int ret;

	ret = cdns_dsi_clock_enable(dsi, dsi->base.dev);
	if (ret) {
		dev_err(dsi->base.dev, "failed to enable clock\n");
		return;
	}
	ret = cdns_dsi_resets_deassert(dsi, dsi->base.dev);
	if (ret < 0) {
		dev_err(dsi->base.dev, "failed to deassert reset\n");
		return;
	}
}

static void cdns_dsi_jh7110_suspend(struct cdns_dsi *dsi)
{
	int ret;

	ret = cdns_dsi_resets_assert(dsi, dsi->base.dev);
	if (ret < 0) {
		dev_err(dsi->base.dev, "failed to deassert reset\n");
		return;
	}

	cdns_dsi_clock_disable(dsi);
}

static void dpi_get_metrics(const struct dpi_params *dpi, struct dpi_metrics *m)
{
	unsigned int total_lines = dpi->vsync_len + dpi->vback_porch +
			       dpi->vactive + dpi->vfront_porch;
	m->pixels_one_line = dpi->hsync_len + dpi->hback_porch +
			     dpi->hactive + dpi->hfront_porch;
	m->fps = (double)dpi->pixelclock / m->pixels_one_line / total_lines;
	m->pixelclock_period = (unsigned long)NSEC_PER_SEC /
			       ((unsigned long)dpi->pixelclock / 1000);
	m->hact_time = m->pixelclock_period * dpi->hactive;
	m->hfp_time = m->pixelclock_period * dpi->hfront_porch;
	m->hbp_time = m->pixelclock_period * dpi->hback_porch;
	m->hsa_time = m->pixelclock_period * dpi->hsync_len;
	m->one_line_time = m->pixelclock_period * m->pixels_one_line;
}

static int gen_dsi_timing(const struct dpi_params *dpi, int lanes,
			  unsigned long bitrate, struct dsi_params *dsi,
			  const struct calc_ctrl *ctrl)
{
	unsigned long pixel_bytes = dpi->bpp / 8;
	unsigned long pixels_hblk = dpi->hback_porch + dpi->hfront_porch + dpi->hsync_len;

	dsi->dlanes = lanes;
	dsi->bitrate = bitrate;

	// 1. HACT(WC) = Active pixels per line * Bits per pixel/8
	//    VACT = Active lines per frame
	unsigned long hact_wc = dpi->hactive * pixel_bytes;
	unsigned long vact = dpi->vactive;

	dsi->hact = hact_wc;
	dsi->vact = vact;

	/* 2. Get total line-time in pixel clock */
	unsigned long pixelclock_period = NSEC_PER_SEC /
					  (dpi->pixelclock / 1000);
	unsigned long pixels_in_one_line = dpi->hactive + pixels_hblk;
	unsigned long total_line_time = pixelclock_period * pixels_in_one_line;

	/* 3. Calculate blanking time */
	unsigned long byteclock = bitrate / 8;
	unsigned long byteclock_period = NSEC_PER_SEC / (byteclock / 1000);
	unsigned long hact_duration = dpi->hactive * pixel_bytes *
				      byteclock_period / lanes;
	unsigned long blanking_time = total_line_time - hact_duration;
	unsigned long blanking_wc = blanking_time * lanes / byteclock_period;

	/*
	 * 4. Get timing parameter based on Video mode
	 * Video mode: Sync Pulses
	 * One line is composed of HSS +HSA + HSE + HBP + HACT + HFP
	 * HSS/HSE -> Short packet -> 4 bytes
	 * HSA/HBP/HACT/HFP -> Long packet -> 4 bytes header + Payload + 2 bytes CRC
	 * Total of 2*4 + 4*6 = 32 bytes are covered in header and footer
	 * Available blanking WC = 1210- 32 = 1178
	 */
	unsigned long avail_blanking_wc = blanking_wc - 32;

	/*
	 * 5. Divide the total available WC across available blanking parameters HSA,HBP & HFP.
	 * The MIPI specification does not define the ratio. However, some
	 * may have specific requirements. Hence, please consult the data sheet for
	 * your display.
	 */
	struct dsi_hblk_ratio hblk_ratio = {
	    .den = dpi->hsync_len + dpi->hback_porch + dpi->hfront_porch,
	    .hsa_num = dpi->hsync_len,
	    .hbp_num = dpi->hback_porch,
	    .hfp_num = dpi->hfront_porch,
	};
	if (ctrl->r_hsa && ctrl->r_hbp && ctrl->r_hfp) {
		/* The following is implemented based on
		 * MIPI DSI Transmitter Subsystem v2.3 Page 30
		 */
		hblk_ratio.den = ctrl->r_hsa + ctrl->r_hbp + ctrl->r_hfp;
		hblk_ratio.hsa_num = ctrl->r_hsa;
		hblk_ratio.hbp_num = ctrl->r_hbp;
		hblk_ratio.hfp_num = ctrl->r_hfp;

		dsi->hsa = DIV_ROUND_UP(avail_blanking_wc * hblk_ratio.hsa_num, hblk_ratio.den);
		dsi->hbp = DIV_ROUND_UP(avail_blanking_wc * hblk_ratio.hbp_num, hblk_ratio.den);
		dsi->hfp = avail_blanking_wc - dsi->hsa - dsi->hbp;

		dsi->hsa += DSI_HSA_FRAME_OVERHEAD;
		dsi->hbp += DSI_HBP_FRAME_OVERHEAD;
		dsi->hfp += DSI_HFP_FRAME_OVERHEAD;
	} else {
		/* The following is implemented based on
		 * MIPI MIPI_DSI_v1.3.1_Host_Controller_User_Guide_v1p09.pdf
		 * page 95
		 */
		dsi->hsa = dpi->hsync_len * dpi->bpp / 8;
		dsi->hbp = dpi->hback_porch * dpi->bpp / 8;
		dsi->hfp = blanking_wc - dsi->hsa - dsi->hbp;
	}

	/* vertical blanking lines */
	dsi->vbp = dpi->vback_porch;
	dsi->vfp = dpi->vfront_porch;
	dsi->vsa = dpi->vsync_len;

	return 0;
}

static int dsi_get_metrics(const struct dsi_params *dsi, struct dsi_metrics *m)
{
	if (!dsi || !m)
		return -1;

	m->bytes_one_line	= dsi->hsa + dsi->hbp + dsi->hact + dsi->hfp;
	m->byteclock		= dsi->bitrate / 8;
	m->byteclock_period	= (unsigned long)NSEC_PER_SEC /
						  ((unsigned long)m->byteclock / 1000);
	m->hsa_hbp_time		= DIV_ROUND_UP((dsi->hsa + dsi->hbp), dsi->dlanes) *
						  m->byteclock_period;
	m->hact_time		= DIV_ROUND_UP(dsi->hact, dsi->dlanes) *
						  m->byteclock_period;
	m->hfp_time			= DIV_ROUND_UP(dsi->hfp, dsi->dlanes) *
						  m->byteclock_period;
	m->one_line_time	= DIV_ROUND_UP(m->bytes_one_line, dsi->dlanes) *
						  m->byteclock_period;
	return 0;
}

static unsigned long dphy_adjust_bitrate(unsigned int dlanes,
					 unsigned long bitrate_alignment,
					 unsigned long bitrate_want)
{
	unsigned long bitrate = bitrate_want;

	/* align to dphy timing */
	if (bitrate_alignment > 0) {
		unsigned long reminder = bitrate % bitrate_alignment;

		if (reminder)
			bitrate += bitrate_alignment - reminder;
	}

	return bitrate;
}

static int calc_gen_dsi(const struct calc_ctrl *ctrl, struct dpi_params *dpi,
			struct dsi_params *dsi)
{
	struct dpi_metrics dpi_metrics;
	unsigned long bitrate_want;
	unsigned long bitrate;
	unsigned long abs_line_time_delta;
	struct dsi_metrics dsi_metrics;

	dpi_get_metrics(dpi, &dpi_metrics);

	bitrate_want = dpi->pixelclock / ctrl->dlanes * dpi->bpp;
	bitrate = dphy_adjust_bitrate(ctrl->dlanes, ctrl->bitrate_alignment,
				      bitrate_want);

	do {
		gen_dsi_timing(dpi, ctrl->dlanes, bitrate, dsi, ctrl);
		dsi_get_metrics(dsi, &dsi_metrics);
		abs_line_time_delta = dsi_metrics.one_line_time - dpi_metrics.one_line_time;
		if (dsi_metrics.one_line_time < dpi_metrics.one_line_time)
			abs_line_time_delta = dpi_metrics.one_line_time - dsi_metrics.one_line_time;

		if (abs_line_time_delta <= ctrl->line_time_tolerance)
			return 0;

		bitrate += ctrl->bitrate_alignment;
	} while (bitrate < ctrl->max_bitrate);

	return -1;
}

static void cdns_dsi_jh7110_update(struct cdns_dsi *dsi, struct cdns_dsi_cfg *dsi_cfg,
				   const struct drm_display_mode *mode)
{
	struct calc_ctrl ctrl = {
		.fps_tolerance       = 0.1f,
		.max_bitrate         = 1000000000,
		.min_bitrate         = 370000000,
		.bitrate_alignment   = 10000000,
		.dsi_video_mode      = DSI_Video_NonBurstPulse,
		.line_time_tolerance = 2000,
		.r_hsa = 2,
		.r_hbp = 2,
		.r_hfp = 2,
	};
	struct dsi_params dsi_p = {0};
	struct dpi_params dpi = {0};

	ctrl.dlanes = dsi->output.dev->lanes;

	dpi.bpp = mipi_dsi_pixel_format_to_bpp(dsi->output.dev->format);
	dpi.pixelclock = mode->clock * 1000;
	dpi.hactive = mode->hdisplay;
	dpi.hfront_porch = mode->hsync_start - mode->hdisplay;
	dpi.hback_porch = mode->htotal - mode->hsync_end;
	dpi.hsync_len = mode->hsync_end - mode->hsync_start;
	dpi.vactive = mode->vdisplay;
	dpi.vfront_porch = mode->vsync_start - mode->vdisplay;
	dpi.vback_porch = mode->vtotal - mode->vsync_end;
	dpi.vsync_len = mode->vsync_end - mode->vsync_start;

	if (!calc_gen_dsi(&ctrl, &dpi, &dsi_p)) {
		dsi_cfg->hbp = dsi_p.hbp - DSI_HBP_FRAME_OVERHEAD;
		dsi_cfg->hsa = dsi_p.hsa - DSI_HSA_FRAME_OVERHEAD;
		dsi_cfg->hfp = dsi_p.hfp - DSI_HFP_FRAME_OVERHEAD;
		dsi->output.phy_opts.mipi_dphy.hs_clk_rate = dsi_p.bitrate;
	}
}

static void jh7110_cdns_dsi_hs_init(struct cdns_dsi *dsi)
{
	cdns_dsi_hs_init(dsi);
	reset_control_deassert(dsi->txbytehs_rst);
}

const struct cdns_dsi_platform_ops dsi_ti_jh7110_ops = {
	.init = cdns_dsi_jh7110_init,
	.resume = cdns_dsi_jh7110_resume,
	.suspend = cdns_dsi_jh7110_suspend,
	.update = cdns_dsi_jh7110_update,
	.transfer = jh7110_cdns_dsi_hs_init,
};
