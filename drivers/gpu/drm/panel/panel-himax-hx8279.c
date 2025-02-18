// SPDX-License-Identifier: GPL-2.0-only
/*
 * Himax HX8279 DriverIC panels driver
 *
 * Copyright (c) 2025 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

/* Page selection */
#define HX8279_REG_PAGE			0xb0
 #define HX8279_PAGE_SEL		GENMASK(3, 0)

/* Page 0 - Driver/Module Configuration */
#define HX8279_P0_VGHS			0xbf
#define HX8279_P0_VGLS			0xc0
#define HX8279_P0_VGPHS			0xc2
#define HX8279_P0_VGNHS			0xc4
 #define HX8279_P0_VG_SEL		GENMASK(4, 0)
 #define HX8279_VGH_MIN_MV		8700
 #define HX8279_VGH_STEP_MV		300
 #define HX8279_VGL_MIN_MV		6700
 #define HX8279_VGL_STEP_MV		300
 #define HX8279_VGPNH_MIN_MV		4000
 #define HX8279_VGPNX_STEP_MV		50
 #define HX8279_VGH_VOLT_SEL(x)		((x - HX8279_VGH_MIN_MV) / HX8279_VGH_STEP_MV)
 #define HX8279_VGL_VOLT_SEL(x)		((x - HX8279_VGL_MIN_MV) / HX8279_VGL_STEP_MV)
 #define HX8279_VGPN_VOLT_SEL(x)	((x - HX8279_VGPNH_MIN_MV) / HX8279_VGPNX_STEP_MV)

/* Page 1 - Gate driver On Array (GOA) Timing */
#define HX8279_P1_REG_GOA_L		0xc0
#define HX8279_P1_REG_GOUTL(x)		(HX8279_P1_REG_GOA_L + (x))
#define HX8279_P1_REG_GOA_R		0xd4
#define HX8279_P1_REG_GOUTR(x)		(HX8279_P1_REG_GOA_R + (x))
 #define HX8279_GOUT_STB		GENMASK(7, 6)
 #define HX8279_GOUT_SEL		GENMASK(5, 0)

/* Page 3 - Gate driver On Array (GOA) Configuration */
#define HX8279_P3_REG_UNKNOWN_BA	0xba
#define HX8279_P3_REG_GOA_STVL		0xc8
 #define HX8279_P3_GOA_STV_LEAD		GENMASK(4, 0)
#define HX8279_P3_REG_GOA_CKVL		0xc9
 #define HX8279_P3_GOA_CKV_LEAD		GENMASK(4, 0)
#define HX8279_P3_REG_GOA_CKVD		0xca
 #define HX8279_P3_GOA_CKV_NONOVERLAP	BIT(7)
 #define HX8279_P3_GOA_CKV_RESERVED	BIT(6)
 #define HX8279_P3_GOA_CKV_DUMMY	GENMASK(5, 0)
#define HX8279_P3_REG_GOA_CLR1_W_ADJ	0xd2
#define HX8279_P3_REG_GOA_CLR234_W_ADJ	0xd3
#define HX8279_P3_REG_GOA_CLR1_CFG	0xd4
#define HX8279_P3_REG_GOA_CLR_CFG(x)	(HX8279_P3_REG_GOA_CLR1_CFG + x) /* CLR1..4 */
 #define HX8279_P3_GOA_CLR_CFG_POLARITY	BIT(7)
 #define HX8279_P3_GOA_CLR_CFG_STARTPOS	GENMASK(6, 0)
#define HX8279_P3_REG_UNKNOWN_E4	0xe4
#define HX8279_P3_REG_UNKNOWN_E5	0xe5

/* Page 5 - MIPI */
#define HX8279_P5_REG_TIMING		0xb3
 #define HX8279_P5_TIMING_THS_SETTLE	GENMASK(7, 5)
 #define HX8279_P5_TIMING_LHS_SETTLE	BIT(4)
 #define HX8279_P5_TIMING_TLPX		GENMASK(3, 0)
#define HX8279_P5_REG_UNKNOWN_B8	0xb8
#define HX8279_P5_REG_UNKNOWN_BC	0xbc
#define HX8279_P5_REG_UNKNOWN_D6	0xd6

