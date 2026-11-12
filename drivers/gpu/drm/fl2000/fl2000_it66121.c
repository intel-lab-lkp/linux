// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include <linux/delay.h>
#include <linux/hdmi.h>
#include <linux/unaligned.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_edid.h>

#include "fl2000.h"
#include "fl2000_it66121.h"
#include "fl2000_it66121_regs.h"

#define VENDOR_ID      0x4954
#define DEVICE_ID      0x0612
#define REVISION_MASK  0xF000
#define REVISION_SHIFT 12

/* How often, and how far apart, to look for the chip on the bus */
#define IT66121_DETECT_TRIES	5
#define IT66121_DETECT_DELAY_MS	50

struct it66121_priv {
	struct i2c_adapter *adapter;
	struct i2c_client *client;
	struct regmap *regmap;
	struct drm_bridge bridge;

	/* The chip was reset and its HDCP engine may be holding the DDC bus */
	bool ddc_held;

	struct regmap_field *irq_pending;
	struct regmap_field *hpd;
	struct regmap_field *clr_irq;
};

static const struct regmap_range_cfg it66121_regmap_banks[] = {
	/*
	 * Do not put common registers to any range, this will lead to skipping "bank" configuration
	 * when accessing those at addresses 0x00-0x2F.
	 */
	{
		.name = "Banks",
		.range_min = IT66121_BANK_START,
		.range_max = IT66121_BANK_END,
		.selector_reg = IT66121_SYS_CONTROL,
		.selector_mask = IT66121_SYS_BANK_MASK,
		.selector_shift = 0,
		.window_start = IT66121_BANK_START,
		.window_len = IT66121_BANK_SIZE,
	},
};

static bool it66121_reg_volatile(struct device *dev, unsigned int reg)
{
	return IT66121_REG_VOLATILE(reg);
}

static const struct regmap_config it66121_regmap_config = {
	.val_bits = 8, /* 8-bit register size */
	.reg_bits = 8, /* 8-bit register address space */
	.reg_stride = 1,
	.max_register = 2 * IT66121_BANK_SIZE - 1, /* 2 banks of registers */

	.cache_type = REGCACHE_MAPLE,

	.volatile_reg = it66121_reg_volatile,

	.ranges = it66121_regmap_banks,
	.num_ranges = ARRAY_SIZE(it66121_regmap_banks),

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.use_single_read = true,
	.use_single_write = true,
};

/*
 * Poll sparsely: every status read is a transaction on the shared bus that
 * competes with the DDC engine's own transfer.
 */
#define DDC_POLL_SLEEP_US   5000
#define DDC_POLL_TIMEOUT_US 500000

#define DDC_CMD_ABORT 0xF

static int it66121_configure_input(struct it66121_priv *priv)
{
	int ret;

	/*
	 * This configures the bridge IC connection to the encoder IC, i.e. the
	 * interface between IT66121 and FL2000. It is fixed by the board design,
	 * so it is hardcoded:
	 *   - input mode RGB
	 *   - IO latch clock = TXCLK
	 *   - CCIR656 disabled
	 *   - embedded sync disabled
	 *   - DDR mode disabled
	 *   - 1 cycle input PCLK delay
	 */
	ret = regmap_write(priv->regmap, IT66121_INPUT_MODE,
			   IT66121_INPUT_MODE_RGB | IT66121_INPUT_PCLKDELAY1);
	if (ret)
		return ret;

	/*
	 * No color space conversion: the input is RGB and so is the output, so the
	 * conversion matrix and blank levels are left untouched.
	 */
	ret = regmap_write(priv->regmap, IT66121_INPUT_COLOR_CONV, IT66121_INPUT_NO_CONV);
	if (ret)
		return ret;

	return 0;
}

