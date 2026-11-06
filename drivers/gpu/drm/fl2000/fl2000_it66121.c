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

/* HW defect leading to losing 3 first bytes during EDID read operation */
#define EDID_LOSS_LEN 3

#define VENDOR_ID      0x4954
#define DEVICE_ID      0x0612
#define REVISION_MASK  0xF000
#define REVISION_SHIFT 12

#define IRQ_POLL_INTRVL 100

struct it66121_priv {
	struct i2c_adapter *adapter;
	struct i2c_client *client;
	struct regmap *regmap;
	struct drm_bridge bridge;

	struct delayed_work work;
	struct workqueue_struct *work_queue;

	/* Serializes users of the DDC engine (multi-step EDID FIFO reads) */
	struct mutex ddc_lock;
	/* The EDID is read through the FL2000 bus, not the IT66121 FIFO, on this board */
	bool edid_direct;
	/* Set when the monitor went away: the cached EDID is no longer valid */
	bool edid_stale;

	struct regmap_field *irq_pending;
	struct regmap_field *hpd;
	struct regmap_field *clr_irq;

	/* EDID of the monitor, kept for as long as it stays; NULL if it could not be read */
	const struct drm_edid *drm_edid;
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
#define EDID_SLEEP   5000
#define EDID_TIMEOUT 500000

#define EDID_HDCP_ADDR 0x74
#define EDID_DDC_ADDR  0xA0

#define EDID_FIFO_SIZE 32

enum it66121_ddc_cmd {
	DDC_CMD_BURST_READ = 0x0,
	DDC_CMD_LINK_CHECK = 0x2,
	DDC_CMD_EDID_READ = 0x3,
	DDC_CMD_ASKV_WRITE = 0x4,
	DDC_CMD_AINFO_WRITE = 0x5,
	DDC_CMD_AN_WRITE = 0x6,
	DDC_CMD_FIFO_CLEAR = 0x9,
	DDC_CMD_SCL_PULSE = 0xA,
	DDC_CMD_ABORT = 0xF,
};

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
		ret = regmap_write_bits(priv->regmap, IT66121_AFE_IP_CONTROL_3, 0x10, 0x80);
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
#define DDC_STATUS_TX_DONE    BIT(7)
#define DDC_STATUS_ACTIVE     BIT(6)
#define DDC_STATUS_ERROR_MASK GENMASK(5, 3)

static int it66121_wait_ddc_ready(struct it66121_priv *priv)
{
	int ret;
	unsigned int val;

	/*
	 * Wait for completion only. On this hardware the FL2000 I2C master and
	 * the IT66121 DDC master share the physical bus, so 'wait bus' and
	 * 'arbitration lose' flags are live/latched transients of the engine
	 * retrying on a contended bus, not terminal errors - the only reliable
	 * terminal signal is TX_DONE (or the timeout).
	 */
	ret = regmap_read_poll_timeout(priv->regmap, IT66121_DDC_STATUS, val,
				       val & DDC_STATUS_TX_DONE, EDID_SLEEP, EDID_TIMEOUT);
	if (ret) {
		dev_err(&priv->client->dev, "DDC not done (%d), status 0x%02X\n", ret, val);
		return ret;
	}

	return 0;
}

static int it66121_clear_ddc_fifo(struct it66121_priv *priv)
{
	int ret;

	ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND, DDC_CMD_FIFO_CLEAR);
	if (ret)
		return ret;

	return 0;
}

static int it66121_abort_ddc_ops(struct it66121_priv *priv)
{
	int ret;
	unsigned int swreset, cpdesire;

	/*
	 * The HDCP engine shares the DDC master and holds the bus after chip
	 * reset (DDC status reports 'wait bus' + 'arbitration lose', 0x1A).
	 * Follow the ITE reference sequence, as the ite-it66121 driver does:
	 * clear HDCP CP_DESIRE, reset the HDCP engine and re-select the host as
	 * DDC master before issuing the abort command.
	 */
	ret = regmap_read(priv->regmap, IT66121_SW_RST, &swreset);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, IT66121_HDCP_CONTROL, &cpdesire);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, IT66121_HDCP_CONTROL,
			   cpdesire & ~IT66121_HDCP_CP_DESIRE);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, IT66121_SW_RST, swreset | IT66121_SW_HDCP_RST);
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
	int ret;

	ret = regmap_field_read(priv->hpd, &val);
	if (ret)
		return ret;

	return !!val;
}

