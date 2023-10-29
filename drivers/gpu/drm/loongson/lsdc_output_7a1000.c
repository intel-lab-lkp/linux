// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

#include "lsdc_drv.h"
#include "lsdc_output.h"

#include "ite_it66121.h"
/*
 * The display controller in the LS7A1000 exports two DVO interfaces, thus
 * external encoder is required, except connected to the DPI panel directly.
 *
 *       ___________________                                     _________
 *      |            -------|                                   |         |
 *      |  CRTC0 --> | DVO0 ----> Encoder0 ---> Connector0 ---> | Display |
 *      |  _   _     -------|        ^             ^            |_________|
 *      | | | | |  +------+ |        |             |
 *      | |_| |_|  | i2c6 | <--------+-------------+
 *      |          +------+ |
 *      |                   |
 *      |  DC in LS7A1000   |
 *      |                   |
 *      |  _   _   +------+ |
 *      | | | | |  | i2c7 | <--------+-------------+
 *      | |_| |_|  +------+ |        |             |             _________
 *      |            -------|        |             |            |         |
 *      |  CRTC1 --> | DVO1 ----> Encoder1 ---> Connector1 ---> |  Panel  |
 *      |            -------|                                   |_________|
 *      |___________________|
 *
 * Currently, we assume the external encoders connected to the DVO are
 * transparent. Loongson's DVO interface can directly drive RGB888 panels.
 *
 *  TODO: Add support for non-transparent encoders
 */

static void ls7a1000_pipe0_encoder_reset(struct drm_encoder *encoder)
{
	struct drm_device *ddev = encoder->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);

	/*
	 * We need this for S3 support, screen will not lightup if don't set
	 * this register correctly.
	 */
	lsdc_wreg32(ldev, LSDC_CRTC0_DVO_CONF_REG,
		    PHY_CLOCK_POL | PHY_CLOCK_EN | PHY_DATA_EN);
}

static void ls7a1000_pipe1_encoder_reset(struct drm_encoder *encoder)
{
	struct drm_device *ddev = encoder->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);

	/*
	 * We need this for S3 support, screen will not lightup if don't set
	 * this register correctly.
	 */

	/* DVO */
	lsdc_wreg32(ldev, LSDC_CRTC1_DVO_CONF_REG,
		    BIT(31) | PHY_CLOCK_POL | PHY_CLOCK_EN | PHY_DATA_EN);
}

static const struct drm_encoder_funcs ls7a1000_encoder_funcs[2] = {
	{
		.reset = ls7a1000_pipe0_encoder_reset,
		.destroy = drm_encoder_cleanup,
	},
	{
		.reset = ls7a1000_pipe1_encoder_reset,
		.destroy = drm_encoder_cleanup,
	},
};

/*
 * This is a default output description for LS7A1000/LS2K1000, this is always
 * true from the hardware perspective. It is just that when there are external
 * display bridge connected, this description no longer complete. As it cannot
 * describe the topology about the external encoders.
 */
static const struct lsdc_output_desc ls7a1000_output_desc[2] = {
	{
		.pipe = 0,
		.encoder_type = DRM_MODE_ENCODER_DPI,
		.connector_type = DRM_MODE_CONNECTOR_DPI,
		.encoder_funcs = &ls7a1000_encoder_funcs[0],
		.encoder_helper_funcs = &lsdc_encoder_helper_funcs,
		.connector_funcs = &lsdc_connector_funcs,
		.connector_helper_funcs = &lsdc_connector_helper_funcs,
		.name = "DVO-0",
	},
	{
		.pipe = 1,
		.encoder_type = DRM_MODE_ENCODER_DPI,
		.connector_type = DRM_MODE_CONNECTOR_DPI,
		.encoder_funcs = &ls7a1000_encoder_funcs[1],
		.encoder_helper_funcs = &lsdc_encoder_helper_funcs,
		.connector_funcs = &lsdc_connector_funcs,
		.connector_helper_funcs = &lsdc_connector_helper_funcs,
		.name = "DVO-1",
	},
};

int ls7a1000_output_init(struct drm_device *ddev,
			 struct lsdc_display_pipe *dispipe,
			 struct i2c_adapter *ddc,
			 unsigned int index)
{
	struct lsdc_output *output = &dispipe->output;
	enum loongson_vbios_encoder_name encoder_name = 0;
	struct drm_bridge *bridge = NULL;
	u8 slave_addr;
	bool ret;

	output->descp = &ls7a1000_output_desc[index];

	ret = loongson_vbios_query_encoder_info(ddev, index, NULL,
						&encoder_name, &slave_addr);
	if (!ret)
		goto skip;

	switch (encoder_name) {
	case ENCODER_CHIP_IT66121:
		bridge = it66121_bridge_create(ddev, ddc, slave_addr, false,
					       0, index);
		break;
	default:
		break;
	}

	if (IS_ERR(bridge))
		goto skip;

	output->bridge = bridge;

skip:
	return lsdc_output_init(ddev, dispipe, ddc, index);
}