static int it66121_configure_afe(struct it66121_priv *priv, int clock_khz)
{
	int ret;

	ret = regmap_write(priv->regmap, IT66121_AFE_DRV_CONTROL, IT66121_AFE_RST);
	if (ret)
		return ret;

	/* Values from the vendor reference driver, the bits are undocumented */
	if (clock_khz > 80000) {
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, 0x90, 0x80);
		if (ret)
			return ret;
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, 0x89, 0x80);
		if (ret)
			return ret;
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_3, 0x10, 0);
		if (ret)
			return ret;
	} else {
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, 0x90, 0x10);
		if (ret)
			return ret;
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, 0x89, 0x09);
		if (ret)
			return ret;
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_3, 0x10, 0x10);
		if (ret)
			return ret;
	}

	/* Fire AFE */
	ret = regmap_write(priv->regmap, IT66121_AFE_DRV_CONTROL, 0);
	if (ret)
		return ret;

	return 0;
}

/* DDC status bits: done (7), active (6), noack/wait bus/arbitration lose (5:3) */
#define DDC_STATUS_TX_DONE BIT(7)

static int it66121_wait_ddc_ready(struct it66121_priv *priv)
{
	unsigned int val = 0;
	int ret;

	/*
	 * Wait for completion only. On this hardware the FL2000 I2C master and
	 * the IT66121 DDC master share the physical bus, so 'wait bus' and
	 * 'arbitration lose' flags are live/latched transients of the engine
	 * retrying on a contended bus, not terminal errors - the only reliable
	 * terminal signal is TX_DONE (or the timeout).
	 */
	ret = regmap_read_poll_timeout(priv->regmap, IT66121_DDC_STATUS, val,
				       val & DDC_STATUS_TX_DONE, DDC_POLL_SLEEP_US,
				       DDC_POLL_TIMEOUT_US);
	if (ret)
		dev_dbg(&priv->client->dev, "DDC not done (%d), status 0x%02X\n", ret, val);

	return ret;
}

/*
 * The HDCP engine shares the DDC master and holds the bus after chip reset (DDC status reports
 * 'wait bus' + 'arbitration lose', 0x1A), and that bus is the one the monitor's DDC is on. Follow
 * the ITE reference sequence, as the ite-it66121 driver does: clear HDCP CP_DESIRE, reset the HDCP
 * engine and re-select the host as DDC master before issuing the abort command.
 */
static int it66121_release_ddc(struct it66121_priv *priv)
{
	int ret;

	ret = regmap_write_bits(priv->regmap, IT66121_HDCP_CONTROL, IT66121_HDCP_CP_DESIRE, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(priv->regmap, IT66121_SW_RST, IT66121_SW_HDCP_RST,
				IT66121_SW_HDCP_RST);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, IT66121_DDC_CONTROL,
			   IT66121_DDC_MASTER_DDC | IT66121_DDC_MASTER_HOST);
	if (ret)
		return ret;

	/* The ITE reference driver issues the abort command twice */
	for (int i = 0; i < 2; i++) {
		ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND, DDC_CMD_ABORT);
		if (ret)
			return ret;

		ret = it66121_wait_ddc_ready(priv);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * it66121_hpd_connected() - live state of the HDMI hot plug detect line
 * @bridge: the IT66121 bridge
 *
 * Return: 1 if an HDMI monitor is connected, 0 if not, a negative error code if the line cannot
 * be read
 */
int it66121_hpd_connected(struct drm_bridge *bridge)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);
	unsigned int val;
	int idx, ret;

	if (!drm_bridge_enter(bridge, &idx))
		return -ENODEV;

	ret = regmap_field_read(priv->hpd, &val);
	drm_bridge_exit(idx);
	if (ret)
		return ret;

	return !!val;
}

/**
 * it66121_ack_interrupts() - acknowledge what is pending in the IT66121
 * @bridge: the IT66121 bridge
 *
 * Its interrupt line is routed to the FL2000, which reports it on its own interrupt endpoint (see
 * fl2000_interrupt.c): called from there, and hotplug is handled by probing the connector. The
 * interrupt sources themselves are not decoded here. An interrupt left pending would keep the line
 * from changing again.
 *
 * Return: 0 on success, a negative error code otherwise
 */