/*
 * Acknowledges pending IT66121 interrupts. The IT66121 interrupt line is
 * routed to the FL2000, which reports it on its own interrupt endpoint (see
 * fl2000_interrupt.c); hotplug is handled from there, by re-probing the
 * connector. The interrupt sources themselves are not decoded here.
 */
static void it66121_intr_work(struct work_struct *work_item)
{
	int ret;
	unsigned int val;
	struct delayed_work *dwork = to_delayed_work(work_item);
	struct it66121_priv *priv = container_of(dwork, struct it66121_priv, work);
	/* Do not use bridge.dev here: it is NULL while the bridge is detached */
	struct device *dev = &priv->client->dev;

	ret = regmap_field_read(priv->irq_pending, &val);
	if (ret) {
		dev_err(dev, "Cannot read interrupt status (%d)\n", ret);
		return;
	}

	if (val) {
		ret = regmap_read(priv->regmap, IT66121_INT_STATUS_1, &val);
		if (ret) {
			dev_err(dev, "Cannot read IT66121_INT_STATUS_1 (%d)\n", ret);
			return;
		}

		regmap_field_write(priv->clr_irq, 1);
	}

	queue_delayed_work(priv->work_queue, &priv->work, msecs_to_jiffies(IRQ_POLL_INTRVL));
}

/*
 * EDID read through the IT66121 DDC master (EDID FIFO command), for boards
 * where the monitor's DDC lines are wired to the IT66121.
 */
static int it66121_do_get_edid_block(struct it66121_priv *priv, u8 *buf, unsigned int block,
				     size_t len)
{
	int ret;
	size_t remain = len;
	unsigned int val;
	unsigned int segment = block >> 1;
	unsigned int offset = (block & 1) ? 128 : 0;
	static const u8 header[EDID_LOSS_LEN] = { 0x00, 0xFF, 0xFF };

	/* Abort any ongoing DDC operation */
	ret = it66121_abort_ddc_ops(priv);
	if (ret)
		return ret;

	/*
	 * Statically fill first 3 bytes (due to EDID reading HW bug they are
	 * lost, but they are constant in any valid EDID header).
	 */
	while (offset < EDID_LOSS_LEN && remain > 0) {
		*(buf++) = header[offset];
		remain--;
		offset++;
	}

	while (remain > 0) {
		/* Add bytes that will be lost during EDID read */
		size_t size = remain + EDID_LOSS_LEN;

		/* ... and check size fits FIFO */
		size = min_t(size_t, size, EDID_FIFO_SIZE);

		ret = it66121_clear_ddc_fifo(priv);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, IT66121_DDC_ADDRESS, EDID_DDC_ADDR);
		if (ret)
			return ret;

		/* Account 3 bytes that will be lost */
		ret = regmap_write(priv->regmap, IT66121_DDC_OFFSET, offset - EDID_LOSS_LEN);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, IT66121_DDC_SIZE, (unsigned int)size);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, IT66121_DDC_SEGMENT, segment);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, IT66121_DDC_COMMAND, DDC_CMD_EDID_READ);
		if (ret)
			return ret;

		ret = it66121_wait_ddc_ready(priv);
		if (ret) {
			it66121_abort_ddc_ops(priv);
			return ret;
		}

		/* Deduct lost bytes when reading from FIFO */
		size -= EDID_LOSS_LEN;

		for (size_t i = 0; i < size; i++) {
			ret = regmap_read(priv->regmap, IT66121_DDC_RD_FIFO, &val);
			if (ret)
				return ret;

			*(buf++) = val & 0xFF;
		}

		remain -= size;
		offset += size;
	}

	return 0;
}

