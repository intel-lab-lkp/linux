// SPDX-License-Identifier: GPL-2.0
/*
 * TC358775 DSI to LVDS bridge driver
 *
 * Copyright (C) 2020 SMART Wireless Computing
 * Author: Vinay Simha BN <simhavcs@gmail.com>
 *
 */
/* #define DEBUG */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/phy/phy-mipi-dphy.h>

#include <asm/unaligned.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

/* Registers */

/* DSI D-PHY Layer Registers */
#define D0W_DPHYCONTTX  0x0004  /* Data Lane 0 DPHY Tx Control */
#define CLW_DPHYCONTRX  0x0020  /* Clock Lane DPHY Rx Control */
#define D0W_DPHYCONTRX  0x0024  /* Data Lane 0 DPHY Rx Control */
#define D1W_DPHYCONTRX  0x0028  /* Data Lane 1 DPHY Rx Control */
#define D2W_DPHYCONTRX  0x002C  /* Data Lane 2 DPHY Rx Control */
#define D3W_DPHYCONTRX  0x0030  /* Data Lane 3 DPHY Rx Control */
#define COM_DPHYCONTRX  0x0038  /* DPHY Rx Common Control */
#define CLW_CNTRL       0x0040  /* Clock Lane Control */
#define D0W_CNTRL       0x0044  /* Data Lane 0 Control */
#define D1W_CNTRL       0x0048  /* Data Lane 1 Control */
#define D2W_CNTRL       0x004C  /* Data Lane 2 Control */
#define D3W_CNTRL       0x0050  /* Data Lane 3 Control */
#define DFTMODE_CNTRL   0x0054  /* DFT Mode Control */

/* DSI PPI Layer Registers */
#define PPI_STARTPPI    0x0104  /* START control bit of PPI-TX function. */
#define PPI_STARTPPI_STARTPPI	BIT(0)

#define PPI_BUSYPPI     0x0108
#define PPI_LINEINITCNT 0x0110  /* Line Initialization Wait Counter  */
#define PPI_LPTXTIMECNT 0x0114
#define PPI_LANEENABLE  0x0134  /* Enables each lane at the PPI layer. */
#define LANEENABLE_CLEN	BIT(0)
#define LANEENABLE_L0EN	BIT(1)
#define PPI_TX_RX_TA    0x013C  /* DSI Bus Turn Around timing parameters */

/* Analog timer function enable */
#define PPI_CLS_ATMR    0x0140  /* Delay for Clock Lane in LPRX  */
#define PPI_D0S_ATMR    0x0144  /* Delay for Data Lane 0 in LPRX */
#define PPI_D1S_ATMR    0x0148  /* Delay for Data Lane 1 in LPRX */
#define PPI_D2S_ATMR    0x014C  /* Delay for Data Lane 2 in LPRX */
#define PPI_D3S_ATMR    0x0150  /* Delay for Data Lane 3 in LPRX */

#define PPI_D0S_CLRSIPOCOUNT    0x0164  /* For lane 0 */
#define PPI_D1S_CLRSIPOCOUNT    0x0168  /* For lane 1 */
#define PPI_D2S_CLRSIPOCOUNT    0x016C  /* For lane 2 */
#define PPI_D3S_CLRSIPOCOUNT    0x0170  /* For lane 3 */

#define CLS_PRE         0x0180  /* Digital Counter inside of PHY IO */
#define D0S_PRE         0x0184  /* Digital Counter inside of PHY IO */
#define D1S_PRE         0x0188  /* Digital Counter inside of PHY IO */
#define D2S_PRE         0x018C  /* Digital Counter inside of PHY IO */
#define D3S_PRE         0x0190  /* Digital Counter inside of PHY IO */
#define CLS_PREP        0x01A0  /* Digital Counter inside of PHY IO */
#define D0S_PREP        0x01A4  /* Digital Counter inside of PHY IO */
#define D1S_PREP        0x01A8  /* Digital Counter inside of PHY IO */
#define D2S_PREP        0x01AC  /* Digital Counter inside of PHY IO */
#define D3S_PREP        0x01B0  /* Digital Counter inside of PHY IO */
#define CLS_ZERO        0x01C0  /* Digital Counter inside of PHY IO */
#define D0S_ZERO        0x01C4  /* Digital Counter inside of PHY IO */
#define D1S_ZERO        0x01C8  /* Digital Counter inside of PHY IO */
#define D2S_ZERO        0x01CC  /* Digital Counter inside of PHY IO */
#define D3S_ZERO        0x01D0  /* Digital Counter inside of PHY IO */

