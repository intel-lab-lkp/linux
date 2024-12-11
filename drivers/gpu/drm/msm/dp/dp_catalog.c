// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"[drm-dp] %s: " fmt, __func__

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/rational.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/drm_print.h>

#include "dp_catalog.h"
#include "dp_reg.h"

#define POLLING_SLEEP_US			1000
#define POLLING_TIMEOUT_US			10000

#define DP_INTERRUPT_STATUS_ACK_SHIFT	1
#define DP_INTERRUPT_STATUS_MASK_SHIFT	2

#define DP_INTF_CONFIG_DATABUS_WIDEN     BIT(4)

#define DP_INTERRUPT_STATUS1 \
	(DP_INTR_AUX_XFER_DONE| \
	DP_INTR_WRONG_ADDR | DP_INTR_TIMEOUT | \
	DP_INTR_NACK_DEFER | DP_INTR_WRONG_DATA_CNT | \
	DP_INTR_I2C_NACK | DP_INTR_I2C_DEFER | \
	DP_INTR_PLL_UNLOCKED | DP_INTR_AUX_ERROR)

#define DP_INTERRUPT_STATUS1_ACK \
	(DP_INTERRUPT_STATUS1 << DP_INTERRUPT_STATUS_ACK_SHIFT)
#define DP_INTERRUPT_STATUS1_MASK \
	(DP_INTERRUPT_STATUS1 << DP_INTERRUPT_STATUS_MASK_SHIFT)

#define DP_INTERRUPT_STATUS2 \
	(DP_INTR_READY_FOR_VIDEO | DP_INTR_IDLE_PATTERN_SENT | \
	DP_INTR_FRAME_END | DP_INTR_CRC_UPDATED)

#define DP_INTERRUPT_STATUS2_ACK \
	(DP_INTERRUPT_STATUS2 << DP_INTERRUPT_STATUS_ACK_SHIFT)
#define DP_INTERRUPT_STATUS2_MASK \
	(DP_INTERRUPT_STATUS2 << DP_INTERRUPT_STATUS_MASK_SHIFT)

#define DP_INTERRUPT_STATUS4 \
	(PSR_UPDATE_INT | PSR_CAPTURE_INT | PSR_EXIT_INT | \
	PSR_UPDATE_ERROR_INT | PSR_WAKE_ERROR_INT)

#define DP_INTERRUPT_MASK4 \
	(PSR_UPDATE_MASK | PSR_CAPTURE_MASK | PSR_EXIT_MASK | \
	PSR_UPDATE_ERROR_MASK | PSR_WAKE_ERROR_MASK)

#define DP_DEFAULT_AHB_OFFSET	0x0000
#define DP_DEFAULT_AHB_SIZE	0x0200
#define DP_DEFAULT_AUX_OFFSET	0x0200
#define DP_DEFAULT_AUX_SIZE	0x0200
#define DP_DEFAULT_LINK_OFFSET	0x0400
#define DP_DEFAULT_LINK_SIZE	0x0C00
#define DP_DEFAULT_P0_OFFSET	0x1000
#define DP_DEFAULT_P0_SIZE	0x0400

struct msm_dp_catalog_private {
	struct device *dev;
	struct drm_device *drm_dev;
	u32 (*audio_map)[DP_AUDIO_SDP_HEADER_MAX];
	struct msm_dp_catalog msm_dp_catalog;
};

void msm_dp_catalog_snapshot(struct msm_dp_catalog *msm_dp_catalog, struct msm_disp_state *disp_state)
{
	msm_disp_snapshot_add_block(disp_state,
				    msm_dp_catalog->ahb_len, msm_dp_catalog->ahb_base, "dp_ahb");
	msm_disp_snapshot_add_block(disp_state,
				    msm_dp_catalog->aux_len, msm_dp_catalog->aux_base, "dp_aux");
	msm_disp_snapshot_add_block(disp_state,
				    msm_dp_catalog->link_len, msm_dp_catalog->link_base, "dp_link");
	msm_disp_snapshot_add_block(disp_state,
				    msm_dp_catalog->p0_len, msm_dp_catalog->p0_base, "dp_p0");
}