/* Page 6 - Engineer */
#define HX8279_P6_REG_ENGINEER_PWD	0xb8
#define HX8279_P6_REG_INHOUSE_FUNC	0xc0
 #define HX8279_P6_ENG_UNLOCK_WORD	0xa5
#define HX8279_P6_REG_GAMMA_CHOPPER	0xbc
 #define HX8279_P6_GAMMA_POCGM_CTL	GENMASK(6, 4)
 #define HX8279_P6_GAMMA_POGCMD_CTL	GENMASK(2, 0)
#define HX8279_P6_REG_DLY_TIME_ADJ	0xd5

struct hx8279 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	struct regulator_bulk_data vregs[2];
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	const struct hx8279_panel_desc *desc;
	u8 last_page;
	bool skip_voltage_config;
	bool skip_goa_timing;
	bool skip_goa_config;
	bool skip_mipi_timing;
	bool skip_gamma_config;
};

struct hx8279_panel_mode {
	const struct drm_display_mode mode;
	int bpc;
	bool is_video_mode;
};

struct hx8279_panel_desc {
	const char *model_name;
	const struct mipi_dsi_device_info dsi_info;
	const struct hx8279_panel_mode *mode_data;
	bool is_dual_dsi;
	u8 num_lanes;
	u8 num_modes;

	/* Page 0 */
	unsigned int vgh_mv;
	unsigned int vgl_mv;
	unsigned int vgph_mv;
	unsigned int vgnh_mv;

	/* Page 1 */
	u8 gout_l[20];
	u8 gout_r[20];

	/* Page 3 */
	u8 goa_unk_ba;
	u8 goa_stv_lead_time_ck;
	u8 goa_ckv_lead_time_ck;
	u8 goa_ckv_dummy_vblank_num;
	bool goa_ckv_non_overlap_ctl;
	u8 goa_clr1_width_adj;
	u8 goa_clr234_width_adj;
	s8 goa_clr_polarity[4];
	int goa_clr_start_pos[4];
	u8 goa_unk_e4;
	u8 goa_unk_e5;

	/* Page 5 */
	u8 bta_tlpx;
	bool lhs_settle_time_by_osc25;
	u8 ths_settle_time;
	u8 timing_unk_b8;
	u8 timing_unk_bc;
	u8 timing_unk_d6;

	/* Page 6 */
	u8 gamma_ctl;
	u8 src_delay_time_adj_ck;
};

static inline struct hx8279 *to_hx8279(struct drm_panel *panel)
{
	return container_of(panel, struct hx8279, panel);
}

static int hx8279_set_page(struct hx8279 *hx,
			   struct mipi_dsi_multi_context *dsi_ctx, u8 page)
{
	const u8 cmd_set_page[] = { HX8279_REG_PAGE, page };

	if (hx->last_page == page)
		return 0;

	mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_page, ARRAY_SIZE(cmd_set_page));
	if (!dsi_ctx->accum_err)
		hx->last_page = page;
	else
		return -EINVAL;

	return 0;
}

static int hx8279_set_module_config(struct hx8279 *hx,
				    struct mipi_dsi_multi_context *dsi_ctx)
{
	const struct hx8279_panel_desc *desc = hx->desc;
	u8 cmd_set_voltage[2];
	int ret;

	if (hx->skip_voltage_config)
		return 0;

	/* Page 0 - Driver/Module Configuration */
	ret = hx8279_set_page(hx, dsi_ctx, 0);
	if (ret)
		return ret;

	if (desc->vgh_mv) {
		cmd_set_voltage[0] = HX8279_P0_VGHS;
		cmd_set_voltage[1] = HX8279_VGH_VOLT_SEL(desc->vgh_mv);
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_voltage,
					     ARRAY_SIZE(cmd_set_voltage));
	}

	if (desc->vgl_mv) {
		cmd_set_voltage[0] = HX8279_P0_VGLS;
		cmd_set_voltage[1] = HX8279_VGL_VOLT_SEL(desc->vgl_mv);
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_voltage,
					     ARRAY_SIZE(cmd_set_voltage));
	}

	if (desc->vgph_mv) {
		cmd_set_voltage[0] = HX8279_P0_VGPHS;
		cmd_set_voltage[1] = HX8279_VGPN_VOLT_SEL(desc->vgph_mv);
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_voltage,
					     ARRAY_SIZE(cmd_set_voltage));
	}

	if (desc->vgnh_mv) {
		cmd_set_voltage[0] = HX8279_P0_VGNHS;
		cmd_set_voltage[1] = HX8279_VGPN_VOLT_SEL(desc->vgnh_mv);
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_voltage,
					     ARRAY_SIZE(cmd_set_voltage));
	}

	return 0;
}