static int it66121_get_edid_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct it66121_priv *priv = context;
	int ret;

	mutex_lock(&priv->ddc_lock);
	ret = it66121_do_get_edid_block(priv, buf, block, len);
	mutex_unlock(&priv->ddc_lock);

	if (ret)
		dev_dbg(&priv->client->dev, "EDID block %u read failed (%d)\n", block, ret);

	return ret;
}

/*
 * Fallback EDID path: on Fresco reference designs the monitor's DDC lines hang
 * directly off the FL2000 I2C bus, and the vendor driver reads EDID at address
 * 0x50 with the FL2000's own dword I2C engine, bypassing the IT66121 DDC
 * master entirely. The engine reads 4 bytes per operation with an 8-bit
 * offset, which covers EDID blocks 0 and 1.
 */
static int it66121_get_edid_block_direct(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct it66121_priv *priv = context;
	int ret;

	ret = fl2000_ddc_read_block(priv->adapter, buf, block, len);
	if (ret)
		dev_dbg(&priv->client->dev, "Direct EDID block %u read failed (%d)\n", block, ret);

	return ret;
}

static void it66121_read_edid(struct it66121_priv *priv, struct drm_connector *connector)
{
	int (*first)(void *context, u8 *buf, unsigned int block, size_t len);
	int (*second)(void *context, u8 *buf, unsigned int block, size_t len);

	/*
	 * Board wirings differ: try the direct FL2000-bus DDC read if the
	 * IT66121 EDID FIFO path did not work, and start with whichever worked
	 * last time so the FIFO path's timeouts are paid once.
	 */
	if (priv->edid_direct) {
		first = it66121_get_edid_block_direct;
		second = it66121_get_edid_block;
	} else {
		first = it66121_get_edid_block;
		second = it66121_get_edid_block_direct;
	}

	drm_edid_free(priv->drm_edid);
	priv->drm_edid = drm_edid_read_custom(connector, first, priv);
	if (!priv->drm_edid) {
		dev_dbg(&priv->client->dev, "EDID read failed, trying the other DDC path\n");
		priv->drm_edid = drm_edid_read_custom(connector, second, priv);
		if (priv->drm_edid)
			priv->edid_direct = !priv->edid_direct;
	}
}

static enum drm_connector_status it66121_bridge_detect(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);
	int ret = it66121_hpd_connected(bridge);

	if (ret > 0)
		return connector_status_connected;

	/* Whatever comes next may be another monitor: its EDID must be read again */
	WRITE_ONCE(priv->edid_stale, true);

	if (ret < 0) {
		/* Not bridge->dev: the chip, not the DRM device, failed to answer */
		dev_err_ratelimited(&priv->client->dev, "Cannot get monitor status (%d)\n", ret);
		return connector_status_unknown;
	}

	return connector_status_disconnected;
}

/*
 * Reading the EDID takes about a second over USB, and the connector asks for it on every probe,
 * with the mode config locked: keep it until the monitor goes away (see it66121_bridge_detect())
 * and hand out copies. It must still be handed over every time: the DRM core drops the
 * connector's copy when the connector goes through 'disconnected'.
 */
static const struct drm_edid *it66121_bridge_edid_read(struct drm_bridge *bridge,
						       struct drm_connector *connector)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	if (!priv->drm_edid || READ_ONCE(priv->edid_stale)) {
		WRITE_ONCE(priv->edid_stale, false);
		it66121_read_edid(priv, connector);
		if (!priv->drm_edid)
			dev_warn(&priv->client->dev, "EDID read failed, using fallback modes\n");
	}

	return drm_edid_dup(priv->drm_edid);
}