int it66121_ack_interrupts(struct drm_bridge *bridge)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);
	unsigned int val;
	int ret;

	ret = regmap_field_read(priv->irq_pending, &val);
	if (ret || !val)
		return ret;

	ret = regmap_read(priv->regmap, IT66121_INT_STATUS_1, &val);
	if (ret)
		return ret;

	/* The only two sources with the same bit in both registers, and the only ones unmasked */
	val &= IT66121_INT_STATUS_1_HPD | IT66121_INT_STATUS_1_RX_SENSE;
	if (val) {
		ret = regmap_write(priv->regmap, IT66121_INT_CLEAR_1, val);
		if (ret)
			return ret;
	}

	/* Forced: when the bit happens to read as set, a plain update would not write it */
	return regmap_field_force_write(priv->clr_irq, 1);
}

/*
 * The EDID is read with the FL2000's own I2C engine. On the Fresco reference designs the monitor's
 * DDC lines hang off the FL2000 I2C bus and the vendor driver reads the EDID at address 0x50 this
 * way. The IT66121's own EDID command cannot be used: it never completes on these boards, and
 * could not work behind this I2C engine anyway, which transfers aligned dwords: every read of the
 * DDC status register (0x16) also reads, and so pops, the DDC FIFO (0x17).
 */
static int it66121_get_edid_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct it66121_priv *priv = context;
	int ret;

	ret = fl2000_ddc_read_block(priv->adapter, buf, block, len);
	if (ret)
		dev_dbg(&priv->client->dev, "EDID block %u read failed (%d)\n", block, ret);

	return ret;
}

static enum drm_connector_status it66121_bridge_detect(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);
	int ret = it66121_hpd_connected(bridge);

	if (ret > 0)
		return connector_status_connected;

	/* Gone with the USB device */
	if (ret == -ENODEV)
		return connector_status_disconnected;

	if (ret < 0) {
		/* Not bridge->dev: the chip, not the DRM device, failed to answer */
		dev_err_ratelimited(&priv->client->dev, "Cannot get monitor status (%d)\n", ret);
		return connector_status_unknown;
	}

	return connector_status_disconnected;
}

static const struct drm_edid *it66121_bridge_edid_read(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);
	const struct drm_edid *drm_edid;
	int idx, ret;

	if (!drm_bridge_enter(bridge, &idx))
		return NULL;

	if (priv->ddc_held) {
		/* Not fatal: the read tells whether the bus is free */
		ret = it66121_release_ddc(priv);
		if (ret)
			dev_dbg(&priv->client->dev, "Cannot release the DDC bus (%d)\n", ret);
		priv->ddc_held = false;
	}

	drm_edid = drm_edid_read_custom(connector, it66121_get_edid_block, priv);
	if (!drm_edid)
		dev_warn(&priv->client->dev, "EDID read failed, using fallback modes\n");

	drm_bridge_exit(idx);

	return drm_edid;
}

/*
 * Bring the transmitter out of reset into a known state, as the IT66121 manual and the vendor
 * driver do: at attach, and again after system sleep, which may have cut the power.
 */