static int hx8279_set_gout(struct hx8279 *hx,
			   struct mipi_dsi_multi_context *dsi_ctx)
{
	const struct hx8279_panel_desc *desc = hx->desc;
	u8 cmd_set_gout[2];
	int i, ret;

	if (hx->skip_goa_timing)
		return 0;

	ret = hx8279_set_page(hx, dsi_ctx, 1);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(desc->gout_l); i++) {
		cmd_set_gout[0] = HX8279_P1_REG_GOUTL(i);
		cmd_set_gout[1] = desc->gout_l[i];
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_gout,
					     ARRAY_SIZE(cmd_set_gout));
	}

	for (i = 0; i < ARRAY_SIZE(desc->gout_r); i++) {
		cmd_set_gout[0] = HX8279_P1_REG_GOUTR(i);
		cmd_set_gout[1] = desc->gout_r[i];
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_gout,
					     ARRAY_SIZE(cmd_set_gout));
	}

	return 0;
}

static int hx8279_set_goa_cfg(struct hx8279 *hx,
			      struct mipi_dsi_multi_context *dsi_ctx)
{
	const struct hx8279_panel_desc *desc = hx->desc;
	u8 cmd_set_goa[2];
	int i, ret;

	if (hx->skip_goa_config)
		return 0;

	ret = hx8279_set_page(hx, dsi_ctx, 3);
	if (ret)
		return ret;

	if (desc->goa_unk_ba)  {
		cmd_set_goa[0] = HX8279_P3_REG_UNKNOWN_BA;
		cmd_set_goa[1] = desc->goa_unk_ba;
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
					     ARRAY_SIZE(cmd_set_goa));
	}

	if (desc->goa_stv_lead_time_ck) {
		cmd_set_goa[0] = HX8279_P3_REG_GOA_STVL;
		cmd_set_goa[1] = FIELD_PREP(HX8279_P3_GOA_STV_LEAD,
					    desc->goa_stv_lead_time_ck);
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
					     ARRAY_SIZE(cmd_set_goa));
	}

	if (desc->goa_ckv_lead_time_ck) {
		cmd_set_goa[0] = HX8279_P3_REG_GOA_CKVL;
		cmd_set_goa[1] = FIELD_PREP(HX8279_P3_GOA_CKV_DUMMY,
					    desc->goa_ckv_lead_time_ck);
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
					     ARRAY_SIZE(cmd_set_goa));
	}

	if (desc->goa_ckv_dummy_vblank_num) {
		cmd_set_goa[0] = HX8279_P3_REG_GOA_CKVD;
		cmd_set_goa[1] = FIELD_PREP(HX8279_P3_GOA_CKV_LEAD,
					    desc->goa_ckv_dummy_vblank_num);
		cmd_set_goa[1] |= FIELD_PREP(HX8279_P3_GOA_CKV_NONOVERLAP,
					     desc->goa_ckv_non_overlap_ctl);
		/* RESERVED must be always set */
		cmd_set_goa[1] |= HX8279_P3_GOA_CKV_RESERVED;
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
					     ARRAY_SIZE(cmd_set_goa));
	}

	if (desc->goa_clr1_width_adj) {
		cmd_set_goa[0] = HX8279_P3_REG_GOA_CLR1_W_ADJ;
		cmd_set_goa[1] = desc->goa_clr1_width_adj;
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
					     ARRAY_SIZE(cmd_set_goa));
	}

	if (desc->goa_clr234_width_adj) {
		cmd_set_goa[0] = HX8279_P3_REG_GOA_CLR234_W_ADJ;
		cmd_set_goa[1] = desc->goa_clr234_width_adj;
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
					     ARRAY_SIZE(cmd_set_goa));
	}

	/* Polarity and Start Position arrays are of the same size */
	for (i = 0; i < ARRAY_SIZE(desc->goa_clr_polarity); i++) {
		if (desc->goa_clr_polarity[i] < 0 || desc->goa_clr_start_pos[i] < 0)
			continue;

		cmd_set_goa[0] = HX8279_P3_REG_GOA_CLR_CFG(i);
		cmd_set_goa[1] = FIELD_PREP(HX8279_P3_GOA_CLR_CFG_STARTPOS,
					    desc->goa_clr_start_pos[i]);
		cmd_set_goa[1] |= FIELD_PREP(HX8279_P3_GOA_CLR_CFG_POLARITY,
					     desc->goa_clr_polarity[i]);
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
					     ARRAY_SIZE(cmd_set_goa));
	}

	if (desc->goa_unk_e4) {
		cmd_set_goa[0] = HX8279_P3_REG_UNKNOWN_E4;
		cmd_set_goa[1] = desc->goa_unk_e4;
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
					     ARRAY_SIZE(cmd_set_goa));
	}

	cmd_set_goa[0] = HX8279_P3_REG_UNKNOWN_E5;
	cmd_set_goa[1] = desc->goa_unk_e5;
	mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_goa,
				     ARRAY_SIZE(cmd_set_goa));

	return 0;
}