static int it66121_bind(struct device *comp, struct device *master, void *master_data)
{
	int ret;
	struct drm_bridge *bridge = dev_get_drvdata(comp);
	struct drm_device *drm = master_data;
	struct i2c_adapter *adapter = i2c_verify_adapter(master);
	struct drm_encoder *encoder;

	if (!adapter)
		return -ENODEV;

	/* The FL2000 has a single encoder */
	encoder = list_first_entry_or_null(&drm->mode_config.encoder_list, struct drm_encoder,
					   head);
	if (!encoder)
		return -ENODEV;

	dev_dbg(comp, "Binding IT66121 component\n");

	/* The display driver makes the connector, from what the bridge reports */
	ret = drm_bridge_attach(encoder, bridge, NULL, DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		dev_err(comp, "Cannot attach IT66121 bridge (%d)\n", ret);

	return ret;
}

static void it66121_unbind(struct device *comp, struct device *master, void *master_data)
{
	/* The bridge is detached by the DRM core when the encoder is cleaned up */
	dev_dbg(comp, "Unbinding IT66121 component\n");
}

static const struct component_ops it66121_component_ops = {
	.bind = it66121_bind,
	.unbind = it66121_unbind,
};

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
}

/* Unmask the DDC interrupt and start acknowledging the chip's interrupts */
static void it66121_start_interrupts(struct it66121_priv *priv)
{
	regmap_write_bits(priv->regmap, IT66121_INT_MASK_1, IT66121_MASK_DDC, 0);
	queue_delayed_work(priv->work_queue, &priv->work, msecs_to_jiffies(IRQ_POLL_INTRVL));
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

	drm_dbg_kms(bridge->dev, "Bridge attached\n");

	return 0;
}

static void it66121_bridge_detach(struct drm_bridge *bridge)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	/*
	 * Stop the HPD polling work started by attach so it cannot notify a
	 * bridge that is no longer attached to a DRM device.
	 */
	cancel_delayed_work_sync(&priv->work);
}

/**
 * it66121_suspend() - stop talking to the IT66121 before system sleep
 * @bridge: the IT66121 bridge
 *
 * Its interrupt polling would fail while the USB device sleeps, and a failed read ends it.
 */
