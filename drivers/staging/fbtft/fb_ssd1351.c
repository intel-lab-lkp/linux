// SPDX-License-Identifier: GPL-2.0

#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/string_choices.h>

#include "fbtft.h"

#define DRVNAME		"fb_ssd1351"
#define WIDTH		128
#define HEIGHT		128
#define GAMMA_NUM	1
#define GAMMA_LEN	63
#define DEFAULT_GAMMA	"0 2 2 2 2 2 2 2 " \
			"2 2 2 2 2 2 2 2 " \
			"2 2 2 2 2 2 2 2 " \
			"2 2 2 2 2 2 2 2 " \
			"2 2 2 2 2 2 2 2 " \
			"2 2 2 2 2 2 2 2 " \
			"2 2 2 2 2 2 2 2 " \
			"2 2 2 2 2 2 2" \

static void register_onboard_backlight(struct fbtft_par *par);

static int init_display(struct fbtft_par *par)
{
	if (par->pdata &&
	    par->pdata->display.backlight == FBTFT_ONBOARD_BACKLIGHT) {
		/* module uses onboard GPIO for panel power */
		par->fbtftops.register_backlight = register_onboard_backlight;
	}

	par->fbtftops.reset(par);

	write_reg(par, 0xfd, 0x12); /* Command Lock */
	write_reg(par, 0xfd, 0xb1); /* Command Lock */
	write_reg(par, 0xae); /* Display Off */
	write_reg(par, 0xb3, 0xf1); /* Front Clock Div */
	write_reg(par, 0xca, 0x7f); /* Set Mux Ratio */
	write_reg(par, 0x15, 0x00, 0x7f); /* Set Column Address */
	write_reg(par, 0x75, 0x00, 0x7f); /* Set Row Address */
	write_reg(par, 0xa1, 0x00); /* Set Display Start Line */
	write_reg(par, 0xa2, 0x00); /* Set Display Offset */
	write_reg(par, 0xb5, 0x00); /* Set GPIO */
	write_reg(par, 0xab, 0x01); /* Set Function Selection */
	write_reg(par, 0xb1, 0x32); /* Set Phase Length */
	write_reg(par, 0xb4, 0xa0, 0xb5, 0x55); /* Set Segment Low Voltage */
	write_reg(par, 0xbb, 0x17); /* Set Precharge Voltage */
	write_reg(par, 0xbe, 0x05); /* Set VComH Voltage */
	write_reg(par, 0xc1, 0xc8, 0x80, 0xc8); /* Set Contrast */
	write_reg(par, 0xc7, 0x0f); /* Set Master Contrast */
	write_reg(par, 0xb6, 0x01); /* Set Second Precharge Period */
	write_reg(par, 0xa6); /* Set Display Mode Reset */
	write_reg(par, 0xaf); /* Set Sleep Mode Display On */

	return 0;
}

static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	write_reg(par, 0x15, xs, xe);
	write_reg(par, 0x75, ys, ye);
	write_reg(par, 0x5c);
}

static int set_var(struct fbtft_par *par)
{
	unsigned int remap;

	if (par->fbtftops.init_display != init_display) {
		/* don't risk messing up register A0h */
		return 0;
	}

	remap = 0x60 | (par->bgr << 2); /* Set Colour Depth */

	switch (par->info->var.rotate) {
	case 0:
		write_reg(par, 0xA0, remap | 0x00 | BIT(4));
		break;
	case 270:
		write_reg(par, 0xA0, remap | 0x03 | BIT(4));
		break;
	case 180:
		write_reg(par, 0xA0, remap | 0x02);
		break;
	case 90:
		write_reg(par, 0xA0, remap | 0x01);
		break;
	}

	return 0;
}

static noinline_for_stack void write_gamma_reg(struct fbtft_par *par,
					       u32 gamma[63])
{
	write_reg(par, 0xB8,
		  gamma[0],  gamma[1],  gamma[2],  gamma[3],
		  gamma[4],  gamma[5],  gamma[6],  gamma[7],
		  gamma[8],  gamma[9],  gamma[10], gamma[11],
		  gamma[12], gamma[13], gamma[14], gamma[15],
		  gamma[16], gamma[17], gamma[18], gamma[19],
		  gamma[20], gamma[21], gamma[22], gamma[23],
		  gamma[24], gamma[25], gamma[26], gamma[27],
		  gamma[28], gamma[29], gamma[30], gamma[31],
		  gamma[32], gamma[33], gamma[34], gamma[35],
		  gamma[36], gamma[37], gamma[38], gamma[39],
		  gamma[40], gamma[41], gamma[42], gamma[43],
		  gamma[44], gamma[45], gamma[46], gamma[47],
		  gamma[48], gamma[49], gamma[50], gamma[51],
		  gamma[52], gamma[53], gamma[54], gamma[55],
		  gamma[56], gamma[57], gamma[58], gamma[59],
		  gamma[60], gamma[61], gamma[62]);
}