#define PPI_CLRFLG      0x01E0  /* PRE Counters has reached set values */
#define PPI_CLRSIPO     0x01E4  /* Clear SIPO values, Slave mode use only. */
#define HSTIMEOUT       0x01F0  /* HS Rx Time Out Counter */
#define HSTIMEOUTENABLE 0x01F4  /* Enable HS Rx Time Out Counter */

#define DSI_LANEENABLE  0x0210  /* Enables each lane at the Protocol layer. */
#define DSI_LANESTATUS0 0x0214  /* Displays lane is in HS RX mode. */
#define DSI_LANESTATUS1 0x0218  /* Displays lane is in ULPS or STOP state */

#define DSI_INTSTATUS   0x0220  /* Interrupt Status */
#define DSI_INTMASK     0x0224  /* Interrupt Mask */
#define DSI_INTCLR      0x0228  /* Interrupt Clear */
#define DSI_LPTXTO      0x0230  /* Low Power Tx Time Out Counter */

#define DSIERRCNT       0x0300  /* DSI Error Count */
#define APLCTRL         0x0400  /* Application Layer Control */
#define RDPKTLN         0x0404  /* Command Read Packet Length */

#define VPCTRL          0x0450  /* Video Path Control */
#define VPCTRL_MSF	BIT(0)
#define VPCTRL_OPXLFMT	BIT(8)
#define VPCTRL_EVTMODE	BIT(5)  /* Video event mode enable, tc35876x only */
#define HTIM1           0x0454  /* Horizontal Timing Control 1 */
#define HTIM1_HPW	GENMASK(8, 0)
#define HTIM1_HBPR	GENMASK(24, 16)
#define HTIM2           0x0458  /* Horizontal Timing Control 2 */
#define HTIM2_HACT	GENMASK(10, 0)
#define HTIM2_HFPR	GENMASK(24, 16)
#define VTIM1           0x045C  /* Vertical Timing Control 1 */
#define VTIM1_VPW	GENMASK(7, 0)
#define VTIM1_VBPR	GENMASK(23, 16)
#define VTIM2           0x0460  /* Vertical Timing Control 2 */
#define VTIM2_VACT	GENMASK(10, 0)
#define VTIM2_VFPR	GENMASK(23, 16)
#define VFUEN           0x0464  /* Video Frame Timing Update Enable */
#define VFUEN_VFUEN	BIT(0)  /* Upload Enable */

/* Mux Input Select for LVDS LINK Input */
#define LV_MX0003        0x0480  /* Bit 0 to 3 */
#define LV_MX0407        0x0484  /* Bit 4 to 7 */
#define LV_MX0811        0x0488  /* Bit 8 to 11 */
#define LV_MX1215        0x048C  /* Bit 12 to 15 */
#define LV_MX1619        0x0490  /* Bit 16 to 19 */
#define LV_MX2023        0x0494  /* Bit 20 to 23 */
#define LV_MX2427        0x0498  /* Bit 24 to 27 */
#define LV_MX(b0, b1, b2, b3) \
	(((b3) << 24) | ((b2) << 16) | ((b1) << 8) | (b0))

/* Input bit numbers used in mux registers */
enum {
	LVI_R0, LVI_R1, LVI_R2, LVI_R3, LVI_R4, LVI_R5, LVI_R6, LVI_R7,
	LVI_G0, LVI_G1, LVI_G2, LVI_G3, LVI_G4, LVI_G5, LVI_G6, LVI_G7,
	LVI_B0, LVI_B1, LVI_B2, LVI_B3, LVI_B4, LVI_B5, LVI_B6, LVI_B7,
	LVI_HS, LVI_VS, LVI_DE, LVI_L0
};