u32 msm_dp_catalog_aux_get_irq(struct msm_dp_catalog *msm_dp_catalog)
{
	u32 intr, intr_ack;

	intr = msm_dp_read_ahb(msm_dp_catalog, REG_DP_INTR_STATUS);
	intr &= ~DP_INTERRUPT_STATUS1_MASK;
	intr_ack = (intr & DP_INTERRUPT_STATUS1)
			<< DP_INTERRUPT_STATUS_ACK_SHIFT;
	msm_dp_write_ahb(msm_dp_catalog, REG_DP_INTR_STATUS, intr_ack |
			DP_INTERRUPT_STATUS1_MASK);

	return intr;

}

/**
 * msm_dp_catalog_hw_revision() - retrieve DP hw revision
 *
 * @msm_dp_catalog: DP catalog structure
 *
 * Return: DP controller hw revision
 *
 */
u32 msm_dp_catalog_hw_revision(const struct msm_dp_catalog *msm_dp_catalog)
{
	return msm_dp_read_ahb(msm_dp_catalog, REG_DP_HW_VERSION);
}

void msm_dp_catalog_ctrl_enable_irq(struct msm_dp_catalog *msm_dp_catalog,
						bool enable)
{
	if (enable) {
		msm_dp_write_ahb(msm_dp_catalog, REG_DP_INTR_STATUS,
				DP_INTERRUPT_STATUS1_MASK);
		msm_dp_write_ahb(msm_dp_catalog, REG_DP_INTR_STATUS2,
				DP_INTERRUPT_STATUS2_MASK);
	} else {
		msm_dp_write_ahb(msm_dp_catalog, REG_DP_INTR_STATUS, 0x00);
		msm_dp_write_ahb(msm_dp_catalog, REG_DP_INTR_STATUS2, 0x00);
	}
}

void msm_dp_catalog_hpd_config_intr(struct msm_dp_catalog *msm_dp_catalog,
			u32 intr_mask, bool en)
{
	struct msm_dp_catalog_private *catalog = container_of(msm_dp_catalog,
				struct msm_dp_catalog_private, msm_dp_catalog);

	u32 config = msm_dp_read_aux(msm_dp_catalog, REG_DP_DP_HPD_INT_MASK);

	config = (en ? config | intr_mask : config & ~intr_mask);

	drm_dbg_dp(catalog->drm_dev, "intr_mask=%#x config=%#x\n",
					intr_mask, config);
	msm_dp_write_aux(msm_dp_catalog, REG_DP_DP_HPD_INT_MASK,
				config & DP_DP_HPD_INT_MASK);
}

void msm_dp_catalog_ctrl_hpd_enable(struct msm_dp_catalog *msm_dp_catalog)
{
	u32 reftimer = msm_dp_read_aux(msm_dp_catalog, REG_DP_DP_HPD_REFTIMER);

	/* Configure REFTIMER and enable it */
	reftimer |= DP_DP_HPD_REFTIMER_ENABLE;
	msm_dp_write_aux(msm_dp_catalog, REG_DP_DP_HPD_REFTIMER, reftimer);

	/* Enable HPD */
	msm_dp_write_aux(msm_dp_catalog, REG_DP_DP_HPD_CTRL, DP_DP_HPD_CTRL_HPD_EN);
}

void msm_dp_catalog_ctrl_hpd_disable(struct msm_dp_catalog *msm_dp_catalog)
{
	u32 reftimer = msm_dp_read_aux(msm_dp_catalog, REG_DP_DP_HPD_REFTIMER);

	reftimer &= ~DP_DP_HPD_REFTIMER_ENABLE;
	msm_dp_write_aux(msm_dp_catalog, REG_DP_DP_HPD_REFTIMER, reftimer);

	msm_dp_write_aux(msm_dp_catalog, REG_DP_DP_HPD_CTRL, 0);
}