static void it66121_init_chip(struct it66121_priv *priv)
{
	/* Reset according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_SW_RST, IT66121_SW_REF_RST_HDMITX,
			  IT66121_SW_REF_RST_HDMITX);
	msleep(50);

	/* Power up GRCLK & power down IACLK, TxCLK, CRCLK */
	regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, (0xf << 3), (7 << 3));

	/* Continue according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_INT_CONTROL, (1 << 0), (0 << 0));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1 << 5), (0 << 5));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1 << 2) | (1 << 6),
			  (0 << 2) | (0 << 6));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1 << 6), (0 << 6));
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_CONTROL, (1 << 4), (0 << 4));
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_CONTROL, (1 << 3), (1 << 3));
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_1, (1 << 2), (1 << 2));

	/* Extra AFE steps from the vendor driver */
	/* Whole register is defined as XP_TEST, values are undisclosed */
	regmap_write_bits(priv->regmap, IT66121_AFE_XP_TEST, 0xFF, 0x70);
	/* Lower 5 bits are undisclosed in manual */
	regmap_write_bits(priv->regmap, IT66121_AFE_DRV_HS, 0xFF, 0x1F);
	/*
	 * DRV_ISW[5:3] value '011' is a default output current level swing, with change to '111' we
	 * set output current level swing to maximum.
	 */
	regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_2, 0xFF, 0x38);

	/* Power up IACLK, TxCLK */
	regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, (3 << 4), (0 << 4));

	/* Reset according to IT66121 manual */
	regmap_write_bits(priv->regmap, IT66121_SW_RST, IT66121_SW_REF_RST_HDMITX,
			  IT66121_SW_REF_RST_HDMITX);
	msleep(50);

	/* We do not support HDCP so its ok to statically set host controls */
	regmap_write(priv->regmap, IT66121_DDC_CONTROL,
		     IT66121_DDC_MASTER_DDC | IT66121_DDC_MASTER_HOST);

	priv->ddc_held = true;
}

/*
 * Unmask the hot plug interrupt, the one that means something here. DDC errors have nobody to read
 * them: the DDC engine is not used.
 */
static void it66121_start_interrupts(struct it66121_priv *priv)
{
	regmap_write_bits(priv->regmap, IT66121_INT_MASK_1, IT66121_MASK_DDC | IT66121_MASK_HPD,
			  IT66121_MASK_DDC);
	it66121_ack_interrupts(&priv->bridge);
}

static int it66121_bridge_attach(struct drm_bridge *bridge, struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	/* The connector is the display driver's (drm_bridge_connector_init()) */
	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)) {
		drm_err(bridge->dev, "The IT66121 bridge does not create a connector\n");
		return -EINVAL;
	}

	if (!bridge->encoder) {
		drm_err(bridge->dev, "Parent encoder object not found\n");
		return -ENODEV;
	}

	it66121_init_chip(priv);

	it66121_start_interrupts(priv);

	drm_dbg(bridge->dev, "Bridge attached\n");

	return 0;
}

/**
 * it66121_resume() - set the IT66121 up again after system sleep
 * @bridge: the IT66121 bridge
 *
 * The dongle is bus powered, and the test laptop resets its USB devices when it wakes from
 * suspend to RAM: the chip may have lost every register. The register cache would then be wrong,
 * and the read-modify-write updates of the initialisation would skip writes the chip still needs:
 * drop it, so that values are read from the chip again. The mode is set again by the DRM core.
 */
void it66121_resume(struct drm_bridge *bridge)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	regcache_drop_region(priv->regmap, 0, it66121_regmap_config.max_register);
	it66121_init_chip(priv);
	it66121_start_interrupts(priv);
}

/*
 * Program the transmitter for @mode, as the monitor behind @connector wants it (HDMI, or DVI when
 * its EDID says so or there is none). The FL2000 already clocks the input at this point.
 */