static int hx8279_set_mipi_cfg(struct hx8279 *hx,
			       struct mipi_dsi_multi_context *dsi_ctx)
{
	const struct hx8279_panel_desc *desc = hx->desc;
	u8 cmd_set_mipi[2];
	int ret;

	if (hx->skip_mipi_timing)
		return 0;

	ret = hx8279_set_page(hx, dsi_ctx, 5);
	if (ret)
		return ret;

	if (desc->bta_tlpx || desc->ths_settle_time || desc->lhs_settle_time_by_osc25) {
		cmd_set_mipi[0] = HX8279_P5_REG_TIMING;
		cmd_set_mipi[1] = FIELD_PREP(HX8279_P5_TIMING_TLPX, desc->bta_tlpx);
		cmd_set_mipi[1] |= FIELD_PREP(HX8279_P5_TIMING_THS_SETTLE,
					      desc->ths_settle_time);
		cmd_set_mipi[1] |= FIELD_PREP(HX8279_P5_TIMING_LHS_SETTLE,
					      desc->lhs_settle_time_by_osc25);
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_mipi,
					     ARRAY_SIZE(cmd_set_mipi));
	}

	if (desc->timing_unk_b8) {
		cmd_set_mipi[0] = HX8279_P5_REG_UNKNOWN_B8;
		cmd_set_mipi[1] = desc->timing_unk_b8;
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_mipi,
					     ARRAY_SIZE(cmd_set_mipi));
	}

	if (desc->timing_unk_bc) {
		cmd_set_mipi[0] = HX8279_P5_REG_UNKNOWN_BC;
		cmd_set_mipi[1] = desc->timing_unk_bc;
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_mipi,
					     ARRAY_SIZE(cmd_set_mipi));
	}

	if (desc->timing_unk_d6) {
		cmd_set_mipi[0] = HX8279_P5_REG_UNKNOWN_D6;
		cmd_set_mipi[1] = desc->timing_unk_d6;
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_mipi,
					     ARRAY_SIZE(cmd_set_mipi));
	}

	return 0;
}

static int hx8279_set_adv_cfg(struct hx8279 *hx,
			      struct mipi_dsi_multi_context *dsi_ctx)
{
	const struct hx8279_panel_desc *desc = hx->desc;
	const u8 cmd_set_gamma[] = { HX8279_P6_REG_GAMMA_CHOPPER, desc->gamma_ctl };
	const u8 cmd_set_dly[] = { HX8279_P6_REG_DLY_TIME_ADJ, desc->src_delay_time_adj_ck };
	u8 cmd_set_eng[] = { HX8279_P6_REG_ENGINEER_PWD, HX8279_P6_ENG_UNLOCK_WORD };
	int ret;

	if (!desc->gamma_ctl && !desc->src_delay_time_adj_ck)
		return 0;

	ret = hx8279_set_page(hx, dsi_ctx, 6);
	if (ret)
		return ret;

