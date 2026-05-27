/*
 * Copyright (c) 2015 NVIDIA Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/bitfield.h>
#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/overflow.h>

#include <drm/display/drm_scdc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

/**
 * DOC: scdc helpers
 *
 * Status and Control Data Channel (SCDC) is a mechanism introduced by the
 * HDMI 2.0 specification. It is a point-to-point protocol that allows the
 * HDMI source and HDMI sink to exchange data. The same I2C interface that
 * is used to access EDID serves as the transport mechanism for SCDC.
 *
 * Note: The SCDC status is going to be lost when the display is
 * disconnected. This can happen physically when the user disconnects
 * the cable, but also when a display is switched on (such as waking up
 * a TV).
 *
 * This is further complicated by the fact that, upon a disconnection /
 * reconnection, KMS won't change the mode on its own. This means that
 * one can't just rely on setting the SCDC status on enable, but also
 * has to track the connector status changes using interrupts and
 * restore the SCDC status. The typical solution for this is to trigger an
 * empty modeset in drm_connector_helper_funcs.detect_ctx(), like what vc4 does
 * in vc4_hdmi_reset_link().
 */

#define SCDC_I2C_SLAVE_ADDRESS 0x54

struct scdc_debugfs_priv {
	struct drm_connector *connector;
	struct drm_scdc_state state;
};

static const char *drm_scdc_frl_rate_str(enum drm_scdc_frl_rate rate)
{
	switch (rate) {
	case SCDC_FRL_RATE_OFF:
		return "Off";
	case SCDC_FRL_RATE_3X3:
		return "3 Gbit/s x 3 lanes";
	case SCDC_FRL_RATE_6X3:
		return "6 Gbit/s x 3 lanes";
	case SCDC_FRL_RATE_6X4:
		return "6 Gbit/s x 4 lanes";
	case SCDC_FRL_RATE_8X4:
		return "8 Gbit/s x 4 lanes";
	case SCDC_FRL_RATE_10X4:
		return "10 Gbit/s x 4 lanes";
	case SCDC_FRL_RATE_12X4:
		return "12 Gbit/s x 4 lanes";
	case SCDC_FRL_RATE_RESV_7:
	case SCDC_FRL_RATE_RESV_8:
	case SCDC_FRL_RATE_RESV_9:
	case SCDC_FRL_RATE_RESV_10:
	case SCDC_FRL_RATE_RESV_11:
	case SCDC_FRL_RATE_RESV_12:
	case SCDC_FRL_RATE_RESV_13:
	case SCDC_FRL_RATE_RESV_14:
	case SCDC_FRL_RATE_RESV_15:
		return "(Reserved)";
	default:
		return NULL;
	}
}

/**
 * drm_scdc_read - read a block of data from SCDC
 * @adapter: I2C controller
 * @offset: start offset of block to read
 * @buffer: return location for the block to read
 * @size: size of the block to read
 *
 * Reads a block of data from SCDC, starting at a given offset.
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int drm_scdc_read(struct i2c_adapter *adapter, u8 offset, void *buffer,
		  size_t size)
{
	int ret;
	struct i2c_msg msgs[2] = {
		{
			.addr = SCDC_I2C_SLAVE_ADDRESS,
			.flags = 0,
			.len = 1,
			.buf = &offset,
		}, {
			.addr = SCDC_I2C_SLAVE_ADDRESS,
			.flags = I2C_M_RD,
			.len = size,
			.buf = buffer,
		}
	};

	ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EPROTO;

	return 0;
}
EXPORT_SYMBOL(drm_scdc_read);

/**
 * drm_scdc_write - write a block of data to SCDC
 * @adapter: I2C controller
 * @offset: start offset of block to write
 * @buffer: block of data to write
 * @size: size of the block to write
 *
 * Writes a block of data to SCDC, starting at a given offset.
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int drm_scdc_write(struct i2c_adapter *adapter, u8 offset, const void *buffer,
		   size_t size)
{
	struct i2c_msg msg = {
		.addr = SCDC_I2C_SLAVE_ADDRESS,
		.flags = 0,
		.len = 1 + size,
		.buf = NULL,
	};
	void *data;
	int err;

	data = kmalloc(1 + size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	msg.buf = data;

	memcpy(data, &offset, sizeof(offset));
	memcpy(data + 1, buffer, size);

	err = i2c_transfer(adapter, &msg, 1);

	kfree(data);

	if (err < 0)
		return err;
	if (err != 1)
		return -EPROTO;

	return 0;
}
EXPORT_SYMBOL(drm_scdc_write);

/**
 * drm_scdc_get_scrambling_status - what is status of scrambling?
 * @connector: connector
 *
 * Reads the scrambler status over SCDC, and checks the
 * scrambling status.
 *
 * Returns:
 * True if the scrambling is enabled, false otherwise.
 */