u32 msm_dp_catalog_link_is_connected(struct msm_dp_catalog *msm_dp_catalog)
{
	struct msm_dp_catalog_private *catalog = container_of(msm_dp_catalog,
				struct msm_dp_catalog_private, msm_dp_catalog);
	u32 status;

	status = msm_dp_read_aux(msm_dp_catalog, REG_DP_DP_HPD_INT_STATUS);
	drm_dbg_dp(catalog->drm_dev, "aux status: %#x\n", status);
	status >>= DP_DP_HPD_STATE_STATUS_BITS_SHIFT;
	status &= DP_DP_HPD_STATE_STATUS_BITS_MASK;

	return status;
}

u32 msm_dp_catalog_hpd_get_intr_status(struct msm_dp_catalog *msm_dp_catalog)
{
	int isr, mask;

	isr = msm_dp_read_aux(msm_dp_catalog, REG_DP_DP_HPD_INT_STATUS);
	msm_dp_write_aux(msm_dp_catalog, REG_DP_DP_HPD_INT_ACK,
				 (isr & DP_DP_HPD_INT_MASK));
	mask = msm_dp_read_aux(msm_dp_catalog, REG_DP_DP_HPD_INT_MASK);

	/*
	 * We only want to return interrupts that are unmasked to the caller.
	 * However, the interrupt status field also contains other
	 * informational bits about the HPD state status, so we only mask
	 * out the part of the register that tells us about which interrupts
	 * are pending.
	 */
	return isr & (mask | ~DP_DP_HPD_INT_MASK);
}

u32 msm_dp_catalog_ctrl_read_psr_interrupt_status(struct msm_dp_catalog *msm_dp_catalog)
{
	u32 intr, intr_ack;

	intr = msm_dp_read_ahb(msm_dp_catalog, REG_DP_INTR_STATUS4);
	intr_ack = (intr & DP_INTERRUPT_STATUS4)
			<< DP_INTERRUPT_STATUS_ACK_SHIFT;
	msm_dp_write_ahb(msm_dp_catalog, REG_DP_INTR_STATUS4, intr_ack);

	return intr;
}

void msm_dp_catalog_ctrl_config_psr_interrupt(struct msm_dp_catalog *msm_dp_catalog)
{
	msm_dp_write_ahb(msm_dp_catalog, REG_DP_INTR_MASK4, DP_INTERRUPT_MASK4);
}

int msm_dp_catalog_ctrl_get_interrupt(struct msm_dp_catalog *msm_dp_catalog)
{
	u32 intr, intr_ack;

	intr = msm_dp_read_ahb(msm_dp_catalog, REG_DP_INTR_STATUS2);
	intr &= ~DP_INTERRUPT_STATUS2_MASK;
	intr_ack = (intr & DP_INTERRUPT_STATUS2)
			<< DP_INTERRUPT_STATUS_ACK_SHIFT;
	msm_dp_write_ahb(msm_dp_catalog, REG_DP_INTR_STATUS2,
			intr_ack | DP_INTERRUPT_STATUS2_MASK);

	return intr;
}

/* panel related catalog functions */
int msm_dp_catalog_panel_timing_cfg(struct msm_dp_catalog *msm_dp_catalog, u32 total,
				u32 sync_start, u32 width_blanking, u32 msm_dp_active)
{
	u32 reg;

	msm_dp_write_link(msm_dp_catalog, REG_DP_TOTAL_HOR_VER, total);
	msm_dp_write_link(msm_dp_catalog, REG_DP_START_HOR_VER_FROM_SYNC, sync_start);
	msm_dp_write_link(msm_dp_catalog, REG_DP_HSYNC_VSYNC_WIDTH_POLARITY, width_blanking);
	msm_dp_write_link(msm_dp_catalog, REG_DP_ACTIVE_HOR_VER, msm_dp_active);

	reg = msm_dp_read_p0(msm_dp_catalog, MMSS_DP_INTF_CONFIG);

	if (msm_dp_catalog->wide_bus_en)
		reg |= DP_INTF_CONFIG_DATABUS_WIDEN;
	else
		reg &= ~DP_INTF_CONFIG_DATABUS_WIDEN;


	DRM_DEBUG_DP("wide_bus_en=%d reg=%#x\n", msm_dp_catalog->wide_bus_en, reg);

	msm_dp_write_p0(msm_dp_catalog, MMSS_DP_INTF_CONFIG, reg);
	return 0;
}