#define LVCFG           0x049C  /* LVDS Configuration  */
#define LVCFG_LVEN	BIT(0)
#define LVCFG_LVDLINK	BIT(1)
#define LVCFG_PCLKDIV	GENMASK(7, 4)
#define LVCFG_PCLKSEL	GENMASK(11, 10)
#define PCLKSEL_HSRCK	0	/* DSI clock */

#define LVPHY0          0x04A0  /* LVDS PHY 0 */
#define LVPHY0_LV_ND	GENMASK(4, 0)
#define LVPHY0_LV_FS	GENMASK(6, 5)
#define LVPHY0_LV_IS	GENMASK(15, 14) /* charge pump current */
#define LVPHY0_LV_RST	BIT(22)

#define LVPHY1          0x04A4  /* LVDS PHY 1 */
#define SYSSTAT         0x0500  /* System Status  */
#define SYSRST          0x0504  /* System Reset  */

#define SYS_RST_I2CS	BIT(0) /* Reset I2C-Slave controller */
#define SYS_RST_I2CM	BIT(1) /* Reset I2C-Master controller */
#define SYS_RST_LCD	BIT(2) /* Reset LCD controller */
#define SYS_RST_BM	BIT(3) /* Reset Bus Management controller */
#define SYS_RST_DSIRX	BIT(4) /* Reset DSI-RX and App controller */
#define SYS_RST_REG	BIT(5) /* Reset Register module */

/* GPIO Registers */
#define GPIOC           0x0520  /* GPIO Control  */
#define GPIOO           0x0524  /* GPIO Output  */
#define GPIOI           0x0528  /* GPIO Input  */

/* I2C Registers */
#define I2CTIMCTRL      0x0540  /* I2C IF Timing and Enable Control */
#define I2CMADDR        0x0544  /* I2C Master Addressing */
#define WDATAQ          0x0548  /* Write Data Queue */
#define RDATAQ          0x054C  /* Read Data Queue */

/* Chip ID and Revision ID Register */
#define IDREG           0x0580

#define TC358775XBG_ID  0x00007500

/* Debug Registers */
#define DEBUG00         0x05A0  /* Debug */
#define DEBUG01         0x05A4  /* LVDS Data */

enum tc358775_ports {
	TC358775_DSI_IN,
	TC358775_LVDS_OUT0,
	TC358775_LVDS_OUT1,
};

enum tc3587x5_type {
	TC358765 = 0x65,
	TC358775 = 0x75,
};

struct tc_data {
	struct regmap		*regmap;
	struct device		*dev;

	struct drm_bridge	bridge;
	struct drm_bridge	*panel_bridge;

	struct device_node *host_node;
	struct mipi_dsi_device *dsi;
	u8 num_dsi_lanes;

	struct regulator	*vdd;
	struct regulator	*vddio;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*stby_gpio;
	bool			lvds_dual_link;
	bool			powered;
	u8			bpc;

	enum tc3587x5_type	type;
};

struct tc358775_pll_settings {
	unsigned int min_khz;
	unsigned int max_khz;
	u8 fs;
	u8 nd;
	u8 is;
};

static inline struct tc_data *bridge_to_tc(struct drm_bridge *b)
{
	return container_of(b, struct tc_data, bridge);
}

static void tc358775_power_up(struct tc_data *tc)
{
	struct device *dev = &tc->dsi->dev;
	int ret;

	if (tc->powered)
		return;

	ret = regulator_enable(tc->vddio);
	if (ret < 0)
		dev_err(dev, "regulator vddio enable failed, %d\n", ret);

	ret = regulator_enable(tc->vdd);
	if (ret < 0)
		dev_err(dev, "regulator vdd enable failed, %d\n", ret);

	gpiod_set_value(tc->stby_gpio, 0);
	usleep_range(10, 20);

	gpiod_set_value(tc->reset_gpio, 0);
	usleep_range(200, 250);

	tc->powered = true;
}