bool drm_scdc_get_scrambling_status(struct drm_connector *connector)
{
	u8 status;
	int ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_SCRAMBLER_STATUS, &status);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to read scrambling status: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	return status & SCDC_SCRAMBLING_STATUS;
}
EXPORT_SYMBOL(drm_scdc_get_scrambling_status);

/**
 * drm_scdc_set_scrambling - enable scrambling
 * @connector: connector
 * @enable: bool to indicate if scrambling is to be enabled/disabled
 *
 * Writes the TMDS config register over SCDC channel, and:
 * enables scrambling when enable = 1
 * disables scrambling when enable = 0
 *
 * Returns:
 * True if scrambling is set/reset successfully, false otherwise.
 */
bool drm_scdc_set_scrambling(struct drm_connector *connector,
			     bool enable)
{
	u8 config;
	int ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_TMDS_CONFIG, &config);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to read TMDS config: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	if (enable)
		config |= SCDC_SCRAMBLING_ENABLE;
	else
		config &= ~SCDC_SCRAMBLING_ENABLE;

	ret = drm_scdc_writeb(connector->ddc, SCDC_TMDS_CONFIG, config);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to enable scrambling: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	return true;
}
EXPORT_SYMBOL(drm_scdc_set_scrambling);

/**
 * drm_scdc_set_high_tmds_clock_ratio - set TMDS clock ratio
 * @connector: connector
 * @set: ret or reset the high clock ratio
 *
 *
 *	TMDS clock ratio calculations go like this:
 *		TMDS character = 10 bit TMDS encoded value
 *
 *		TMDS character rate = The rate at which TMDS characters are
 *		transmitted (Mcsc)
 *
 *		TMDS bit rate = 10x TMDS character rate
 *
 *	As per the spec:
 *		TMDS clock rate for pixel clock < 340 MHz = 1x the character
 *		rate = 1/10 pixel clock rate
 *
 *		TMDS clock rate for pixel clock > 340 MHz = 0.25x the character
 *		rate = 1/40 pixel clock rate
 *
 *	Writes to the TMDS config register over SCDC channel, and:
 *		sets TMDS clock ratio to 1/40 when set = 1
 *
 *		sets TMDS clock ratio to 1/10 when set = 0
 *
 * Returns:
 * True if write is successful, false otherwise.
 */
bool drm_scdc_set_high_tmds_clock_ratio(struct drm_connector *connector,
					bool set)
{
	u8 config;
	int ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_TMDS_CONFIG, &config);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to read TMDS config: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	if (set)
		config |= SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;
	else
		config &= ~SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

	ret = drm_scdc_writeb(connector->ddc, SCDC_TMDS_CONFIG, config);
	if (ret < 0) {
		drm_dbg_kms(connector->dev,
			    "[CONNECTOR:%d:%s] Failed to set TMDS clock ratio: %d\n",
			    connector->base.id, connector->name, ret);
		return false;
	}

	/*
	 * The spec says that a source should wait minimum 1ms and maximum
	 * 100ms after writing the TMDS config for clock ratio. Lets allow a
	 * wait of up to 2ms here.
	 */
	usleep_range(1000, 2000);
	return true;
}
EXPORT_SYMBOL(drm_scdc_set_high_tmds_clock_ratio);