	/* Unlock ENG settings: write same word to both ENGINEER_PWD and INHOUSE_FUNC */
	mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_eng, ARRAY_SIZE(cmd_set_eng));

	cmd_set_eng[0] = HX8279_P6_REG_INHOUSE_FUNC;
	mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_eng, ARRAY_SIZE(cmd_set_eng));

	/* Set Gamma Chopper and Gamma buffer Chopper control */
	if (desc->gamma_ctl)
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_gamma,
					     ARRAY_SIZE(cmd_set_gamma));

	/* Set Source delay time adjustment (CKV falling to Source off) */
	if (desc->src_delay_time_adj_ck)
		mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_dly,
					     ARRAY_SIZE(cmd_set_dly));

	/* Lock ENG settings again */
	cmd_set_eng[0] = HX8279_P6_REG_ENGINEER_PWD;
	cmd_set_eng[1] = 0;
	mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_eng, ARRAY_SIZE(cmd_set_eng));

	cmd_set_eng[0] = HX8279_P6_REG_INHOUSE_FUNC;
	mipi_dsi_generic_write_multi(dsi_ctx, cmd_set_eng, ARRAY_SIZE(cmd_set_eng));

	return 0;
}

static int hx8279_on(struct hx8279 *hx)
{
	struct mipi_dsi_device *dsi = hx->dsi[0];
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	int ret;

	/* Page 5 */
	ret = hx8279_set_mipi_cfg(hx, &dsi_ctx);
	if (ret)
		return ret;

	/* Page 1 */
	ret = hx8279_set_gout(hx, &dsi_ctx);
	if (ret)
		return ret;

	/* Page 3 */
	ret = hx8279_set_goa_cfg(hx, &dsi_ctx);
	if (ret)
		return ret;

	/* Page 0 - Driver/Module Configuration */
	ret = hx8279_set_module_config(hx, &dsi_ctx);
	if (ret)
		return ret;

	/* Page 6 */
	ret = hx8279_set_adv_cfg(hx, &dsi_ctx);
	if (ret)
		return ret;

	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	return 0;
}

static void hx8279_power_off(struct hx8279 *hx)
{
	gpiod_set_value_cansleep(hx->reset_gpio, 0);
	usleep_range(100, 500);
	gpiod_set_value_cansleep(hx->enable_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(hx->vregs), hx->vregs);
}

static int hx8279_disable(struct drm_panel *panel)
{
	struct hx8279 *hx = to_hx8279(panel);
	struct mipi_dsi_device *dsi = hx->dsi[0];
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);

	return 0;
}

static int hx8279_enable(struct drm_panel *panel)
{
	struct hx8279 *hx = to_hx8279(panel);
	struct mipi_dsi_device *dsi = hx->dsi[0];
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return 0;
}

static int hx8279_prepare(struct drm_panel *panel)
{
	struct hx8279 *hx = to_hx8279(panel);
	struct mipi_dsi_device *dsi = hx->dsi[0];
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(hx->vregs), hx->vregs);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(hx->enable_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(hx->reset_gpio, 1);
	usleep_range(6000, 7000);

	ret = hx8279_on(hx);
	if (ret) {
		hx8279_power_off(hx);
		return ret;
	}

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 130);

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	if (hx->dsi[1])
		hx->dsi[1]->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int hx8279_unprepare(struct drm_panel *panel)
{
	struct hx8279 *hx = to_hx8279(panel);
	struct mipi_dsi_device *dsi = hx->dsi[0];
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	if (hx->dsi[1])
		hx->dsi[1]->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 130);

	hx8279_power_off(hx);
	return 0;
}

static int hx8279_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct hx8279 *hx = to_hx8279(panel);
	int i;

	for (i = 0; i < hx->desc->num_modes; i++) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, &hx->desc->mode_data[i].mode);
		if (!mode)
			return -ENOMEM;

		drm_mode_set_name(mode);

		mode->type |= DRM_MODE_TYPE_DRIVER;
		if (hx->desc->num_modes == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.bpc = hx->desc->mode_data[0].bpc;
	connector->display_info.height_mm = hx->desc->mode_data[0].mode.height_mm;
	connector->display_info.width_mm = hx->desc->mode_data[0].mode.width_mm;

	return hx->desc->num_modes;
}