static void tc358775_power_down(struct tc_data *tc)
{
	struct device *dev = &tc->dsi->dev;
	int ret;

	gpiod_set_value(tc->reset_gpio, 1);
	usleep_range(10, 20);

	gpiod_set_value(tc->stby_gpio, 1);

	ret = regulator_disable(tc->vdd);
	if (ret < 0)
		dev_err(dev, "regulator vdd disable failed, %d\n", ret);

	ret = regulator_disable(tc->vddio);
	if (ret < 0)
		dev_err(dev, "regulator vddio disable failed, %d\n", ret);

	tc->powered = false;
}

/* helper function to access bus_formats */
static struct drm_connector *get_connector(struct drm_encoder *encoder)
{
	struct drm_device *dev = encoder->dev;
	struct drm_connector *connector;

	list_for_each_entry(connector, &dev->mode_config.connector_list, head)
		if (connector->encoder == encoder)
			return connector;

	return NULL;
}

static const struct reg_sequence tc_lvmux_vesa24[] = {
	{ LV_MX0003, LV_MX(LVI_R0, LVI_R1, LVI_R2, LVI_R3) },
	{ LV_MX0407, LV_MX(LVI_R4, LVI_R7, LVI_R5, LVI_G0) },
	{ LV_MX0811, LV_MX(LVI_G1, LVI_G2, LVI_G6, LVI_G7) },
	{ LV_MX1215, LV_MX(LVI_G3, LVI_G4, LVI_G5, LVI_B0) },
	{ LV_MX1619, LV_MX(LVI_B6, LVI_B7, LVI_B1, LVI_B2) },
	{ LV_MX2023, LV_MX(LVI_B3, LVI_B4, LVI_B5, LVI_L0) },
	{ LV_MX2427, LV_MX(LVI_HS, LVI_VS, LVI_DE, LVI_R6) },
};

/* JEIDA-24/JEIDA-18 have the same mapping */
static const struct reg_sequence tc_lvmux_jeida18_24[] = {
	{ LV_MX0003, LV_MX(LVI_R2, LVI_R3, LVI_R4, LVI_R5) },
	{ LV_MX0407, LV_MX(LVI_R6, LVI_R1, LVI_R7, LVI_G2) },
	{ LV_MX0811, LV_MX(LVI_G3, LVI_G4, LVI_G0, LVI_G1) },
	{ LV_MX1215, LV_MX(LVI_G5, LVI_G6, LVI_G7, LVI_B2) },
	{ LV_MX1619, LV_MX(LVI_B0, LVI_B1, LVI_B3, LVI_B4) },
	{ LV_MX2023, LV_MX(LVI_B5, LVI_B6, LVI_B7, LVI_L0) },
	{ LV_MX2427, LV_MX(LVI_HS, LVI_VS, LVI_DE, LVI_R0) },
};

/* All the DSI timing is counted by the HS byte clock internally */
static uint32_t tc358775_ps_to_cnt(unsigned long long ps,
				   struct phy_configure_opts_mipi_dphy *cfg)
{
	unsigned long hs_byte_clk = cfg->hs_clk_rate / 8;

	return DIV_ROUND_UP(ps * hs_byte_clk, PSEC_PER_SEC);
}

static void tc358775_configure_dsi(struct tc_data *tc, unsigned int pixelclk)
{
	int bpp = mipi_dsi_pixel_format_to_bpp(tc->dsi->format);
	struct phy_configure_opts_mipi_dphy cfg;
	unsigned int val;

	phy_mipi_dphy_get_default_config(pixelclk * 1000, bpp,
					 tc->num_dsi_lanes, &cfg);

	regmap_write(tc->regmap, PPI_TX_RX_TA,
		     (tc358775_ps_to_cnt(cfg.ta_get, &cfg) << 16) |
		     tc358775_ps_to_cnt(cfg.ta_sure, &cfg));
	regmap_write(tc->regmap, PPI_LPTXTIMECNT,
		     tc358775_ps_to_cnt(cfg.lpx, &cfg));

	val = tc358775_ps_to_cnt(cfg.hs_settle, &cfg);
	regmap_write(tc->regmap, PPI_D0S_CLRSIPOCOUNT, val);
	regmap_write(tc->regmap, PPI_D1S_CLRSIPOCOUNT, val);
	regmap_write(tc->regmap, PPI_D2S_CLRSIPOCOUNT, val);
	regmap_write(tc->regmap, PPI_D3S_CLRSIPOCOUNT, val);

	val = LANEENABLE_CLEN;
	val |= (LANEENABLE_L0EN << tc->num_dsi_lanes) - LANEENABLE_L0EN;
	regmap_write(tc->regmap, PPI_LANEENABLE, val);
	regmap_write(tc->regmap, DSI_LANEENABLE, val);

	regmap_write(tc->regmap, PPI_STARTPPI, PPI_STARTPPI_STARTPPI);
}