/**
 * drm_scdc_read_status0_flags - Read SCDC "Status Flags" Register
 * @connector: pointer to &struct drm_connector to issue the scdc request on
 * @flags: pointer to the caller's &struct drm_scdc_status_flags to output to
 *
 * Reads the SCDC Status Flags 0 register, and outputs its contents to the
 * destination @flags. Contents of @flags are only valid if function returns 0.
 *
 * Returns: %0 on success, negative errno on error.
 */
int drm_scdc_read_status0_flags(struct drm_connector *connector,
				struct drm_scdc_status_flags *flags)
{
	int ret;
	u8 val;

	ret = drm_scdc_writeb(connector->ddc, SCDC_UPDATE_0, SCDC_STATUS_UPDATE);
	if (ret)
		return ret;

	ret = drm_scdc_readb(connector->ddc, SCDC_STATUS_FLAGS_0, &val);
	if (ret)
		return ret;

	flags->clock_detected = val & SCDC_CLOCK_DETECT;
	flags->ch0_locked = val & SCDC_CH0_LOCK;
	flags->ch1_locked = val & SCDC_CH1_LOCK;
	flags->ch2_locked = val & SCDC_CH2_LOCK;
	flags->ln3_locked = val & SCDC_LN3_LOCK;
	flags->flt_ready = val & SCDC_FLT_READY;
	flags->dsc_fail = val & SCDC_DSC_FAIL;

	return 0;
}
EXPORT_SYMBOL(drm_scdc_read_status0_flags);

/**
 * drm_scdc_read_status1_2_flags - Read SCDC "Status Flags" 1 and 2 Registers
 * @connector: pointer to &struct drm_connector to issue the scdc request on
 * @flags: pointer to the caller's &struct drm_scdc_status_flags to output to
 *
 * Reads the SCDC Status Flags 1 and 2 registers, and outputs their contents to
 * the destination @flags. Contents of @flags are only valid if function returns
 * 0.
 *
 * Returns 0 on success, negative errno on error.
 */
int drm_scdc_read_status1_2_flags(struct drm_connector *connector,
				  struct drm_scdc_status_flags *flags)
{
	u8 val[2] = {};
	int ret;

	ret = drm_scdc_writeb(connector->ddc, SCDC_UPDATE_0, SCDC_FLT_UPDATE);
	if (ret)
		return ret;

	ret = drm_scdc_read(connector->ddc, SCDC_STATUS_FLAGS_1, val, 2);
	if (ret)
		return ret;

	flags->ln0_training_pattern = FIELD_GET(SCDC_LN_EVEN_TRAIN_PTRN, val[0]);
	flags->ln1_training_pattern = FIELD_GET(SCDC_LN_ODD_TRAIN_PTRN, val[0]);

	flags->ln2_training_pattern = FIELD_GET(SCDC_LN_EVEN_TRAIN_PTRN, val[1]);
	flags->ln3_training_pattern = FIELD_GET(SCDC_LN_ODD_TRAIN_PTRN, val[1]);

	return 0;
}
EXPORT_SYMBOL(drm_scdc_read_status1_2_flags);

#define ERR_DET_OFF(x) ((x) - SCDC_ERR_DET_0_L)

static int scdc_err_cnt_buf_idx(unsigned int lane)
{
	switch (lane) {
	case 0:
		return ERR_DET_OFF(SCDC_ERR_DET_0_L);
	case 1:
		return ERR_DET_OFF(SCDC_ERR_DET_1_L);
	case 2:
		return ERR_DET_OFF(SCDC_ERR_DET_2_L);
	case 3:
		return ERR_DET_OFF(SCDC_ERR_DET_3_L);
	default:
		return -EINVAL;
	}
}

/**
 * drm_scdc_read_error_counters - Read and clear SCDC error counters
 * @connector: pointer to &struct drm_connector to issue the scdc request on
 * @counter: Caller's u16 array with 4 elements to write the counter values into
 * @num_lanes: number of active lanes, either 3 or 4
 *
 * Read the SCDC channel error counters. If the count of channel *n* is valid,
 * write it into counter[n]. Otherwise, set counter[n] to 0. Reads all counters
 * in one read chunk, then clears every counter, as is mandated.
 *
 * Returns: %0 on success, negative errno on error.
 */