static void msm_dp_catalog_panel_send_vsc_sdp(struct msm_dp_catalog *msm_dp_catalog, struct dp_sdp *vsc_sdp)
{
	u32 header[2];
	u32 val;
	int i;

	msm_dp_utils_pack_sdp_header(&vsc_sdp->sdp_header, header);

	msm_dp_write_link(msm_dp_catalog, MMSS_DP_GENERIC0_0, header[0]);
	msm_dp_write_link(msm_dp_catalog, MMSS_DP_GENERIC0_1, header[1]);

	for (i = 0; i < sizeof(vsc_sdp->db); i += 4) {
		val = ((vsc_sdp->db[i]) | (vsc_sdp->db[i + 1] << 8) | (vsc_sdp->db[i + 2] << 16) |
		       (vsc_sdp->db[i + 3] << 24));
		msm_dp_write_link(msm_dp_catalog, MMSS_DP_GENERIC0_2 + i, val);
	}
}

static void msm_dp_catalog_panel_update_sdp(struct msm_dp_catalog *msm_dp_catalog)
{
	u32 hw_revision;

	hw_revision = msm_dp_catalog_hw_revision(msm_dp_catalog);
	if (hw_revision < DP_HW_VERSION_1_2 && hw_revision >= DP_HW_VERSION_1_0) {
		msm_dp_write_link(msm_dp_catalog, MMSS_DP_SDP_CFG3, 0x01);
		msm_dp_write_link(msm_dp_catalog, MMSS_DP_SDP_CFG3, 0x00);
	}
}

void msm_dp_catalog_panel_enable_vsc_sdp(struct msm_dp_catalog *msm_dp_catalog, struct dp_sdp *vsc_sdp)
{
	struct msm_dp_catalog_private *catalog;
	u32 cfg, cfg2, misc;

	catalog = container_of(msm_dp_catalog, struct msm_dp_catalog_private, msm_dp_catalog);

	cfg = msm_dp_read_link(msm_dp_catalog, MMSS_DP_SDP_CFG);
	cfg2 = msm_dp_read_link(msm_dp_catalog, MMSS_DP_SDP_CFG2);
	misc = msm_dp_read_link(msm_dp_catalog, REG_DP_MISC1_MISC0);

	cfg |= GEN0_SDP_EN;
	msm_dp_write_link(msm_dp_catalog, MMSS_DP_SDP_CFG, cfg);

	cfg2 |= GENERIC0_SDPSIZE_VALID;
	msm_dp_write_link(msm_dp_catalog, MMSS_DP_SDP_CFG2, cfg2);

	msm_dp_catalog_panel_send_vsc_sdp(msm_dp_catalog, vsc_sdp);

	/* indicates presence of VSC (BIT(6) of MISC1) */
	misc |= DP_MISC1_VSC_SDP;

	drm_dbg_dp(catalog->drm_dev, "vsc sdp enable=1\n");

	pr_debug("misc settings = 0x%x\n", misc);
	msm_dp_write_link(msm_dp_catalog, REG_DP_MISC1_MISC0, misc);

	msm_dp_catalog_panel_update_sdp(msm_dp_catalog);
}