/*
 * Grayscale Lookup Table
 * GS1 - GS63
 * The driver Gamma curve contains the relative values between the entries
 * in the Lookup table.
 *
 * From datasheet:
 * 8.8 Gray Scale Decoder
 *
 *	there are total 180 Gamma Settings (Setting 0 to Setting 180)
 *	available for the Gray Scale table.
 *
 *	The gray scale is defined in incremental way, with reference
 *	to the length of previous table entry:
 *		Setting of GS1 has to be >= 0
 *		Setting of GS2 has to be > Setting of GS1 +1
 *		Setting of GS3 has to be > Setting of GS2 +1
 *		:
 *		Setting of GS63 has to be > Setting of GS62 +1
 *
 */
static int set_gamma(struct fbtft_par *par, u32 *curves)
{
	u32 tmp[GAMMA_NUM * GAMMA_LEN];
	int i, acc = 0;

	for (i = 0; i < 63; i++) {
		if (i > 0 && curves[i] < 2) {
			dev_err(par->info->device,
				"Illegal value in Grayscale Lookup Table at index %d : %d. Must be greater than 1\n",
				i, curves[i]);
			return -EINVAL;
		}
		acc += curves[i];
		tmp[i] = acc;
		if (acc > 180) {
			dev_err(par->info->device,
				"Illegal value(s) in Grayscale Lookup Table. At index=%d : %d, the accumulated value has exceeded 180\n",
				i, acc);
			return -EINVAL;
		}
	}

	write_gamma_reg(par, tmp);

	return 0;
}

static int blank(struct fbtft_par *par, bool on)
{
	fbtft_par_dbg(DEBUG_BLANK, par, "(%s=%s)\n",
		      __func__, str_true_false(on));
	if (on)
		write_reg(par, 0xAE);
	else
		write_reg(par, 0xAF);
	return 0;
}

static struct fbtft_display display = {
	.regwidth = 8,
	.width = WIDTH,
	.height = HEIGHT,
	.gamma_num = GAMMA_NUM,
	.gamma_len = GAMMA_LEN,
	.gamma = DEFAULT_GAMMA,
	.fbtftops = {
		.init_display = init_display,
		.set_addr_win = set_addr_win,
		.set_var = set_var,
		.set_gamma = set_gamma,
		.blank = blank,
	},
};

static int update_onboard_backlight(struct backlight_device *bd)
{
	struct fbtft_par *par = bl_get_data(bd);
	bool on;

	fbtft_par_dbg(DEBUG_BACKLIGHT, par, "%s: power=%d\n", __func__, bd->props.power);

	on = !backlight_is_blank(bd);
	/* Onboard backlight connected to GPIO0 on SSD1351, GPIO1 unused */
	write_reg(par, 0xB5, on ? 0x03 : 0x02);

	return 0;
}

static const struct backlight_ops bl_ops = {
	.update_status = update_onboard_backlight,
};

static void register_onboard_backlight(struct fbtft_par *par)
{
	struct backlight_device *bd;
	struct backlight_properties bl_props = { 0, };

	bl_props.type = BACKLIGHT_RAW;
	bl_props.power = BACKLIGHT_POWER_OFF;

	bd = backlight_device_register(dev_driver_string(par->info->device),
				       par->info->device, par, &bl_ops,
				       &bl_props);
	if (IS_ERR(bd)) {
		dev_err(par->info->device,
			"cannot register backlight device (%ld)\n",
			PTR_ERR(bd));
		return;
	}
	par->info->bl_dev = bd;

	if (!par->fbtftops.unregister_backlight)
		par->fbtftops.unregister_backlight = fbtft_unregister_backlight;
}

FBTFT_REGISTER_DRIVER(DRVNAME, "solomon,ssd1351", &display);

MODULE_ALIAS("spi:" DRVNAME);
MODULE_ALIAS("platform:" DRVNAME);
MODULE_ALIAS("spi:ssd1351");
MODULE_ALIAS("platform:ssd1351");

MODULE_DESCRIPTION("SSD1351 OLED Driver");
MODULE_AUTHOR("James Davies");
MODULE_LICENSE("GPL");