void it66121_suspend(struct drm_bridge *bridge)
{
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	cancel_delayed_work_sync(&priv->work);
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

	/* Another monitor may have been plugged in meanwhile */
	WRITE_ONCE(priv->edid_stale, true);
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

	drm_dbg_kms(bridge->dev, "Setting %s mode " DRM_MODE_FMT "\n", dvi ? "DVI" : "HDMI",
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

	connector = drm_atomic_get_new_connector_for_encoder(state, bridge->encoder);
	if (WARN_ON(!connector))
		return;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (WARN_ON(!conn_state || !conn_state->crtc))
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (WARN_ON(!crtc_state))
		return;

	it66121_set_mode(priv, connector, &crtc_state->mode);

	/* Unmute AV */
	regmap_write(priv->regmap, IT66121_HDMI_AV_MUTE, 0);
}

static void it66121_bridge_atomic_disable(struct drm_bridge *bridge,
					  struct drm_atomic_commit *state)
{
	int ret;
	struct it66121_priv *priv = container_of(bridge, struct it66121_priv, bridge);

	/* Mute AV */
	ret = regmap_write_bits(priv->regmap, IT66121_HDMI_AV_MUTE, IT66121_HDMI_AV_MUTE_ON,
				IT66121_HDMI_AV_MUTE_ON);
	if (ret)
		return;
}

static const struct drm_bridge_funcs it66121_bridge_funcs = {
	.attach = it66121_bridge_attach,
	.detach = it66121_bridge_detach,
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

static int it66121_i2c_probe(struct i2c_adapter *adapter, unsigned short address)
{
	int ret;
	u8 id_regs[] = { IT66121_VENDOR_ID_1, IT66121_VENDOR_ID_2, IT66121_DEVICE_ID_1,
			 IT66121_DEVICE_ID_2 };
	u8 id[ARRAY_SIZE(id_regs)];
	u16 vendor, device;

	dev_dbg(&adapter->dev, "Detecting IT66121 at address 0x%X on %s\n", address,
		adapter->name);

	/* We rely on full I2C protocol + 1 byte SMBUS read for detection */
	ret = i2c_check_functionality(adapter, I2C_FUNC_I2C | I2C_FUNC_SMBUS_READ_BYTE);
	if (!ret) {
		dev_dbg(&adapter->dev, "Adapter does not support I2C functions properly\n");
		return -ENODEV;
	}

	for (int i = 0; i < ARRAY_SIZE(id_regs); i++) {
		struct i2c_msg msgs[] = {
			{ .addr = address, .flags = 0, .len = 1, .buf = &id_regs[i] },
			{ .addr = address, .flags = I2C_M_RD, .len = 1, .buf = &id[i] }
		};

		ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
		if (ret < 0) {
			dev_err(&adapter->dev, "I2C transfer failed (%d)\n", ret);
			return -ENODEV;
		}
	}

	vendor = get_unaligned_le16(&id[0]);
	device = get_unaligned_le16(&id[2]);
	if (vendor != VENDOR_ID || (device & ~REVISION_MASK) != DEVICE_ID) {
		dev_dbg(&adapter->dev, "IT66121 not found (0x%X-0x%X)\n", vendor, device);
		return -ENODEV;
	}

	dev_dbg(&adapter->dev, "IT66121 found, revision %d\n",
		(device & REVISION_MASK) >> REVISION_SHIFT);

	return 0;
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

	return i2c_new_scanned_device(adapter, &board_info, it66121_addr, it66121_i2c_probe);
}

void it66121_destroy(struct i2c_client *client)
{
	struct drm_bridge *bridge;
	struct it66121_priv *priv;

	if (!client)
		return;

	bridge = dev_get_drvdata(&client->dev);
	priv = container_of(bridge, struct it66121_priv, bridge);

	cancel_delayed_work_sync(&priv->work);

	destroy_workqueue(priv->work_queue);

	component_del(&priv->client->dev, &it66121_component_ops);

	drm_edid_free(priv->drm_edid);
	priv->drm_edid = NULL;

	drm_bridge_remove(&priv->bridge);

	/*
	 * Drops the devm reference to the bridge; priv is freed together with
	 * the bridge once DRM releases the remaining references.
	 */
	i2c_unregister_device(priv->client);
}

struct i2c_client *it66121_create(struct i2c_adapter *adapter)
{
	int ret;
	struct it66121_priv *priv;
	struct i2c_client *client;

	client = it66121_i2c_init(adapter);
	if (IS_ERR(client)) {
		dev_err(&adapter->dev, "Cannot find IT66121 I2C client\n");
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

	INIT_DELAYED_WORK(&priv->work, &it66121_intr_work);
	mutex_init(&priv->ddc_lock);

	/*
	 * The chip's interrupt reaches the host only through the FL2000's, which sends a hotplug
	 * event for the whole device (fl2000_drm_hotplug()). The bridge therefore reports no hot
	 * plug of its own (DRM_BRIDGE_OP_HPD), and the connector also polls the line.
	 */
	priv->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	priv->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID;

	drm_bridge_add(&priv->bridge);

	/* Setup work queue for interrupt processing work */
	priv->work_queue = alloc_ordered_workqueue("it66121_work", 0);
	if (!priv->work_queue) {
		drm_bridge_remove(&priv->bridge);
		i2c_unregister_device(client);
		return ERR_PTR(-ENOMEM);
	}

	/* Store bridge pointer at I2C client device for further binding */
	dev_set_drvdata(&priv->client->dev, &priv->bridge);

	/* Bind component of I2C client to master I2C adapter */
	ret = component_add(&priv->client->dev, &it66121_component_ops);
	if (ret) {
		dev_err(&client->dev, "Cannot register IT66121 component (%d)\n", ret);
		destroy_workqueue(priv->work_queue);
		drm_bridge_remove(&priv->bridge);
		i2c_unregister_device(client);
		return ERR_PTR(ret);
	}

	return client;
}