static void tc358775_configure_lvds_timings(struct tc_data *tc,
					    struct drm_display_mode *mode)
{
	u32 hback_porch, hsync_len, hfront_porch, hactive;
	u32 vback_porch, vsync_len, vfront_porch, vactive;
	unsigned int val;

	hback_porch = mode->htotal - mode->hsync_end;
	hsync_len  = mode->hsync_end - mode->hsync_start;
	hactive = mode->hdisplay;
	hfront_porch = mode->hsync_start - mode->hdisplay;

	vback_porch = mode->vtotal - mode->vsync_end;
	vsync_len  = mode->vsync_end - mode->vsync_start;
	vactive = mode->vdisplay;
	vfront_porch = mode->vsync_start - mode->vdisplay;

	/* Video event mode vs pulse mode bit, does not exist for tc358775 */
	if (tc->type == TC358765)
		val = VPCTRL_EVTMODE;
	else
		val = 0;

	if (tc->bpc == 8)
		val |= VPCTRL_OPXLFMT;
	else /* bpc = 6; */
		val |= VPCTRL_MSF;

	regmap_update_bits(tc->regmap, VPCTRL, val,
			   VPCTRL_OPXLFMT | VPCTRL_MSF | VPCTRL_EVTMODE);

	val = u32_encode_bits(hsync_len, HTIM1_HPW);
	val |= u32_encode_bits(hback_porch, HTIM1_HBPR);
	regmap_write(tc->regmap, HTIM1, val);

	val = u32_encode_bits(hactive, HTIM2_HACT);
	val |= u32_encode_bits(hfront_porch, HTIM2_HFPR);
	regmap_write(tc->regmap, HTIM2, val);

	val = u32_encode_bits(vsync_len, VTIM1_VPW);
	val |= u32_encode_bits(vback_porch, VTIM1_VBPR);
	regmap_write(tc->regmap, VTIM1, val);

	val = u32_encode_bits(vactive, VTIM2_VACT);
	val |= u32_encode_bits(vfront_porch, VTIM2_VFPR);
	regmap_write(tc->regmap, VTIM2, val);

	regmap_write(tc->regmap, VFUEN, VFUEN_VFUEN);
}

static const struct tc358775_pll_settings tc358775_pll_settings[] = {
	{ 25000, 30000, 2, 27, 1 },
	{ 30000, 60000, 1, 13, 1 },
	{ 60000, 135000, 0, 6, 1 },
	{}
};

static void tc358775_configure_pll(struct tc_data *tc, unsigned int pixelclk)
{
	const struct tc358775_pll_settings *settings;
	unsigned int val;

	if (tc->lvds_dual_link)
		pixelclk /= 2;

	for (settings = tc358775_pll_settings; settings->min_khz; settings++)
		if (pixelclk > settings->min_khz &&
		    pixelclk < settings->max_khz)
			break;

	if (!settings->min_khz)
		return;

	val = u32_encode_bits(settings->fs, LVPHY0_LV_FS);
	val |= u32_encode_bits(settings->nd, LVPHY0_LV_ND);
	val |= u32_encode_bits(settings->is, LVPHY0_LV_IS);

	regmap_write(tc->regmap, LVPHY0, val | LVPHY0_LV_RST);
	usleep_range(100, 150);
	regmap_write(tc->regmap, LVPHY0, val);

	regmap_write(tc->regmap, SYSRST, SYS_RST_LCD);
}