static void it66121_set_mode(struct it66121_priv *priv, struct drm_connector *connector,
			     const struct drm_display_mode *mode)
{
	struct drm_bridge *bridge = &priv->bridge;
	bool dvi = !connector->display_info.is_hdmi;
	struct hdmi_avi_infoframe frame;
	ssize_t frame_size;
	u8 buf[HDMI_INFOFRAME_SIZE(AVI)];
	int ret;
	const u16 aviinfo_reg[HDMI_AVI_INFOFRAME_SIZE] = {
		IT66121_HDMI_AVIINFO_DB1,  IT66121_HDMI_AVIINFO_DB2,  IT66121_HDMI_AVIINFO_DB3,
		IT66121_HDMI_AVIINFO_DB4,  IT66121_HDMI_AVIINFO_DB5,  IT66121_HDMI_AVIINFO_DB6,
		IT66121_HDMI_AVIINFO_DB7,  IT66121_HDMI_AVIINFO_DB8,  IT66121_HDMI_AVIINFO_DB9,
		IT66121_HDMI_AVIINFO_DB10, IT66121_HDMI_AVIINFO_DB11, IT66121_HDMI_AVIINFO_DB12,
		IT66121_HDMI_AVIINFO_DB13
	};

	drm_dbg(bridge->dev, "Setting %s mode " DRM_MODE_FMT "\n", dvi ? "DVI" : "HDMI",
		DRM_MODE_ARG(mode));

	ret = drm_hdmi_avi_infoframe_from_display_mode(&frame, connector, mode);
	if (ret) {
		drm_err(bridge->dev, "Cannot create AVI infoframe (%d)\n", ret);
		return;
	}

	frame_size = hdmi_avi_infoframe_pack(&frame, buf, sizeof(buf));
	if (frame_size < 0) {
		drm_err(bridge->dev, "Cannot pack AVI infoframe (%zd)\n", frame_size);
		return;
	}

	/* Set TX mode */
	ret = regmap_write(priv->regmap, IT66121_HDMI_MODE,
			   dvi ? IT66121_HDMI_MODE_DVI : IT66121_HDMI_MODE_HDMI);
	if (ret) {
		drm_err(bridge->dev, "Cannot set TX mode (%d)\n", ret);
		return;
	}

	/* Enable HDMI packets, repeat for every data frame as recommended */
	ret = regmap_write(priv->regmap, IT66121_HDMI_GEN_CTRL_PKT,
			   IT66121_HDMI_GEN_CTRL_PKT_ON | IT66121_HDMI_GEN_CTRL_PKT_RPT);
	if (ret) {
		drm_err(bridge->dev, "Cannot enable HDMI packets\n");
		return;
	}

	/* Mute AV */
	ret = regmap_write(priv->regmap, IT66121_HDMI_AV_MUTE,
			   IT66121_HDMI_AV_MUTE_ON | IT66121_HDMI_AV_MUTE_BLUE);
	if (ret) {
		drm_err(bridge->dev, "Cannot mute AV\n");
		return;
	}

	/* Write new AVI infoframe packet */
	for (int i = 0; i < HDMI_AVI_INFOFRAME_SIZE; i++) {
		ret = regmap_write(priv->regmap, aviinfo_reg[i],
				   buf[i + HDMI_INFOFRAME_HEADER_SIZE]);
		if (ret) {
			drm_err(bridge->dev, "Cannot write AVI infoframe byte %d (%d)\n", i, ret);
			return;
		}
	}
	ret = regmap_write(priv->regmap, IT66121_HDMI_AVIINFO_CSUM, buf[3]);
	if (ret) {
		drm_err(bridge->dev, "Cannot write AVI infoframe checksum (%d)\n", ret);
		return;
	}

	/* Enable AVI infoframe */
	ret = regmap_write(priv->regmap, IT66121_HDMI_AVI_INFO_PKT,
			   IT66121_HDMI_AVI_INFO_PKT_ON | IT66121_HDMI_AVI_INFO_RPT);
	if (ret) {
		drm_err(bridge->dev, "Cannot enable AVI infoframe (%d)\n", ret);
		return;
	}

	/* Set reset flags */
	ret = regmap_write_bits(priv->regmap, IT66121_SW_RST,
				IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST,
				IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST);
	if (ret)
		return;

	/* Disable TXCLK prior to configuration */
	ret = regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, IT66121_SYS_TXCLK_OFF,
				IT66121_SYS_TXCLK_OFF);
	if (ret)
		return;

	/* Configure connection, conversions, etc. */
	ret = it66121_configure_input(priv);
	if (ret) {
		drm_err(bridge->dev, "Cannot configure input bus (%d)\n", ret);
		return;
	}

	/* Configure AFE */
	ret = it66121_configure_afe(priv, mode->clock);
	if (ret) {
		drm_err(bridge->dev, "Cannot configure AFE (%d)\n", ret);
		return;
	}

	/* Clear reset flags */
	ret = regmap_write_bits(priv->regmap, IT66121_SW_RST,
				IT66121_SW_REF_RST_HDMITX | IT66121_SW_HDMI_VID_RST, 0);
	if (ret)
		return;

	/* Enable TXCLK */
	ret = regmap_write_bits(priv->regmap, IT66121_SYS_CONTROL, IT66121_SYS_TXCLK_OFF, 0);
	if (ret)
		return;
}

