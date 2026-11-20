// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT36525 MIPI DSI panel driver for the Samsung Galaxy XCover5
 *
 * Panel timing and the DCS initialisation sequence are taken from the
 * Samsung GPL kernel release for the SM-G525F
 * (drivers/video/fbdev/exynos/panel/nt36525/nt36525_xcover5_panel.h).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct nt36525 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *vdd;		/* vdd_ldo28, 1.8 V */
	struct regulator *vpos;		/* LCD positive bias */
	struct regulator *vneg;		/* LCD negative bias */
	struct gpio_desc *reset_gpio;	/* panel RESX, active-low; gpiod value 1 = asserted */
};

static inline struct nt36525 *panel_to_nt36525(struct drm_panel *p)
{
	return container_of(p, struct nt36525, panel);
}

/* XCover5 panel timing, exact from Samsung DTS node. 720x1480@60. */
static const struct drm_display_mode nt36525_xcover5_mode = {
	.clock		= (720 + 128 + 20 + 130) * (1480 + 18 + 2 + 232) * 60 / 1000,
	.hdisplay	= 720,
	.hsync_start	= 720 + 128,			/* HFP */
	.hsync_end	= 720 + 128 + 20,		/* HSA */
	.htotal		= 720 + 128 + 20 + 130,		/* +HBP */
	.vdisplay	= 1480,
	.vsync_start	= 1480 + 18,			/* VFP */
	.vsync_end	= 1480 + 18 + 2,		/* VSA */
	.vtotal		= 1480 + 18 + 2 + 232,		/* +VBP */
	.width_mm	= 59,				/* from panel size node */
	.height_mm	= 121,
	.type		= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int nt36525_on(struct nt36525 *p)
{
	struct mipi_dsi_multi_context ctx = { .dsi = p->dsi };

	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x20);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x00, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x01, 0x55);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x03, 0x55);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x05, 0xa9);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x07, 0x73);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x08, 0xc1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0e, 0x91);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0f, 0x5f);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x69, 0xa9);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x94, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x95, 0xeb);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x96, 0xeb);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x24);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x04, 0x21);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x06, 0x22);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x07, 0x20);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x08, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0a, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0b, 0x0d);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0c, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0d, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0f, 0x04);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x10, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x12, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x13, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x14, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x16, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1a, 0x21);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1c, 0x22);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1d, 0x20);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1e, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x20, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x21, 0x0d);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x22, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x23, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x25, 0x04);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x26, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x28, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x29, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x2a, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x2f, 0x09);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x30, 0x07);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x33, 0x07);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x34, 0x09);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x37, 0x33);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x39, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x3a, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x3b, 0x94);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x3d, 0x92);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x4d, 0x12);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x4e, 0x34);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x51, 0x43);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x52, 0x21);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x55, 0x86);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x56, 0x34);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5a, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5b, 0x94);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5c, 0xc0);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5d, 0x06);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5e, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x60, 0x80);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x61, 0x72);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x64, 0x11);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x82, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x83, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x85, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x86, 0x51);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x92, 0xba);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x93, 0x12);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x94, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xab, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xac, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xad, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xaf, 0x04);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb0, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb1, 0xb5);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x25);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0a, 0x82);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0b, 0x50);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x0c, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x17, 0x82);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x18, 0x06);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x19, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x59, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5a, 0x40);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5b, 0x80);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5c, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5d, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5e, 0x94);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x5f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x60, 0x94);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x61, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x62, 0x94);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x65, 0x05);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x66, 0xb5);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc0, 0x03);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xca, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xcc, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd3, 0x11);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd4, 0xc8);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd5, 0x11);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd6, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xd7, 0x11);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x26);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x00, 0xa0);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x27);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x13, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x15, 0xb4);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1f, 0x55);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x26, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc0, 0x18);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc1, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc3, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc4, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc5, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc6, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc7, 0x03);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x23);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x07, 0x20);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x08, 0x10);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x12, 0xa6);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x15, 0xfc);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x16, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x04, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x19, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1a, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1b, 0x08);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1c, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1d, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1e, 0x12);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x1f, 0x16);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x20, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x21, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x22, 0x20);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x23, 0x24);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x24, 0x28);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x25, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x26, 0x30);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x27, 0x38);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x28, 0x3c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x29, 0x10);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x30, 0xff);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x31, 0xff);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x32, 0xfe);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x33, 0xfd);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x34, 0xfd);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x35, 0xfc);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x36, 0xfb);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x37, 0xf9);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x38, 0xf7);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x39, 0xf3);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x3a, 0xea);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x3b, 0xe6);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x3d, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x3f, 0xdd);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x40, 0xdb);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x41, 0xd9);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x20);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb0, 0x00, 0x08, 0x00, 0x18, 0x00, 0x34, 0x00, 0x4c,
				     0x00, 0x61, 0x00, 0x74, 0x00, 0x86, 0x00, 0x96);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb1, 0x00, 0xa5, 0x00, 0xdb, 0x01, 0x02, 0x01, 0x43,
				     0x01, 0x71, 0x01, 0xbd, 0x01, 0xf7, 0x01, 0xfa);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb2, 0x02, 0x34, 0x02, 0x6f, 0x02, 0x98, 0x02, 0xcc,
				     0x02, 0xf1, 0x03, 0x1d, 0x03, 0x2d, 0x03, 0x3b);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb3, 0x03, 0x4d, 0x03, 0x60, 0x03, 0x7a, 0x03, 0x94,
				     0x03, 0xa6, 0x03, 0xa7);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb4, 0x00, 0x08, 0x00, 0x18, 0x00, 0x34, 0x00, 0x4c,
				     0x00, 0x61, 0x00, 0x74, 0x00, 0x86, 0x00, 0x96);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb5, 0x00, 0xa5, 0x00, 0xdb, 0x01, 0x02, 0x01, 0x43,
				     0x01, 0x71, 0x01, 0xbd, 0x01, 0xf7, 0x01, 0xfa);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb6, 0x02, 0x34, 0x02, 0x6f, 0x02, 0x98, 0x02, 0xcc,
				     0x02, 0xf1, 0x03, 0x1d, 0x03, 0x2d, 0x03, 0x3b);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb7, 0x03, 0x4d, 0x03, 0x60, 0x03, 0x7a, 0x03, 0x94,
				     0x03, 0xa6, 0x03, 0xa7);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb8, 0x00, 0x08, 0x00, 0x18, 0x00, 0x34, 0x00, 0x4c,
				     0x00, 0x61, 0x00, 0x74, 0x00, 0x86, 0x00, 0x96);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb9, 0x00, 0xa5, 0x00, 0xdb, 0x01, 0x02, 0x01, 0x43,
				     0x01, 0x71, 0x01, 0xbd, 0x01, 0xf7, 0x01, 0xfa);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xba, 0x02, 0x34, 0x02, 0x6f, 0x02, 0x98, 0x02, 0xcc,
				     0x02, 0xf1, 0x03, 0x1d, 0x03, 0x2d, 0x03, 0x3b);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xbb, 0x03, 0x4d, 0x03, 0x60, 0x03, 0x7a, 0x03, 0x94,
				     0x03, 0xa6, 0x03, 0xa7);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x21);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb0, 0x00, 0x00, 0x00, 0x10, 0x00, 0x2c, 0x00, 0x44,
				     0x00, 0x59, 0x00, 0x6c, 0x00, 0x7e, 0x00, 0x8e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb1, 0x00, 0x9d, 0x00, 0xd3, 0x00, 0xfa, 0x01, 0x3b,
				     0x01, 0x69, 0x01, 0xb5, 0x01, 0xef, 0x01, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb2, 0x02, 0x2c, 0x02, 0x79, 0x02, 0xaa, 0x02, 0xe6,
				     0x03, 0x0f, 0x03, 0x3f, 0x03, 0x4f, 0x03, 0x5f);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb3, 0x03, 0x71, 0x03, 0x86, 0x03, 0xa0, 0x03, 0xbc,
				     0x03, 0xce, 0x03, 0xd9);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb4, 0x00, 0x00, 0x00, 0x10, 0x00, 0x2c, 0x00, 0x44,
				     0x00, 0x59, 0x00, 0x6c, 0x00, 0x7e, 0x00, 0x8e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb5, 0x00, 0x9d, 0x00, 0xd3, 0x00, 0xfa, 0x01, 0x3b,
				     0x01, 0x69, 0x01, 0xb5, 0x01, 0xef, 0x01, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb6, 0x02, 0x2c, 0x02, 0x79, 0x02, 0xaa, 0x02, 0xe6,
				     0x03, 0x0f, 0x03, 0x3f, 0x03, 0x4f, 0x03, 0x5f);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb7, 0x03, 0x71, 0x03, 0x86, 0x03, 0xa0, 0x03, 0xbc,
				     0x03, 0xce, 0x03, 0xd9);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb8, 0x00, 0x00, 0x00, 0x10, 0x00, 0x2c, 0x00, 0x44,
				     0x00, 0x59, 0x00, 0x6c, 0x00, 0x7e, 0x00, 0x8e);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb9, 0x00, 0x9d, 0x00, 0xd3, 0x00, 0xfa, 0x01, 0x3b,
				     0x01, 0x69, 0x01, 0xb5, 0x01, 0xef, 0x01, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xba, 0x02, 0x2c, 0x02, 0x79, 0x02, 0xaa, 0x02, 0xe6,
				     0x03, 0x0f, 0x03, 0x3f, 0x03, 0x4f, 0x03, 0x5f);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xbb, 0x03, 0x71, 0x03, 0x86, 0x03, 0xa0, 0x03, 0xbc,
				     0x03, 0xce, 0x03, 0xd9);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0xd0);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x25, 0xa9);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xff, 0x10);
	mipi_dsi_msleep(&ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xba, 0x02);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x55, 0x01);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x53, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x51, 0x0f, 0xff);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0x68, 0x00, 0x01);
	mipi_dsi_dcs_exit_sleep_mode_multi(&ctx);
	mipi_dsi_msleep(&ctx, 100);
	mipi_dsi_dcs_set_display_on_multi(&ctx);

	return ctx.accum_err;
}