static const struct drm_panel_funcs hx8279_panel_funcs = {
	.disable = hx8279_disable,
	.unprepare = hx8279_unprepare,
	.prepare = hx8279_prepare,
	.enable = hx8279_enable,
	.get_modes = hx8279_get_modes,
};

static int hx8279_init_vregs(struct hx8279 *hx, struct device *dev)
{
	int ret;

	hx->vregs[0].supply = "vdd";
	hx->vregs[1].supply = "iovcc";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(hx->vregs),
				      hx->vregs);
	if (ret < 0)
		return ret;

	ret = regulator_is_supported_voltage(hx->vregs[0].consumer,
					     3000000, 5000000);
	if (!ret)
		return -EINVAL;

	ret = regulator_is_supported_voltage(hx->vregs[1].consumer,
					     1700000, 1900000);
	if (!ret)
		return -EINVAL;

	return 0;
}

static int hx8279_check_config(struct hx8279 *hx, struct device *dev)
{
	const struct hx8279_panel_desc *desc = hx->desc;
	int i, num_clr, num_zero;

	/* Voltages config validation */
	if (!desc->vgh_mv && !desc->vgl_mv && !desc->vgph_mv && !desc->vgnh_mv)
		hx->skip_voltage_config = true;
	else if ((desc->vgh_mv && desc->vgh_mv < HX8279_VGH_MIN_MV) ||
		 (desc->vgl_mv && desc->vgl_mv < HX8279_VGL_MIN_MV) ||
		 (desc->vgph_mv && desc->vgph_mv < HX8279_VGPNH_MIN_MV) ||
		 (desc->vgnh_mv && desc->vgnh_mv < HX8279_VGPNH_MIN_MV))
			return -EINVAL;

	/* GOA Timing validation */
	num_zero = 1;
	for (i = 0; i < ARRAY_SIZE(desc->gout_l); i++) {
		if (!desc->gout_l[i])
			num_zero++;
		else if (desc->gout_l[i] > (HX8279_GOUT_STB | HX8279_GOUT_SEL))
			return dev_err_probe(dev, -EINVAL,
					     "Invalid value found in gout_l[%d]\n", i);
	}
	if (num_zero >= ARRAY_SIZE(desc->gout_l))
		hx->skip_goa_timing = true;

	if (!hx->skip_goa_timing) {
		num_zero = 1;
		for (i = 0; i < ARRAY_SIZE(desc->gout_r); i++) {
			if (!desc->gout_r[i])
				num_zero++;
			else if (desc->gout_r[i] > (HX8279_GOUT_STB | HX8279_GOUT_SEL))
				return dev_err_probe(dev, -EINVAL,
						     "Invalid value found in gout_r[%d]\n", i);
		}
		if (num_zero >= ARRAY_SIZE(desc->gout_r))
			hx->skip_goa_timing = true;
	}

	/* GOA Configuration validation */
	if (!desc->goa_unk_ba && !desc->goa_stv_lead_time_ck &&
	    !desc->goa_ckv_lead_time_ck && !desc->goa_ckv_dummy_vblank_num &&
	    !desc->goa_clr1_width_adj && !desc->goa_clr234_width_adj &&
	    !desc->goa_unk_e4 && !desc->goa_unk_e5)
		hx->skip_goa_config = true;

	if (!hx->skip_goa_config) {
		if ((desc->goa_stv_lead_time_ck > HX8279_P3_GOA_STV_LEAD) ||
		    (desc->goa_ckv_lead_time_ck > HX8279_P3_GOA_CKV_LEAD) ||
		    (desc->goa_ckv_dummy_vblank_num > HX8279_P3_GOA_CKV_DUMMY))
			return dev_err_probe(dev, -EINVAL,
					     "Invalid lead timings in GOA config\n");
		/*
		 * Don't perform zero check for polarity and start position, as
		 * both pol=0 and start=0 are valid configuration values.
		 */
		num_clr = 0;
		for (i = 0; i < ARRAY_SIZE(desc->goa_clr_start_pos); i++) {
			if (desc->goa_clr_start_pos[i] < 0)
				continue;
			else if (desc->goa_clr_start_pos[i] > HX8279_P3_GOA_CLR_CFG_STARTPOS)
				return dev_err_probe(dev, -EINVAL,
						     "Invalid start position for CLR%d\n", i + 1);
			else
				num_clr++;
		}
		if (!num_clr)
			return -EINVAL;

		for (i = 0; i < ARRAY_SIZE(desc->goa_clr_polarity); i++) {
			if (num_clr < 0)
				return -EINVAL;

			if (desc->goa_clr_polarity[i] < 0)
				continue;
			else if (desc->goa_clr_polarity[i] > 1)
				return dev_err_probe(dev, -EINVAL,
						     "Invalid polarity for CLR%d\n", i + 1);
			else
				num_clr--;
		}
	}

	/* MIPI Configuration validation */
	if (!desc->bta_tlpx && !desc->lhs_settle_time_by_osc25 &&
	    !desc->ths_settle_time && !desc->timing_unk_b8 &&
	    !desc->timing_unk_bc && !desc->timing_unk_d6)
		hx->skip_mipi_timing = true;

	/* Gamma Configuration validation */
	if (desc->gamma_ctl > (HX8279_P6_GAMMA_POCGM_CTL | HX8279_P6_GAMMA_POGCMD_CTL))
		return -EINVAL;

	return 0;
}