void msm_dp_catalog_panel_disable_vsc_sdp(struct msm_dp_catalog *msm_dp_catalog)
{
	struct msm_dp_catalog_private *catalog;
	u32 cfg, cfg2, misc;

	catalog = container_of(msm_dp_catalog, struct msm_dp_catalog_private, msm_dp_catalog);

	cfg = msm_dp_read_link(msm_dp_catalog, MMSS_DP_SDP_CFG);
	cfg2 = msm_dp_read_link(msm_dp_catalog, MMSS_DP_SDP_CFG2);
	misc = msm_dp_read_link(msm_dp_catalog, REG_DP_MISC1_MISC0);

	cfg &= ~GEN0_SDP_EN;
	msm_dp_write_link(msm_dp_catalog, MMSS_DP_SDP_CFG, cfg);

	cfg2 &= ~GENERIC0_SDPSIZE_VALID;
	msm_dp_write_link(msm_dp_catalog, MMSS_DP_SDP_CFG2, cfg2);

	/* switch back to MSA */
	misc &= ~DP_MISC1_VSC_SDP;

	drm_dbg_dp(catalog->drm_dev, "vsc sdp enable=0\n");

	pr_debug("misc settings = 0x%x\n", misc);
	msm_dp_write_link(msm_dp_catalog, REG_DP_MISC1_MISC0, misc);

	msm_dp_catalog_panel_update_sdp(msm_dp_catalog);
}

void msm_dp_catalog_panel_tpg_enable(struct msm_dp_catalog *msm_dp_catalog,
				struct drm_display_mode *drm_mode)
{
	struct msm_dp_catalog_private *catalog = container_of(msm_dp_catalog,
				struct msm_dp_catalog_private, msm_dp_catalog);
	u32 hsync_period, vsync_period;
	u32 display_v_start, display_v_end;
	u32 hsync_start_x, hsync_end_x;
	u32 v_sync_width;
	u32 hsync_ctl;
	u32 display_hctl;

	/* TPG config parameters*/
	hsync_period = drm_mode->htotal;
	vsync_period = drm_mode->vtotal;

	display_v_start = ((drm_mode->vtotal - drm_mode->vsync_start) *
					hsync_period);
	display_v_end = ((vsync_period - (drm_mode->vsync_start -
					drm_mode->vdisplay))
					* hsync_period) - 1;

	display_v_start += drm_mode->htotal - drm_mode->hsync_start;
	display_v_end -= (drm_mode->hsync_start - drm_mode->hdisplay);

	hsync_start_x = drm_mode->htotal - drm_mode->hsync_start;
	hsync_end_x = hsync_period - (drm_mode->hsync_start -
					drm_mode->hdisplay) - 1;

	v_sync_width = drm_mode->vsync_end - drm_mode->vsync_start;

	hsync_ctl = (hsync_period << 16) |
			(drm_mode->hsync_end - drm_mode->hsync_start);
	display_hctl = (hsync_end_x << 16) | hsync_start_x;


	msm_dp_write_p0(catalog, MMSS_DP_INTF_CONFIG, 0x0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_HSYNC_CTL, hsync_ctl);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_VSYNC_PERIOD_F0, vsync_period *
			hsync_period);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_VSYNC_PULSE_WIDTH_F0, v_sync_width *
			hsync_period);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_VSYNC_PERIOD_F1, 0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_VSYNC_PULSE_WIDTH_F1, 0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_DISPLAY_HCTL, display_hctl);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_ACTIVE_HCTL, 0);
	msm_dp_write_p0(catalog, MMSS_INTF_DISPLAY_V_START_F0, display_v_start);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_DISPLAY_V_END_F0, display_v_end);
	msm_dp_write_p0(catalog, MMSS_INTF_DISPLAY_V_START_F1, 0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_DISPLAY_V_END_F1, 0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_ACTIVE_V_START_F0, 0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_ACTIVE_V_END_F0, 0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_ACTIVE_V_START_F1, 0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_ACTIVE_V_END_F1, 0);
	msm_dp_write_p0(catalog, MMSS_DP_INTF_POLARITY_CTL, 0);

	msm_dp_write_p0(catalog, MMSS_DP_TPG_MAIN_CONTROL,
				DP_TPG_CHECKERED_RECT_PATTERN);
	msm_dp_write_p0(catalog, MMSS_DP_TPG_VIDEO_CONFIG,
				DP_TPG_VIDEO_CONFIG_BPP_8BIT |
				DP_TPG_VIDEO_CONFIG_RGB);
	msm_dp_write_p0(catalog, MMSS_DP_BIST_ENABLE,
				DP_BIST_ENABLE_DPBIST_EN);
	msm_dp_write_p0(catalog, MMSS_DP_TIMING_ENGINE_EN,
				DP_TIMING_ENGINE_EN_EN);
	drm_dbg_dp(catalog->drm_dev, "%s: enabled tpg\n", __func__);
}