static int nt36525_prepare(struct drm_panel *panel)
{
	struct nt36525 *p = panel_to_nt36525(panel);
	int ret;

	/* Power-up sequence from Samsung decon_board_nt36525 panel_power_enable */
	ret = regulator_enable(p->vdd);
	if (ret)
		return ret;
	usleep_range(1000, 1500);
	ret = regulator_enable(p->vpos);
	if (ret)
		goto disable_vdd;
	usleep_range(1000, 1500);
	ret = regulator_enable(p->vneg);
	if (ret)
		goto disable_vpos;
	usleep_range(1000, 1500);

	/*
	 * Reset pulse, Samsung sequence from panel_reset. Logical values are
	 * inverted from the physical levels the vendor sequence describes
	 * (RESX is active-low): deassert, assert, deassert.
	 */
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(p->reset_gpio, 0);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(p->reset_gpio, 1);
	usleep_range(3500, 4000);
	gpiod_set_value_cansleep(p->reset_gpio, 0);
	usleep_range(10000, 11000);

	ret = nt36525_on(p);
	if (ret)
		goto fail;

	return 0;

fail:
	gpiod_set_value_cansleep(p->reset_gpio, 1);
	regulator_disable(p->vneg);
	usleep_range(1000, 1500);
disable_vpos:
	regulator_disable(p->vpos);
	usleep_range(5000, 5500);
disable_vdd:
	regulator_disable(p->vdd);
	return ret;
}

