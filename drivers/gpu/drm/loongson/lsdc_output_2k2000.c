// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include "lsdc_drv.h"
#include "lsdc_output.h"

/*
 * The DC in LS2K2000 is nearly same with the DC in LS7A2000, except that
 * LS2K2000 has only one built-in HDMI encoder which is connected with the
 * display pipe 0. Display pipe 1 is a DVO output interface.
 *       ________________________
 *      |                        |                        ______________
 *      |             +----------|                       |              |
 *      | CRTC-0 ---> | HDMI phy ---> HDMI Connector --> | HDMI Monitor |<--+
 *      |             +----------|                       |______________|   |
 *      |            +-------+   |                                          |
 *      |            | i2c-x |   <------------------------------------------+
 *      |            +-------+   |
 *      |                        |
 *      |    DC in LS2K2000      |
 *      |                        |
 *      |            +-------+   |
 *      |            | i2c-y |   <------------------------------------+
 *      |            +-------+   |                                    |
 *      |                        |                                ____|____
 *      |                +-------|                               |         |
 *      | CRTC-1 ------> |  DVO  --> Encoder1 --> Connector1 --> | Display |
 *      |                +-------|                               |_________|
 *      |________________________|
 */

static void ls2k2000_pipe1_dvo_encoder_reset(struct drm_encoder *encoder)
{
	struct drm_device *ddev = encoder->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	u32 val;

	val = PHY_CLOCK_POL | PHY_CLOCK_EN | PHY_DATA_EN;
	lsdc_wreg32(ldev, LSDC_CRTC1_DVO_CONF_REG, val);
}

const struct drm_encoder_funcs ls2k2000_pipe1_dvo_encoder_funcs = {
	.reset = ls2k2000_pipe1_dvo_encoder_reset,
	.destroy = drm_encoder_cleanup,
};

static const struct lsdc_output_desc ls2k2000_output_desc[2] = {
	{
		.pipe = 0,
		.encoder_type = DRM_MODE_ENCODER_TMDS,
		.connector_type = DRM_MODE_CONNECTOR_HDMIA,
		.encoder_funcs = &lsdc_pipe0_hdmi_encoder_funcs,
		.encoder_helper_funcs = &lsdc_pipe0_hdmi_encoder_helper_funcs,
		.connector_funcs = &lsdc_pipe0_hdmi_connector_funcs,
		.connector_helper_funcs = &lsdc_connector_helper_funcs,
		.name = "HDMI-0",
	},
	{
		.pipe = 1,
		.encoder_type = DRM_MODE_ENCODER_DPI,
		.connector_type = DRM_MODE_CONNECTOR_DPI,
		.encoder_funcs = &ls2k2000_pipe1_dvo_encoder_funcs,
		.encoder_helper_funcs = &lsdc_encoder_helper_funcs,
		.connector_funcs = &lsdc_connector_funcs,
		.connector_helper_funcs = &lsdc_connector_helper_funcs,
		.name = "DVO-1",
	},
};

int ls2k2000_output_init(struct drm_device *ddev,
			 struct lsdc_display_pipe *dispipe,
			 struct i2c_adapter *ddc,
			 unsigned int pipe)
{
	struct lsdc_output *output = &dispipe->output;

	output->descp = &ls2k2000_output_desc[pipe];

	lsdc_output_init(ddev, dispipe, ddc, pipe);

	return 0;
}