static int hx8279_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct hx8279 *hx;
	int i, num_dsis = 1, ret;

	hx = devm_kzalloc(dev, sizeof(*hx), GFP_KERNEL);
	if (!hx)
		return -ENOMEM;

	ret = hx8279_init_vregs(hx, dev);
	if (ret)
		return ret;

	hx->desc = device_get_match_data(dev);
	if (!hx->desc)
		return -ENODEV;

	/*
	 * In some DriverICs some or all fields may be OTP: perform a
	 * basic configuration check before writing to help avoiding
	 * irreparable mistakes.
	 *
	 * Please note that this is not perfect and will only check if
	 * the values may be plausible; values that are wrong for a
	 * specific display, but still plausible for DrIC config will
	 * be accepted.
	 */
	ret = hx8279_check_config(hx, dev);
	if (ret)
		return dev_err_probe(dev, ret, "Invalid DriverIC configuration\n");

	/* The enable line may be always tied to VCCIO, so this is optional */
	hx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(hx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(hx->enable_gpio),
				     "Failed to get enable GPIO\n");

	hx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(hx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(hx->reset_gpio),
				     "Failed to get reset GPIO\n");

	/* If the panel is connected on two DSIs then DSI0 left, DSI1 right */
	if (hx->desc->is_dual_dsi) {
		const struct mipi_dsi_device_info *info = &hx->desc->dsi_info;
		struct mipi_dsi_host *dsi_r_host;
		struct device_node *dsi_r;

		dsi_r = of_graph_get_remote_node(dsi->dev.of_node, 1, -1);
		if (!dsi_r)
			return dev_err_probe(dev, -ENODEV, "Cannot get secondary DSI node.\n");

		dsi_r_host = of_find_mipi_dsi_host_by_node(dsi_r);
		of_node_put(dsi_r);
		if (!dsi_r_host)
			return dev_err_probe(dev, -EPROBE_DEFER,
					     "Cannot get secondary DSI host\n");

		hx->dsi[1] = devm_mipi_dsi_device_register_full(dev, dsi_r_host, info);
		if (IS_ERR(hx->dsi[1]))
			return dev_err_probe(dev, PTR_ERR(hx->dsi[1]),
					     "Cannot get secondary DSI node\n");
		num_dsis++;
		mipi_dsi_set_drvdata(hx->dsi[1], hx);
	}

	hx->dsi[0] = dsi;
	mipi_dsi_set_drvdata(dsi, hx);

	drm_panel_init(&hx->panel, dev, &hx8279_panel_funcs, DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&hx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&hx->panel);

	for (i = 0; i < num_dsis; i++) {
		hx->dsi[i]->lanes = hx->desc->num_lanes;
		hx->dsi[i]->format = MIPI_DSI_FMT_RGB888;

		hx->dsi[i]->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS |
					 MIPI_DSI_MODE_LPM;

		if (hx->desc->mode_data[0].is_video_mode)
			hx->dsi[i]->mode_flags |= MIPI_DSI_MODE_VIDEO |
						  MIPI_DSI_MODE_VIDEO_SYNC_PULSE;

		ret = devm_mipi_dsi_attach(dev, hx->dsi[i]);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "Cannot attach to DSI%d host.\n", i);
	}

	/* Make sure we start in a known state: panel off */
	gpiod_set_value_cansleep(hx->reset_gpio, 0);
	gpiod_set_value_cansleep(hx->enable_gpio, 0);

	return 0;
}