/*
 * The mode is programmed here rather than in .mode_set, which has no atomic state: the connector,
 * which tells HDMI from DVI and goes into the AVI infoframe, is only known from the state.
 */
static void it66121_bridge_atomic_enable(struct drm_bridge *bridge,
					 struct drm_atomic_commit *state)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);
	const struct drm_connector_state *conn_state;
	const struct drm_crtc_state *crtc_state;
	struct drm_connector *connector;
	int idx;

	connector = drm_atomic_get_new_connector_for_encoder(state, bridge->encoder);
	if (WARN_ON(!connector))
		return;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (WARN_ON(!conn_state || !conn_state->crtc))
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (WARN_ON(!crtc_state))
		return;

	if (!drm_bridge_enter(bridge, &idx))
		return;

	it66121_set_mode(priv, connector, &crtc_state->mode);

	/* Unmute AV */
	regmap_write(priv->regmap, IT66121_HDMI_AV_MUTE, 0);

	drm_bridge_exit(idx);
}

static void it66121_bridge_atomic_disable(struct drm_bridge *bridge,
					  struct drm_atomic_commit *state)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);
	int idx;

	if (!drm_bridge_enter(bridge, &idx))
		return;

	/* Mute AV */
	regmap_write_bits(priv->regmap, IT66121_HDMI_AV_MUTE, IT66121_HDMI_AV_MUTE_ON,
			  IT66121_HDMI_AV_MUTE_ON);

	drm_bridge_exit(idx);
}

static const struct drm_bridge_funcs it66121_bridge_funcs = {
	.attach = it66121_bridge_attach,
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_enable = it66121_bridge_atomic_enable,
	.atomic_disable = it66121_bridge_atomic_disable,
	.detect = it66121_bridge_detect,
	.edid_read = it66121_bridge_edid_read,
};

static int it66121_regs_init(struct it66121_priv *priv, struct i2c_client *client)
{
	priv->regmap = devm_regmap_init_i2c(client, &it66121_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->irq_pending =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_SYS_STATUS_irq_pending);
	priv->hpd = devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_SYS_STATUS_hpd);
	priv->clr_irq =
		devm_regmap_field_alloc(&client->dev, priv->regmap, IT66121_SYS_STATUS_clr_irq);
	if (IS_ERR(priv->irq_pending) || IS_ERR(priv->hpd) || IS_ERR(priv->clr_irq))
		return -ENOMEM;

	return 0;
}

/*
 * The callback of i2c_new_scanned_device(): nonzero if the device is there. It reads the IDs with
 * the same transfers as the regmap, which is all this adapter can do (see its quirks).
 */
static int it66121_i2c_probe(struct i2c_adapter *adapter, unsigned short address)
{
	int ret;
	u8 id_regs[] = { IT66121_VENDOR_ID_1, IT66121_VENDOR_ID_2, IT66121_DEVICE_ID_1,
			 IT66121_DEVICE_ID_2 };
	u8 id[ARRAY_SIZE(id_regs)];
	u16 vendor, device;

	dev_dbg(&adapter->dev, "Detecting IT66121 at address 0x%X on %s\n", address,
		adapter->name);

	for (int i = 0; i < ARRAY_SIZE(id_regs); i++) {
		struct i2c_msg msgs[] = {
			{ .addr = address, .flags = 0, .len = 1, .buf = &id_regs[i] },
			{ .addr = address, .flags = I2C_M_RD, .len = 1, .buf = &id[i] }
		};

		ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
		if (ret != ARRAY_SIZE(msgs)) {
			dev_dbg(&adapter->dev, "No answer at address 0x%X (%d)\n", address, ret);
			return 0;
		}
	}

	vendor = get_unaligned_le16(&id[0]);
	device = get_unaligned_le16(&id[2]);
	if (vendor != VENDOR_ID || (device & ~REVISION_MASK) != DEVICE_ID) {
		dev_warn(&adapter->dev, "Not an IT66121 at address 0x%X (0x%04X-0x%04X)\n", address,
			 vendor, device);
		return 0;
	}

	dev_dbg(&adapter->dev, "IT66121 found, revision %d\n",
		(device & REVISION_MASK) >> REVISION_SHIFT);

	return 1;
}