static void tc358775_configure_color_mapping(struct tc_data *tc, u32 fmt)
{
	dev_dbg(tc->dev, "bus_formats %04x bpc %d\n", fmt, tc->bpc);

	if (fmt == MEDIA_BUS_FMT_RGB888_1X7X4_SPWG)
		regmap_multi_reg_write(tc->regmap, tc_lvmux_vesa24,
				       ARRAY_SIZE(tc_lvmux_vesa24));
	else
		regmap_multi_reg_write(tc->regmap, tc_lvmux_jeida18_24,
				       ARRAY_SIZE(tc_lvmux_jeida18_24));
}

static void tc358775_configure_lvds_clock(struct tc_data *tc)
{
	int bpp = mipi_dsi_pixel_format_to_bpp(tc->dsi->format);
	unsigned int val;
	int clkdiv;

	/* Configure LVDS clock */
	clkdiv = bpp / tc->num_dsi_lanes;
	if (!tc->lvds_dual_link)
		clkdiv /= 2;

	val = u32_encode_bits(clkdiv, LVCFG_PCLKDIV);
	val |= u32_encode_bits(PCLKSEL_HSRCK, LVCFG_PCLKSEL);
	if (tc->lvds_dual_link)
		val |= LVCFG_LVDLINK;

	regmap_write(tc->regmap, LVCFG, val);
}

static void tc358775_dsi_lp11_notify(struct drm_bridge *bridge)
{
	struct tc_data *tc = bridge_to_tc(bridge);

	tc358775_power_up(tc);
}

static void tc358775_bridge_pre_enable(struct drm_bridge *bridge)
{
	struct drm_connector *connector = get_connector(bridge->encoder);
	struct tc_data *tc = bridge_to_tc(bridge);
	struct drm_display_mode *mode;
	unsigned int val = 0;

	/*
	 * Legacy behavior, make sure this bridge is powered even if
	 * drm_bridge_dsi_lp11_notify() isn't called by the DSI host
	 */
	tc358775_power_up(tc);

	mode = &bridge->encoder->crtc->state->adjusted_mode;

	regmap_read(tc->regmap, IDREG, &val);

	dev_info(tc->dev, "DSI2LVDS Chip ID.%02x Revision ID. %02x **\n",
		 (val >> 8) & 0xFF, val & 0xFF);

	tc358775_configure_dsi(tc, mode->crtc_clock);
	tc358775_configure_lvds_timings(tc, mode);
	tc358775_configure_pll(tc, mode->crtc_clock);
	tc358775_configure_color_mapping(tc, connector->display_info.bus_formats[0]);
	tc358775_configure_lvds_clock(tc);
}

static void tc358775_bridge_enable(struct drm_bridge *bridge)
{
	struct tc_data *tc = bridge_to_tc(bridge);

	regmap_update_bits(tc->regmap, LVCFG, LVCFG_LVEN, LVCFG_LVEN);
}

static void tc358775_bridge_post_disable(struct drm_bridge *bridge)
{
	struct tc_data *tc = bridge_to_tc(bridge);

	tc358775_power_down(tc);
}

/*
 * According to the datasheet, the horizontal back porch, front porch and sync
 * length must be a multiple of 2 and the minimal horizontal pulse width is 8.
 * To workaround this, we modify the back porch and the sync pulse width by
 * adding enough pixels. These pixels will then be substracted from the front
 * porch which is ignored by the bridge.  Hopefully, this marginal modified
 * timing is tolerated by the panel. The alternative is either a black screen
 * (if the sync pulse width is too short or a shifted picture if the lengths
 * are not even).
 */
static bool tc_mode_fixup(struct drm_bridge *bridge,
			  const struct drm_display_mode *mode,
			  struct drm_display_mode *adj)
{
	u16 hsync_len, hback_porch;

	hback_porch = adj->htotal - adj->hsync_end;
	if (hback_porch & 1) {
		adj->hsync_end -= 1;
		adj->hsync_start -= 1;
	}

	hsync_len = adj->hsync_end - adj->hsync_start;
	if (hsync_len < 8)
		adj->hsync_start -= 8 - hsync_len;
	else if (hsync_len & 1)
		adj->hsync_start -= 1;

	return adj->hsync_start >= adj->hdisplay;
}