static void hx8279_remove(struct mipi_dsi_device *dsi)
{
	struct hx8279 *hx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&hx->panel);
}

static const struct hx8279_panel_mode startek_kd070fhfid078_modes[] = {
	{
		.mode = {
			.clock = 156458,
			.hdisplay = 1200,
			.hsync_start = 1200 + 50,
			.hsync_end = 1200 + 50 + 24,
			.htotal = 1200 + 50 + 24 + 66,
			.vdisplay = 1920,
			.vsync_start = 1920 + 14,
			.vsync_end = 1920 + 14 + 2,
			.vtotal = 1920 + 14 + 2 + 10,
			.width_mm = 95,
			.height_mm = 151,
			.type = DRM_MODE_TYPE_DRIVER
		},
		.bpc = 8,
		.is_video_mode = true,
	},
};

static const struct hx8279_panel_desc startek_kd070fhfid078 = {
	.model_name = "Startek KD070FHFID078",
	.dsi_info = {
		.type = "KD070FHFID078",
		.channel = 0,
		.node = NULL,
	},
	.mode_data = startek_kd070fhfid078_modes,
	.num_modes = ARRAY_SIZE(startek_kd070fhfid078_modes),
	.num_lanes = 4,

	/* Driver/Module Configuration: LC Matrix voltages */
	.vgh_mv = 18000,
	.vgl_mv = 12100,
	.vgph_mv = 5500,
	.vgnh_mv = 5500,

	/* Gate driver On Array (GOA) Timing */
	.gout_l = { 0xd, 0xd, 0x6, 0x6, 0x8, 0x8, 0xa, 0xa, 0xc, 0xc,
		    0x0, 0x0, 0xe, 0xe, 0x1, 0x1, 0x4, 0x4, 0x0, 0x0 },
	.gout_r = { 0xd, 0xd, 0x5, 0x5, 0x7, 0x7, 0x9, 0x9, 0xb, 0xb,
		    0x0, 0x0, 0xe, 0xe, 0x1, 0x1, 0x3, 0x3, 0x0, 0x0 },

	/* Gate driver On Array (GOA) Configuration */
	.goa_unk_ba = 0xf0,
	.goa_stv_lead_time_ck = 7,
	.goa_ckv_lead_time_ck = 3,
	.goa_ckv_dummy_vblank_num = 1,
	.goa_clr1_width_adj = 1,
	.goa_clr234_width_adj = 5,
	.goa_clr_polarity = { 0, 1, -1, -1 },
	.goa_clr_start_pos = { 5, 10, -1, -1 },
	.goa_unk_e4 = 0xc0,
	.goa_unk_e5 = 0x00,

	/* MIPI Configuration */
	.bta_tlpx = 2,
	.lhs_settle_time_by_osc25 = true,
	.ths_settle_time = 2,
	.timing_unk_b8 = 0x7f,
	.timing_unk_bc = 0x20,
	.timing_unk_d6 = 0x7f,

	/* ENG/Gamma Configuration */
	.gamma_ctl = FIELD_PREP_CONST(HX8279_P6_GAMMA_POCGM_CTL, 1) |
		     FIELD_PREP_CONST(HX8279_P6_GAMMA_POGCMD_CTL, 1),
	.src_delay_time_adj_ck = 72,
};

static const struct of_device_id hx8279_of_match[] = {
	{ .compatible = "startek,kd070fhfid078", .data = &startek_kd070fhfid078 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hx8279_of_match);

static struct mipi_dsi_driver hx8279_driver = {
	.probe = hx8279_probe,
	.remove = hx8279_remove,
	.driver = {
		.name = "panel-himax-hx8279",
		.of_match_table = hx8279_of_match,
	},
};
module_mipi_dsi_driver(hx8279_driver);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("Himax HX8279 DriverIC panels driver");
MODULE_LICENSE("GPL");