static struct i2c_client *it66121_i2c_init(struct i2c_adapter *adapter)
{
	/*
	 * Not "it66121": that name would make the ite-it66121 driver, if built,
	 * try to bind this client and release our devres when its probe fails.
	 */
	struct i2c_board_info board_info = { I2C_BOARD_INFO("fl2000-it66121", 0) };

	/* According to datasheet IT66121 addresses are 0x98 or 0x9A including cmd */
	const unsigned short it66121_addr[] = { (0x98 >> 1), (0x9A >> 1), I2C_CLIENT_END };
	struct i2c_client *client;

	/* The chip may still be waking up when the USB device has just enumerated */
	for (int i = 0; i < IT66121_DETECT_TRIES; i++) {
		if (i)
			msleep(IT66121_DETECT_DELAY_MS);

		client = i2c_new_scanned_device(adapter, &board_info, it66121_addr,
						it66121_i2c_probe);
		if (!IS_ERR(client))
			break;
	}

	return client;
}

/**
 * it66121_destroy() - remove the bridge
 * @bridge: the IT66121 bridge
 *
 * A DRM device that userspace keeps open still has the bridge in its encoder's chain, and frees it
 * with the last reference, but nothing gets past drm_bridge_enter() any more.
 */
void it66121_destroy(struct drm_bridge *bridge)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	drm_bridge_unplug(bridge);

	/*
	 * Releases what is managed on the client, its regmap included, and drops the devm reference
	 * to the bridge.
	 */
	i2c_unregister_device(priv->client);
}

/**
 * it66121_create() - find the IT66121 on the FL2000 I2C bus and make a bridge of it
 * @adapter: the FL2000 I2C adapter
 *
 * Return: the bridge, to attach to the encoder, or an ERR_PTR() on failure
 */
struct drm_bridge *it66121_create(struct i2c_adapter *adapter)
{
	int ret;
	struct it66121_priv *priv;
	struct i2c_client *client;

	client = it66121_i2c_init(adapter);
	if (IS_ERR(client)) {
		dev_err(&adapter->dev, "No IT66121 found (%pe): adapters without one are not supported\n",
			client);
		return ERR_CAST(client);
	}

	/*
	 * DRM core takes references on the bridge, so it must be allocated with
	 * devm_drm_bridge_alloc(). Freeing happens automatically when the last
	 * reference is dropped after i2c_unregister_device().
	 */
	priv = devm_drm_bridge_alloc(&client->dev, struct it66121_priv, bridge,
				     &it66121_bridge_funcs);
	if (IS_ERR(priv)) {
		i2c_unregister_device(client);
		return ERR_CAST(priv);
	}

	priv->client = client;
	priv->adapter = adapter;

	ret = it66121_regs_init(priv, priv->client);
	if (ret) {
		dev_err(&client->dev, "Cannot initialize registers (%d)\n", ret);
		i2c_unregister_device(client);
		return ERR_PTR(ret);
	}

	/*
	 * The chip's interrupt reaches the host only through the FL2000's, which sends a hotplug
	 * event for the whole device (fl2000_drm_hotplug()). The bridge therefore reports no hot
	 * plug of its own (DRM_BRIDGE_OP_HPD), and the connector also polls the line.
	 */
	priv->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	priv->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID;

	drm_bridge_add(&priv->bridge);

	return &priv->bridge;
}
