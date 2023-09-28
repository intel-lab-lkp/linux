/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2022 NXP
 */

#ifndef __PHY_HDMI_H_
#define __PHY_HDMI_H_

#include <linux/hdmi.h>
/**
 * struct phy_configure_opts_hdmi - HDMI configuration set
 * @pixel_clk_rate: Pixel clock of video modes in KHz.
 * @bpc: Maximum bits per color channel.
 * @color_space: Colorspace in enum hdmi_colorspace.
 *
 * This structure is used to represent the configuration state of a HDMI phy.
 */
struct phy_configure_opts_hdmi {
	unsigned int pixel_clk_rate;
	unsigned int bpc;
	enum hdmi_colorspace color_space;
};

#endif /* __PHY_HDMI_H_ */