static int nt36525_unprepare(struct drm_panel *panel)
{
	struct nt36525 *p = panel_to_nt36525(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = p->dsi };

	mipi_dsi_dcs_set_display_off_multi(&ctx);
	mipi_dsi_msleep(&ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&ctx);
	mipi_dsi_msleep(&ctx, 60);

	usleep_range(3000, 3500);
	regulator_disable(p->vneg);
	usleep_range(1000, 1500);
	regulator_disable(p->vpos);
	usleep_range(5000, 5500);
	regulator_disable(p->vdd);
	msleep(30);
	gpiod_set_value_cansleep(p->reset_gpio, 1);
	return 0;
}

static int nt36525_get_modes(struct drm_panel *panel, struct drm_connector *conn)
{
	return drm_connector_helper_get_modes_fixed(conn, &nt36525_xcover5_mode);
}

static const struct drm_panel_funcs nt36525_funcs = {
	.prepare	= nt36525_prepare,
	.unprepare	= nt36525_unprepare,
	.get_modes	= nt36525_get_modes,
};

static int nt36525_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nt36525 *p;
	int ret;

	p = devm_drm_panel_alloc(dev, struct nt36525, panel, &nt36525_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(p))
		return PTR_ERR(p);

	p->dsi = dsi;
	ret = drm_panel_of_backlight(&p->panel);
	if (ret)
		return ret;

	p->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(p->vdd))
		return dev_err_probe(dev, PTR_ERR(p->vdd), "vdd supply\n");

	p->vpos = devm_regulator_get(dev, "vpos");
	if (IS_ERR(p->vpos))
		return dev_err_probe(dev, PTR_ERR(p->vpos), "vpos supply\n");

	p->vneg = devm_regulator_get(dev, "vneg");
	if (IS_ERR(p->vneg))
		return dev_err_probe(dev, PTR_ERR(p->vneg), "vneg supply\n");

	/*
	 * Request RESX deasserted (logical 0, active-low) rather than asserting it
	 * here. At probe the TDDI may still be running the image the bootloader (or
	 * a kernel that kexec'd into this one) left on screen; asserting RESX
	 * mid-stream made the touch half of the chip raise its interrupt before any
	 * driver had mapped it. The real fix for that livelock is in pinctrl-exynos
	 * (inherited EINTs are masked at init); this only avoids resetting a live
	 * panel needlessly. prepare() still performs the full deassert-assert-
	 * deassert reset pulse, and vdd_ldo28 is always-on, so leaving RESX
	 * deasserted here never drives an unpowered chip.
	 */
	p->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(p->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(p->reset_gpio), "reset gpio\n");

	/* MIPI DSI config from Samsung panel node: 3-lane, video mode, RGB888 */
	dsi->lanes	 = 3;
	dsi->format	 = MIPI_DSI_FMT_RGB888;
	/*
	 * MIPI_DSI_MODE_LPM is load-bearing: .prepare sends the DCS init sequence
	 * before the DSIM video stream is running. Without LPM those writes go out
	 * in HS, which blocks waiting for a stream that does not exist yet, so
	 * .prepare never returns and the modeset hangs. In LP mode the init goes
	 * over the low-power escape clock, independent of the video stream.
	 */
	dsi->mode_flags	 = MIPI_DSI_MODE_VIDEO		/* video (non-burst) */
			 | MIPI_DSI_MODE_VIDEO_SYNC_PULSE
			 | MIPI_DSI_MODE_LPM		/* force LP for all transfers */
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	/*
	 * This panel's DCS init sequence runs in .prepare(), before the DSI host
	 * bridge's own atomic_pre_enable(). On bridge-based hosts (e.g.
	 * samsung-dsim) that ordering matters: without this, host_transfer()
	 * rejects the LP writes because the host bridge isn't enabled yet.
	 */
	p->panel.prepare_prev_first = true;

	drm_panel_add(&p->panel);

	mipi_dsi_set_drvdata(dsi, p);
	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&p->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void nt36525_remove(struct mipi_dsi_device *dsi)
{
	struct nt36525 *p = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&p->panel);
}

static const struct of_device_id nt36525_of_match[] = {
	{ .compatible = "novatek,nt36525-xcover5" },
	{ }
};
MODULE_DEVICE_TABLE(of, nt36525_of_match);

static struct mipi_dsi_driver nt36525_driver = {
	.probe	= nt36525_probe,
	.remove	= nt36525_remove,
	.driver = {
		.name		= "panel-novatek-nt36525",
		.of_match_table	= nt36525_of_match,
	},
};
module_mipi_dsi_driver(nt36525_driver);

MODULE_DESCRIPTION("Novatek NT36525 panel driver for Samsung Galaxy XCover5");
MODULE_AUTHOR("Adil Hüsrev Tepe <tadilhusrev@gmail.com>");
MODULE_LICENSE("GPL");