void msm_dp_catalog_panel_tpg_disable(struct msm_dp_catalog *msm_dp_catalog)
{
	struct msm_dp_catalog_private *catalog = container_of(msm_dp_catalog,
				struct msm_dp_catalog_private, msm_dp_catalog);

	msm_dp_write_p0(catalog, MMSS_DP_TPG_MAIN_CONTROL, 0x0);
	msm_dp_write_p0(catalog, MMSS_DP_BIST_ENABLE, 0x0);
	msm_dp_write_p0(catalog, MMSS_DP_TIMING_ENGINE_EN, 0x0);
}

static void __iomem *msm_dp_ioremap(struct platform_device *pdev, int idx, size_t *len)
{
	struct resource *res;
	void __iomem *base;

	base = devm_platform_get_and_ioremap_resource(pdev, idx, &res);
	if (!IS_ERR(base))
		*len = resource_size(res);

	return base;
}

static int msm_dp_catalog_get_io(struct msm_dp_catalog_private *catalog)
{
	struct msm_dp_catalog *msm_dp_catalog = &catalog->msm_dp_catalog;
	struct platform_device *pdev = to_platform_device(catalog->dev);

	msm_dp_catalog->ahb_base = msm_dp_ioremap(pdev, 0, &msm_dp_catalog->ahb_len);
	if (IS_ERR(msm_dp_catalog->ahb_base))
		return PTR_ERR(msm_dp_catalog->ahb_base);

	msm_dp_catalog->aux_base = msm_dp_ioremap(pdev, 1, &msm_dp_catalog->aux_len);
	if (IS_ERR(msm_dp_catalog->aux_base)) {
		/*
		 * The initial binding had a single reg, but in order to
		 * support variation in the sub-region sizes this was split.
		 * msm_dp_ioremap() will fail with -EINVAL here if only a single
		 * reg is specified, so fill in the sub-region offsets and
		 * lengths based on this single region.
		 */
		if (PTR_ERR(msm_dp_catalog->aux_base) == -EINVAL) {
			if (msm_dp_catalog->ahb_len < DP_DEFAULT_P0_OFFSET + DP_DEFAULT_P0_SIZE) {
				DRM_ERROR("legacy memory region not large enough\n");
				return -EINVAL;
			}

			msm_dp_catalog->ahb_len = DP_DEFAULT_AHB_SIZE;
			msm_dp_catalog->aux_base = msm_dp_catalog->ahb_base + DP_DEFAULT_AUX_OFFSET;
			msm_dp_catalog->aux_len = DP_DEFAULT_AUX_SIZE;
			msm_dp_catalog->link_base = msm_dp_catalog->ahb_base + DP_DEFAULT_LINK_OFFSET;
			msm_dp_catalog->link_len = DP_DEFAULT_LINK_SIZE;
			msm_dp_catalog->p0_base = msm_dp_catalog->ahb_base + DP_DEFAULT_P0_OFFSET;
			msm_dp_catalog->p0_len = DP_DEFAULT_P0_SIZE;
		} else {
			DRM_ERROR("unable to remap aux region: %pe\n", msm_dp_catalog->aux_base);
			return PTR_ERR(msm_dp_catalog->aux_base);
		}
	} else {
		msm_dp_catalog->link_base = msm_dp_ioremap(pdev, 2, &msm_dp_catalog->link_len);
		if (IS_ERR(msm_dp_catalog->link_base)) {
			DRM_ERROR("unable to remap link region: %pe\n", msm_dp_catalog->link_base);
			return PTR_ERR(msm_dp_catalog->link_base);
		}

		msm_dp_catalog->p0_base = msm_dp_ioremap(pdev, 3, &msm_dp_catalog->p0_len);
		if (IS_ERR(msm_dp_catalog->p0_base)) {
			DRM_ERROR("unable to remap p0 region: %pe\n", msm_dp_catalog->p0_base);
			return PTR_ERR(msm_dp_catalog->p0_base);
		}
	}

	return 0;
}

