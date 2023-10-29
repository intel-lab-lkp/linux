// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include <linux/delay.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

#include "lsdc_drv.h"
#include "lsdc_output.h"

/*
 * The display controller in LS7A2000 has two display pipes
 * Display pipe 0 is attached with a built-in transparent VGA encoder and
 * a built-in HDMI encoder.
 * Display pipe 1 has only one built-in HDMI encoder connected.
 *       ______________________                          _____________
 *      |             +-----+  |                        |             |
 *      | CRTC0 -+--> | VGA |  ----> VGA Connector ---> | VGA Monitor |<---+
 *      |        |    +-----+  |                        |_____________|    |
 *      |        |             |                         ______________    |
 *      |        |    +------+ |                        |              |   |
 *      |        +--> | HDMI | ----> HDMI Connector --> | HDMI Monitor |<--+
 *      |             +------+ |                        |______________|   |
 *      |            +------+  |                                           |
 *      |            | i2c6 |  <-------------------------------------------+
 *      |            +------+  |
 *      |                      |
 *      |    DC in LS7A2000    |
 *      |                      |
 *      |            +------+  |
 *      |            | i2c7 |  <--------------------------------+
 *      |            +------+  |                                |
 *      |                      |                          ______|_______
 *      |            +------+  |                         |              |
 *      | CRTC1 ---> | HDMI |  ----> HDMI Connector ---> | HDMI Monitor |
 *      |            +------+  |                         |______________|
 *      |______________________|
 */

/* The built-in tranparent VGA encoder is only available on display pipe 0 */
static void ls7a2000_pipe0_vga_encoder_reset(struct drm_encoder *encoder)
{
	struct lsdc_device *ldev = to_lsdc(encoder->dev);
	u32 val = PHY_CLOCK_POL | PHY_CLOCK_EN | PHY_DATA_EN;

	lsdc_wreg32(ldev, LSDC_CRTC0_DVO_CONF_REG, val);

	/*
	 * The firmware set LSDC_HDMIx_CTRL_REG blindly to use hardware I2C,
	 * which is may not works because of hardware bug. We using built-in
	 * GPIO emulated I2C instead of the hardware I2C here.
	 */
	lsdc_ureg32_clr(ldev, LSDC_HDMI0_INTF_CTRL_REG, HW_I2C_EN);

	mdelay(20);
}

static const struct drm_encoder_funcs ls7a2000_pipe0_vga_encoder_funcs = {
	.reset = ls7a2000_pipe0_vga_encoder_reset,
	.destroy = drm_encoder_cleanup,
};

static const struct lsdc_output_desc ls7a2000_vga_pipe0 = {
	.pipe = 0,
	.encoder_type = DRM_MODE_ENCODER_DAC,
	.connector_type = DRM_MODE_CONNECTOR_VGA,
	.encoder_funcs = &ls7a2000_pipe0_vga_encoder_funcs,
	.encoder_helper_funcs = &lsdc_pipe0_hdmi_encoder_helper_funcs,
	.connector_funcs = &lsdc_connector_funcs,
	.connector_helper_funcs = &lsdc_connector_helper_funcs,
	.name = "VGA-0",
};

static const struct lsdc_output_desc ls7a2000_hdmi_pipe0 = {
	.pipe = 0,
	.encoder_type = DRM_MODE_ENCODER_TMDS,
	.connector_type = DRM_MODE_CONNECTOR_HDMIA,
	.encoder_funcs = &lsdc_pipe0_hdmi_encoder_funcs,
	.encoder_helper_funcs = &lsdc_pipe0_hdmi_encoder_helper_funcs,
	.connector_funcs = &lsdc_pipe0_hdmi_connector_funcs,
	.connector_helper_funcs = &lsdc_connector_helper_funcs,
	.name = "HDMI-0",
};

static const struct lsdc_output_desc ls7a2000_hdmi_pipe1 = {
	.pipe = 1,
	.encoder_type = DRM_MODE_ENCODER_TMDS,
	.connector_type = DRM_MODE_CONNECTOR_HDMIA,
	.encoder_funcs = &lsdc_pipe1_hdmi_encoder_funcs,
	.encoder_helper_funcs = &lsdc_pipe1_hdmi_encoder_helper_funcs,
	.connector_funcs = &lsdc_pipe1_hdmi_connector_funcs,
	.connector_helper_funcs = &lsdc_connector_helper_funcs,
	.name = "HDMI-1",
};

/*
 * For LS7A2000, the built-in VGA encoder is transparent. If there are
 * external encoder exist, then the internal HDMI encoder MUST be enabled
 * and initialized. As the internal HDMI encoder is always connected, so
 * only the transmitters which take HDMI signal (such as HDMI to eDP, HDMI
 * to LVDS, etc) are usable with.
 */
const struct lsdc_output_desc *
ls7a2000_query_output_configuration(struct drm_device *ddev, unsigned int pipe)
{
	enum loongson_vbios_encoder_name encoder_name = 0;
	bool ret;

	ret = loongson_vbios_query_encoder_info(ddev, pipe, NULL,
						&encoder_name, NULL);
	if (!ret)
		goto bailout;

	if (pipe == 0) {
		switch (encoder_name) {
		case ENCODER_CHIP_INTERNAL_HDMI:
			return &ls7a2000_hdmi_pipe0;

		/*
		 * For LS7A2000, the built-in VGA encoder is transparent.
		 */
		case ENCODER_CHIP_INTERNAL_VGA:
			return &ls7a2000_vga_pipe0;

		/*
		 * External display bridge exists, the internal HDMI encoder
		 * MUST be enabled and initialized. Please add a drm bridge
		 * driver, and attach to this encoder.
		 */
		default:
			return &ls7a2000_hdmi_pipe0;
		}
	}

	if (pipe == 1) {
		switch (encoder_name) {
		case ENCODER_CHIP_INTERNAL_HDMI:
			return &ls7a2000_hdmi_pipe1;

		/*
		 * External display bridge exists, the internal HDMI encoder
		 * MUST be enabled and initialized. Please add a drm bridge
		 * driver, and attach it to this encoder.
		 */
		default:
			return &ls7a2000_hdmi_pipe1;
		}
	}

bailout:
	if (pipe == 0)
		return &ls7a2000_vga_pipe0;

	if (pipe == 1)
		return &ls7a2000_hdmi_pipe1;

	return NULL;
}

int ls7a2000_output_init(struct drm_device *ddev,
			 struct lsdc_display_pipe *dispipe,
			 struct i2c_adapter *ddc,
			 unsigned int pipe)
{
	struct lsdc_output *output = &dispipe->output;

	output->descp = ls7a2000_query_output_configuration(ddev, pipe);
	if (!output->descp)
		return -EINVAL;

	return lsdc_output_init(ddev, dispipe, ddc, pipe);
}