static enum drm_mode_status
tc_mode_valid(struct drm_bridge *bridge,
	      const struct drm_display_info *info,
	      const struct drm_display_mode *mode)
{
	struct tc_data *tc = bridge_to_tc(bridge);

	/*
	 * Maximum pixel clock speed 135MHz for single-link
	 * 270MHz for dual-link
	 */
	if ((mode->clock > 135000 && !tc->lvds_dual_link) ||
	    (mode->clock > 270000 && tc->lvds_dual_link))
		return MODE_CLOCK_HIGH;

	switch (info->bus_formats[0]) {
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:
		/* RGB888 */
		tc->bpc = 8;
		break;
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:
		/* RGB666 */
		tc->bpc = 6;
		break;
	default:
		dev_warn(tc->dev,
			 "unsupported LVDS bus format 0x%04x\n",
			 info->bus_formats[0]);
		return MODE_NOMODE;
	}

	return MODE_OK;
}

static int tc358775_parse_dt(struct device_node *np, struct tc_data *tc)
{
	struct device_node *endpoint;
	struct device_node *remote;
	int dsi_lanes = -1;

	endpoint = of_graph_get_endpoint_by_regs(tc->dev->of_node,
						 TC358775_DSI_IN, -1);
	dsi_lanes = drm_of_get_data_lanes_count(endpoint, 1, 4);

	/* Quirk old dtb: Use data lanes from the DSI host side instead of bridge */
	if (dsi_lanes == -EINVAL || dsi_lanes == -ENODEV) {
		remote = of_graph_get_remote_endpoint(endpoint);
		dsi_lanes = drm_of_get_data_lanes_count(remote, 1, 4);
		of_node_put(remote);
		if (dsi_lanes >= 1)
			dev_warn(tc->dev, "no dsi-lanes for the bridge, using host lanes\n");
	}

	of_node_put(endpoint);

	if (dsi_lanes < 0)
		return dsi_lanes;

	tc->num_dsi_lanes = dsi_lanes;

	tc->host_node = of_graph_get_remote_node(np, 0, 0);
	if (!tc->host_node)
		return -ENODEV;

	of_node_put(tc->host_node);

	endpoint = of_graph_get_endpoint_by_regs(tc->dev->of_node,
						 TC358775_LVDS_OUT1, -1);
	if (endpoint) {
		remote = of_graph_get_remote_port_parent(endpoint);
		of_node_put(endpoint);

		if (remote) {
			if (of_device_is_available(remote))
				tc->lvds_dual_link = true;
			of_node_put(remote);
		}
	}

	dev_dbg(tc->dev, "no.of dsi lanes: %d\n", tc->num_dsi_lanes);
	dev_dbg(tc->dev, "operating in %s-link mode\n",
		tc->lvds_dual_link ? "dual" : "single");

	return 0;
}

static int tc_bridge_attach(struct drm_bridge *bridge,
			    enum drm_bridge_attach_flags flags)
{
	struct tc_data *tc = bridge_to_tc(bridge);

	/* Attach the panel-bridge to the dsi bridge */
	return drm_bridge_attach(bridge->encoder, tc->panel_bridge,
				 &tc->bridge, flags);
}

static const struct drm_bridge_funcs tc_bridge_funcs = {
	.attach = tc_bridge_attach,
	.dsi_lp11_notify = tc358775_dsi_lp11_notify,
	.pre_enable = tc358775_bridge_pre_enable,
	.enable = tc358775_bridge_enable,
	.post_disable = tc358775_bridge_post_disable,
	.mode_fixup = tc_mode_fixup,
	.mode_valid = tc_mode_valid,
};