struct msm_dp_catalog *msm_dp_catalog_get(struct device *dev)
{
	struct msm_dp_catalog_private *catalog;
	int ret;

	catalog  = devm_kzalloc(dev, sizeof(*catalog), GFP_KERNEL);
	if (!catalog)
		return ERR_PTR(-ENOMEM);

	catalog->dev = dev;

	ret = msm_dp_catalog_get_io(catalog);
	if (ret)
		return ERR_PTR(ret);

	return &catalog->msm_dp_catalog;
}

u32 msm_dp_catalog_audio_get_header(struct msm_dp_catalog *msm_dp_catalog,
				enum msm_dp_catalog_audio_sdp_type sdp,
				enum msm_dp_catalog_audio_header_type header)
{
	struct msm_dp_catalog_private *catalog;
	u32 (*sdp_map)[DP_AUDIO_SDP_HEADER_MAX];

	catalog = container_of(msm_dp_catalog,
		struct msm_dp_catalog_private, msm_dp_catalog);

	sdp_map = catalog->audio_map;

	return msm_dp_read_link(msm_dp_catalog, sdp_map[sdp][header]);
}

void msm_dp_catalog_audio_set_header(struct msm_dp_catalog *msm_dp_catalog,
				 enum msm_dp_catalog_audio_sdp_type sdp,
				 enum msm_dp_catalog_audio_header_type header,
				 u32 data)
{
	struct msm_dp_catalog_private *catalog;
	u32 (*sdp_map)[DP_AUDIO_SDP_HEADER_MAX];

	if (!msm_dp_catalog)
		return;

	catalog = container_of(msm_dp_catalog,
		struct msm_dp_catalog_private, msm_dp_catalog);

	sdp_map = catalog->audio_map;

	msm_dp_write_link(msm_dp_catalog, sdp_map[sdp][header], data);
}

void msm_dp_catalog_audio_config_acr(struct msm_dp_catalog *msm_dp_catalog, u32 select)
{
	struct msm_dp_catalog_private *catalog;
	u32 acr_ctrl;

	if (!msm_dp_catalog)
		return;

	catalog = container_of(msm_dp_catalog,
		struct msm_dp_catalog_private, msm_dp_catalog);

	acr_ctrl = select << 4 | BIT(31) | BIT(8) | BIT(14);

	drm_dbg_dp(catalog->drm_dev, "select: %#x, acr_ctrl: %#x\n",
					select, acr_ctrl);

	msm_dp_write_link(msm_dp_catalog, MMSS_DP_AUDIO_ACR_CTRL, acr_ctrl);
}

void msm_dp_catalog_audio_enable(struct msm_dp_catalog *msm_dp_catalog, bool enable)
{
	struct msm_dp_catalog_private *catalog;
	u32 audio_ctrl;

	if (!msm_dp_catalog)
		return;

	catalog = container_of(msm_dp_catalog,
		struct msm_dp_catalog_private, msm_dp_catalog);

	audio_ctrl = msm_dp_read_link(msm_dp_catalog, MMSS_DP_AUDIO_CFG);

	if (enable)
		audio_ctrl |= BIT(0);
	else
		audio_ctrl &= ~BIT(0);

	drm_dbg_dp(catalog->drm_dev, "dp_audio_cfg = 0x%x\n", audio_ctrl);

	msm_dp_write_link(msm_dp_catalog, MMSS_DP_AUDIO_CFG, audio_ctrl);
	/* make sure audio engine is disabled */
	wmb();
}