int drm_scdc_read_error_counters(struct drm_connector *connector, u16 counter[4],
				 unsigned int num_lanes)
{
	unsigned int buf_sz;
	u8 buf[9] = {};
	int ret;
	u8 sum = 0;
	int i, idx;

	switch (num_lanes) {
	case 3:
		buf_sz = 7;
		break;
	case 4:
		buf_sz = 9;
		break;
	default:
		return -EINVAL;
	}

	ret = drm_scdc_writeb(connector->ddc, SCDC_UPDATE_0, SCDC_CED_UPDATE);
	if (ret)
		return ret;

	ret = drm_scdc_read(connector->ddc, SCDC_ERR_DET_0_L, buf, buf_sz);
	if (ret)
		return ret;

	/*
	 * Verify the "checksum", i.e. sum up everything including the checksum
	 * register as a wrapping unsigned 8-bit addition and verify it's 0.
	 */
	for (i = 0; i < buf_sz; i++)
		sum = wrapping_add(u8, sum, buf[i]);

	if (sum)
		return -EPROTO;

	for (i = 0; i < num_lanes; i++) {
		idx = scdc_err_cnt_buf_idx(i);
		if (buf[idx + 1] & SCDC_CHANNEL_VALID)
			counter[i] = buf[idx] | (buf[idx + 1] & ~SCDC_CHANNEL_VALID) << 8;
		else
			counter[i] = 0;
	}

	memset(buf, 0, buf_sz);

	return drm_scdc_write(connector->ddc, SCDC_ERR_DET_0_L, buf, buf_sz);
}
EXPORT_SYMBOL(drm_scdc_read_error_counters);

/**
 * drm_scdc_read_state - Update state from SCDC
 * @connector: pointer to a &struct drm_connector on which to operate on
 * @state: pointer to a &struct drm_scdc_state to fill
 *
 * Reads update flags from SCDC, and updates the parts of @state that SCDC
 * claims have changed, as well as populating those where such a distinction
 * can't be made.
 *
 * Returns: %0 on success, negative errno on failure.
 */