static int tc_attach_host(struct tc_data *tc)
{
	const struct mipi_dsi_device_info info = {
		.type = "tc358775",
		.channel = 0,
		.node = NULL,
	};
	struct device *dev = tc->dev;
	struct mipi_dsi_host *host;
	struct mipi_dsi_device *dsi;
	int ret;

	host = of_find_mipi_dsi_host_by_node(tc->host_node);
	if (!host)
		return dev_err_probe(dev, -EPROBE_DEFER, "failed to find dsi host\n");

	dsi = devm_mipi_dsi_device_register_full(dev, host, &info);
	if (IS_ERR(dsi)) {
		dev_err(dev, "failed to create dsi device\n");
		return PTR_ERR(dsi);
	}

	tc->dsi = dsi;

	dsi->lanes = tc->num_dsi_lanes;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;

	/*
	 * The hs_rate and lp_rate are data rate values. The HS mode is
	 * differential, while the LP mode is single ended. As the HS mode
	 * uses DDR, the DSI clock frequency is half the hs_rate. The 10 Mbs
	 * data rate for LP mode is not specified in the bridge data sheet,
	 * but seems to be part of the MIPI DSI spec.
	 */
	if (tc->type == TC358765)
		dsi->hs_rate = 800000000;
	else
		dsi->hs_rate = 1000000000;
	dsi->lp_rate = 10000000;

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		dev_err(dev, "failed to attach dsi to host\n");
		return ret;
	}

	return 0;
}

static const struct regmap_config tc358775_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.max_register = 0xffff,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int tc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tc_data *tc;
	int ret;

	tc = devm_kzalloc(dev, sizeof(*tc), GFP_KERNEL);
	if (!tc)
		return -ENOMEM;

	tc->dev = dev;
	tc->i2c = client;
	tc->type = (enum tc3587x5_type)(unsigned long)of_device_get_match_data(dev);

	tc->regmap = devm_regmap_init_i2c(client, &tc358775_regmap_config);
	if (IS_ERR(tc->regmap))
		return dev_err_probe(dev, PTR_ERR(tc->regmap),
				     "regmap i2c init failed\n");

	tc->panel_bridge = devm_drm_of_get_bridge(dev, dev->of_node,
						  TC358775_LVDS_OUT0, 0);
	if (IS_ERR(tc->panel_bridge))
		return PTR_ERR(tc->panel_bridge);

	ret = tc358775_parse_dt(dev->of_node, tc);
	if (ret)
		return ret;

	tc->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(tc->vddio))
		return PTR_ERR(tc->vddio);

	tc->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(tc->vdd))
		return PTR_ERR(tc->vdd);

	tc->stby_gpio = devm_gpiod_get_optional(dev, "stby", GPIOD_OUT_HIGH);
	if (IS_ERR(tc->stby_gpio))
		return PTR_ERR(tc->stby_gpio);

	tc->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(tc->reset_gpio)) {
		ret = PTR_ERR(tc->reset_gpio);
		dev_err(dev, "cannot get reset-gpios %d\n", ret);
		return ret;
	}

	tc->bridge.funcs = &tc_bridge_funcs;
	tc->bridge.of_node = dev->of_node;
	tc->bridge.pre_enable_prev_first = true;
	drm_bridge_add(&tc->bridge);

	i2c_set_clientdata(client, tc);

	ret = tc_attach_host(tc);
	if (ret)
		goto err_bridge_remove;

	return 0;

err_bridge_remove:
	drm_bridge_remove(&tc->bridge);
	return ret;
}

static void tc_remove(struct i2c_client *client)
{
	struct tc_data *tc = i2c_get_clientdata(client);

	drm_bridge_remove(&tc->bridge);
}

static const struct i2c_device_id tc358775_i2c_ids[] = {
	{ "tc358765", TC358765, },
	{ "tc358775", TC358775, },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tc358775_i2c_ids);

static const struct of_device_id tc358775_of_ids[] = {
	{ .compatible = "toshiba,tc358765", .data = (void *)TC358765, },
	{ .compatible = "toshiba,tc358775", .data = (void *)TC358775, },
	{ }
};
MODULE_DEVICE_TABLE(of, tc358775_of_ids);

static struct i2c_driver tc358775_driver = {
	.driver = {
		.name = "tc358775",
		.of_match_table = tc358775_of_ids,
	},
	.id_table = tc358775_i2c_ids,
	.probe = tc_probe,
	.remove	= tc_remove,
};
module_i2c_driver(tc358775_driver);

MODULE_AUTHOR("Vinay Simha BN <simhavcs@gmail.com>");
MODULE_DESCRIPTION("TC358775 DSI/LVDS bridge driver");
MODULE_LICENSE("GPL v2");