void msm_dp_catalog_audio_config_sdp(struct msm_dp_catalog *msm_dp_catalog)
{
	struct msm_dp_catalog_private *catalog;
	u32 sdp_cfg = 0;
	u32 sdp_cfg2 = 0;

	if (!msm_dp_catalog)
		return;

	catalog = container_of(msm_dp_catalog,
		struct msm_dp_catalog_private, msm_dp_catalog);

	sdp_cfg = msm_dp_read_link(msm_dp_catalog, MMSS_DP_SDP_CFG);
	/* AUDIO_TIMESTAMP_SDP_EN */
	sdp_cfg |= BIT(1);
	/* AUDIO_STREAM_SDP_EN */
	sdp_cfg |= BIT(2);
	/* AUDIO_COPY_MANAGEMENT_SDP_EN */
	sdp_cfg |= BIT(5);
	/* AUDIO_ISRC_SDP_EN  */
	sdp_cfg |= BIT(6);
	/* AUDIO_INFOFRAME_SDP_EN  */
	sdp_cfg |= BIT(20);

	drm_dbg_dp(catalog->drm_dev, "sdp_cfg = 0x%x\n", sdp_cfg);

	msm_dp_write_link(msm_dp_catalog, MMSS_DP_SDP_CFG, sdp_cfg);

	sdp_cfg2 = msm_dp_read_link(msm_dp_catalog, MMSS_DP_SDP_CFG2);
	/* IFRM_REGSRC -> Do not use reg values */
	sdp_cfg2 &= ~BIT(0);
	/* AUDIO_STREAM_HB3_REGSRC-> Do not use reg values */
	sdp_cfg2 &= ~BIT(1);

	drm_dbg_dp(catalog->drm_dev, "sdp_cfg2 = 0x%x\n", sdp_cfg2);

	msm_dp_write_link(msm_dp_catalog, MMSS_DP_SDP_CFG2, sdp_cfg2);
}

void msm_dp_catalog_audio_init(struct msm_dp_catalog *msm_dp_catalog)
{
	struct msm_dp_catalog_private *catalog;

	static u32 sdp_map[][DP_AUDIO_SDP_HEADER_MAX] = {
		{
			MMSS_DP_AUDIO_STREAM_0,
			MMSS_DP_AUDIO_STREAM_1,
			MMSS_DP_AUDIO_STREAM_1,
		},
		{
			MMSS_DP_AUDIO_TIMESTAMP_0,
			MMSS_DP_AUDIO_TIMESTAMP_1,
			MMSS_DP_AUDIO_TIMESTAMP_1,
		},
		{
			MMSS_DP_AUDIO_INFOFRAME_0,
			MMSS_DP_AUDIO_INFOFRAME_1,
			MMSS_DP_AUDIO_INFOFRAME_1,
		},
		{
			MMSS_DP_AUDIO_COPYMANAGEMENT_0,
			MMSS_DP_AUDIO_COPYMANAGEMENT_1,
			MMSS_DP_AUDIO_COPYMANAGEMENT_1,
		},
		{
			MMSS_DP_AUDIO_ISRC_0,
			MMSS_DP_AUDIO_ISRC_1,
			MMSS_DP_AUDIO_ISRC_1,
		},
	};

	if (!msm_dp_catalog)
		return;

	catalog = container_of(msm_dp_catalog,
		struct msm_dp_catalog_private, msm_dp_catalog);

	catalog->audio_map = sdp_map;
}

void msm_dp_catalog_audio_sfe_level(struct msm_dp_catalog *msm_dp_catalog, u32 safe_to_exit_level)
{
	struct msm_dp_catalog_private *catalog;
	u32 mainlink_levels;

	if (!msm_dp_catalog)
		return;

	catalog = container_of(msm_dp_catalog,
		struct msm_dp_catalog_private, msm_dp_catalog);

	mainlink_levels = msm_dp_read_link(msm_dp_catalog, REG_DP_MAINLINK_LEVELS);
	mainlink_levels &= 0xFE0;
	mainlink_levels |= safe_to_exit_level;

	drm_dbg_dp(catalog->drm_dev,
			"mainlink_level = 0x%x, safe_to_exit_level = 0x%x\n",
			 mainlink_levels, safe_to_exit_level);

	msm_dp_write_link(msm_dp_catalog, REG_DP_MAINLINK_LEVELS, mainlink_levels);
}