int drm_scdc_read_state(struct drm_connector *connector, struct drm_scdc_state *state)
{
	u8 upd_flags[2] = {};
	struct i2c_adapter *ddc;
	struct drm_scdc *scdc;
	int num_lanes;
	int ret;
	u8 val;

	if (!state || !connector)
		return -ENODEV;

	scdc = &connector->display_info.hdmi.scdc;
	ddc = connector->ddc;

	if (!scdc->supported)
		return -EOPNOTSUPP;

	ret = drm_scdc_readb(ddc, SCDC_TMDS_CONFIG, &val);
	if (ret)
		return ret;

	state->scrambling_enabled = val & SCDC_SCRAMBLING_ENABLE;
	state->tmds_bclk_x40 = val & SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

	state->scrambling_detected = drm_scdc_get_scrambling_status(connector);

	ret = drm_scdc_readb(ddc, SCDC_CONFIG_1, &val);
	if (ret)
		return ret;

	state->rate = FIELD_GET(SCDC_FRL_RATE, val);
	num_lanes = drm_scdc_num_frl_lanes(state->rate);
	if (num_lanes < 0)
		return num_lanes;
	if (!num_lanes)
		num_lanes = 3;

	state->ffe_levels = FIELD_GET(SCDC_FFE_LEVELS, val);

	ret = drm_scdc_read(ddc, SCDC_UPDATE_0, &upd_flags, sizeof(upd_flags));
	if (ret)
		return ret;

	if (upd_flags[0] & SCDC_STATUS_UPDATE) {
		ret = drm_scdc_read_status0_flags(connector, &state->stf);
		if (ret)
			return ret;
	}

	if (upd_flags[0] & SCDC_FLT_UPDATE) {
		ret = drm_scdc_read_status1_2_flags(connector, &state->stf);
		if (ret)
			return ret;
	}

	if (upd_flags[0] & SCDC_CED_UPDATE) {
		ret = drm_scdc_read_error_counters(connector, state->error_count,
						   num_lanes);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_scdc_read_state);

#define scdc_print_str(_f, key, s) \
	(seq_printf((_f), "%-30s: %s\n", (key), (s)))
#define scdc_print_flag(_f, key, val) \
	(scdc_print_str((_f), (key), str_yes_no((val))))
#define scdc_print_dec(_f, key, val) \
	(seq_printf((_f), "%-30s: %d\n", (key), (val)))

static int scdc_status_show(struct seq_file *m, void *data)
{
	struct scdc_debugfs_priv *priv = m->private;
	struct drm_scdc_state *st = &priv->state;
	struct drm_connector *connector = priv->connector;
	struct drm_scdc *scdc = &connector->display_info.hdmi.scdc;
	int ret;

	drm_connector_get(connector);

	if (connector->status != connector_status_connected) {
		ret = -ENODEV;
		goto err_conn_put;
	}

	scdc_print_flag(m, "SCDC Supported", scdc->supported);
	if (!scdc->supported) {
		ret = 0;
		goto err_conn_put;
	}

	scdc_print_flag(m, "Sink Read Request Capable", scdc->read_request);
	scdc_print_flag(m, "Scrambling Supported", scdc->scrambling.supported);
	scdc_print_flag(m, "Low Rate Scrambling Supported", scdc->scrambling.low_rates);

	ret = drm_scdc_read_state(connector, st);
	drm_connector_put(connector);
	if (ret)
		return ret;

	scdc_print_flag(m, "Scrambling Enabled", st->scrambling_enabled);
	scdc_print_flag(m, "Scrambling Detected", st->scrambling_detected);
	scdc_print_str(m, "FRL Rate", drm_scdc_frl_rate_str(st->rate));
	scdc_print_dec(m, "FFE Levels", st->ffe_levels);

	if (st->tmds_bclk_x40)
		scdc_print_str(m, "TMDS Bit Clock Ratio", "1/40");
	else
		scdc_print_str(m, "TMDS Bit Clock Ratio", "1/10");

	scdc_print_flag(m, "Clock Detected", st->stf.clock_detected);
	scdc_print_flag(m, "Channel 0 Locked", st->stf.ch0_locked);
	scdc_print_flag(m, "Channel 1 Locked", st->stf.ch1_locked);
	scdc_print_flag(m, "Channel 2 Locked", st->stf.ch2_locked);
	if (drm_scdc_num_frl_lanes(st->rate) == 4)
		scdc_print_flag(m, "Lane 3 Locked", st->stf.ln3_locked);

	scdc_print_flag(m, "Sink Ready For Link Training", st->stf.flt_ready);
	scdc_print_flag(m, "Sink Failed To Decode DSC", st->stf.dsc_fail);

	scdc_print_dec(m, "Channel 0 Errors", st->error_count[0]);
	scdc_print_dec(m, "Channel 1 Errors", st->error_count[1]);
	scdc_print_dec(m, "Channel 2 Errors", st->error_count[2]);
	if (drm_scdc_num_frl_lanes(st->rate) == 4)
		scdc_print_dec(m, "Lane 3 Errors", st->error_count[3]);

	return 0;

err_conn_put:
	drm_connector_put(connector);

	return ret;
}
DEFINE_SHOW_ATTRIBUTE(scdc_status);

/**
 * drm_scdc_debugfs_init - Initialize scdc files in connector debugfs
 * @connector: pointer to &struct drm_connector to operate on
 * @root: debugfs &struct dentry for the debugfs root of @connector
 *
 * Creates SCDC-related debugfs files for @connector. Must be called after
 * @root is already created.
 */
void drm_scdc_debugfs_init(struct drm_connector *connector, struct dentry *root)
{
	struct scdc_debugfs_priv *priv;

	if (!root || !connector)
		return;

	priv = drmm_kzalloc(connector->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return;

	priv->connector = connector;

	debugfs_create_file("scdc_status", 0444, root, priv, &scdc_status_fops);
}
EXPORT_SYMBOL(drm_scdc_debugfs_init);
