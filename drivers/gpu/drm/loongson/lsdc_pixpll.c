// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include <linux/delay.h>

#include <drm/drm_managed.h>

#include "lsdc_drv.h"

/*
 * The structure of the pixel PLL registers is evolved with times,
 * it can be different across different chip also.
 */

/* size is u64, note that all loongson's cpu is little endian.
 * This structure is same for ls7a2000, ls7a1000 and ls2k2000.
 */
struct lsdc_pixpll_reg {
	/* Byte 0 ~ Byte 3 */
	unsigned div_out       : 7;   /*  6 : 0     Output clock divider  */
	unsigned _reserved_1_  : 14;  /* 20 : 7                           */
	unsigned loopc         : 9;   /* 29 : 21    Clock multiplier      */
	unsigned _reserved_2_  : 2;   /* 31 : 30                          */

	/* Byte 4 ~ Byte 7 */
	unsigned div_ref       : 7;   /* 38 : 32    Input clock divider   */
	unsigned locked        : 1;   /* 39         PLL locked indicator  */
	unsigned sel_out       : 1;   /* 40         output clk selector   */
	unsigned _reserved_3_  : 2;   /* 42 : 41                          */
	unsigned set_param     : 1;   /* 43         Trigger the update    */
	unsigned bypass        : 1;   /* 44                               */
	unsigned powerdown     : 1;   /* 45                               */
	unsigned _reserved_4_  : 18;  /* 46 : 63    no use                */
};

union lsdc_pixpll_reg_bitmap {
	struct lsdc_pixpll_reg bitmap;
	u32 w[2];
	u64 d;
};

/*
 * The pixel PLL structure of ls2k1000 is defferent than ls7a2000/ls2k2000.
 * it take up 16 bytes, but only a few bits are useful. Sounds like a little
 * bit of wasting register space, but this is the hardware already have been
 * tapped.
 */
union ls2k1000_pixpll_reg_bitmap {
	struct ls2k1000_pixpll_reg {
		/* Byte 0 ~ Byte 3 */
		unsigned sel_out      :  1;  /*  0    select this PLL        */
		unsigned _reserved_1_ :  1;  /*  1                           */
		unsigned sw_adj_en    :  1;  /*  2    allow software adjust  */
		unsigned bypass       :  1;  /*  3    bypass L1 PLL          */
		unsigned _reserved_2_ :  3;  /*  4:6                         */
		unsigned lock_en      :  1;  /*  7    enable lock L1 PLL     */
		unsigned _reserved_3_ :  2;  /*  8:9                         */
		unsigned lock_check   :  2;  /* 10:11 precision check        */
		unsigned _reserved_4_ :  4;  /* 12:15                        */

		unsigned locked       :  1;  /* 16    PLL locked flag bit    */
		unsigned _reserved_5_ :  2;  /* 17:18                        */
		unsigned powerdown    :  1;  /* 19    powerdown the pll      */
		unsigned _reserved_6_ :  6;  /* 20:25                        */
		unsigned div_ref      :  6;  /* 26:31 L1 Prescaler           */

		/* Byte 4 ~ Byte 7 */
		unsigned loopc        : 10;  /* 32:41 Clock Multiplier       */
		unsigned l1_div       :  6;  /* 42:47 no use                 */
		unsigned _reserved_7_ : 16;  /* 48:63                        */

		/* Byte 8 ~ Byte 15 */
		unsigned div_out      :  6;  /* 64 : 69   output clk divider */
		unsigned _reserved_8_ : 26;  /* 70 : 127  no use             */
	} bitmap;

	u32 w[4];
	u64 d[2];
};

/*
 * Update the pll parameters to hardware, target to the pixpll in ls2k1000
 *
 * @this: point to the object from which this function is called
 * @pin: point to the struct of lsdc_pll_parms passed in
 *
 * return 0 if successful.
 */
static int ls2k1000_pixpll_param_update(struct lsdc_pixpll * const this,
					struct lsdc_pixpll_parms const *pin)
{
	void __iomem *reg = this->mmio;
	unsigned int counter = 0;
	bool locked = false;
	u32 val;

	val = readl(reg);
	/* Bypass the software configured PLL, using refclk directly */
	val &= ~(1 << 0);
	writel(val, reg);

	/* Powerdown the PLL */
	val |= (1 << 19);
	writel(val, reg);

	/* Allow the software configuration */
	val &= ~(1 << 2);
	writel(val, reg);

	/* allow L1 PLL lock */
	val = (1L << 7) | (3L << 10);
	writel(val, reg);

	/* clear div_ref bit field */
	val &= ~(0x3f << 26);
	/* set div_ref bit field */
	val |= pin->div_ref << 26;
	writel(val, reg);

	val = readl(reg + 4);
	/* clear loopc bit field */
	val &= ~0x0fff;
	/* set loopc bit field */
	val |= pin->loopc;
	writel(val, reg + 4);

	/* set div_out */
	writel(pin->div_out, reg + 8);

	val = readl(reg);
	/* use this parms configured just now */
	val |= (1 << 2);
	/* powerup the PLL */
	val &= ~(1 << 19);
	writel(val, reg);

	/* wait pll setup and locked */
	do {
		val = readl(reg);
		locked = val & 0x10000;
		counter++;
	} while (!locked && (counter < 2000));

	drm_dbg(this->ddev, "%u loop waited\n", counter);

	/* Switch to software configured PLL instead of refclk */
	val |= 1;
	writel(val, reg);

	return 0;
}

static unsigned int
ls2k1000_pixpll_get_clock_rate(struct lsdc_pixpll * const this)
{
	struct lsdc_pixpll_parms *ppar = this->priv;
	union ls2k1000_pixpll_reg_bitmap reg_bitmap;
	struct ls2k1000_pixpll_reg *obj = &reg_bitmap.bitmap;

	reg_bitmap.w[0] = readl(this->mmio);
	reg_bitmap.w[1] = readl(this->mmio + 4);
	reg_bitmap.w[2] = readl(this->mmio + 8);
	reg_bitmap.w[3] = readl(this->mmio + 12);

	return ppar->ref_clock / obj->div_ref * obj->loopc / obj->div_out;
}

struct clk_to_pixpll_parms_lookup_t {
	unsigned int clock;        /* kHz */

	unsigned short width;
	unsigned short height;
	unsigned short vrefresh;

	/* Stores parameters for programming the Hardware PLLs */
	unsigned short div_out;
	unsigned short loopc;
	unsigned short div_ref;
};

static const struct clk_to_pixpll_parms_lookup_t pixpll_parms_table[] = {
	{148500, 1920, 1080, 60,  11, 49,  3},   /* 1920x1080@60Hz */
	{141750, 1920, 1080, 60,  11, 78,  5},   /* 1920x1080@60Hz */
						 /* 1920x1080@50Hz */
	{174500, 1920, 1080, 75,  17, 89,  3},   /* 1920x1080@75Hz */
	{181250, 2560, 1080, 75,  8,  58,  4},   /* 2560x1080@75Hz */
	{297000, 2560, 1080, 30,  8,  95,  4},   /* 3840x2160@30Hz */
	{301992, 1920, 1080, 100, 10, 151, 5},   /* 1920x1080@100Hz */
	{146250, 1680, 1050, 60,  16, 117, 5},   /* 1680x1050@60Hz */
	{135000, 1280, 1024, 75,  10, 54,  4},   /* 1280x1024@75Hz */
	{119000, 1680, 1050, 60,  20, 119, 5},   /* 1680x1050@60Hz */
	{108000, 1600, 900,  60,  15, 81,  5},   /* 1600x900@60Hz  */
						 /* 1280x1024@60Hz */
						 /* 1280x960@60Hz */
						 /* 1152x864@75Hz */

	{106500, 1440, 900,  60,  19, 81,  4},   /* 1440x900@60Hz */
	{88750,  1440, 900,  60,  16, 71,  5},   /* 1440x900@60Hz */
	{83500,  1280, 800,  60,  17, 71,  5},   /* 1280x800@60Hz */
	{71000,  1280, 800,  60,  20, 71,  5},   /* 1280x800@60Hz */

	{74250,  1280, 720,  60,  22, 49,  3},   /* 1280x720@60Hz */
						 /* 1280x720@50Hz */

	{78750,  1024, 768,  75,  16, 63,  5},   /* 1024x768@75Hz */
	{75000,  1024, 768,  70,  29, 87,  4},   /* 1024x768@70Hz */
	{65000,  1024, 768,  60,  20, 39,  3},   /* 1024x768@60Hz */

	{51200,  1024, 600,  60,  25, 64,  5},   /* 1024x600@60Hz */

	{57284,  832,  624,  75,  24, 55,  4},   /* 832x624@75Hz */
	{49500,  800,  600,  75,  40, 99,  5},   /* 800x600@75Hz */
	{50000,  800,  600,  72,  44, 88,  4},   /* 800x600@72Hz */
	{40000,  800,  600,  60,  30, 36,  3},   /* 800x600@60Hz */
	{36000,  800,  600,  56,  50, 72,  4},   /* 800x600@56Hz */
	{31500,  640,  480,  75,  40, 63,  5},   /* 640x480@75Hz */
						 /* 640x480@73Hz */

	{30240,  640,  480,  67,  62, 75,  4},   /* 640x480@67Hz */
	{27000,  720,  576,  50,  50, 54,  4},   /* 720x576@60Hz */
	{25175,  640,  480,  60,  85, 107, 5},   /* 640x480@60Hz */
	{25200,  640,  480,  60,  50, 63,  5},   /* 640x480@60Hz */
						 /* 720x480@60Hz */
};

static void lsdc_pixel_pll_free(struct drm_device *ddev, void *data)
{
	struct lsdc_pixpll *this = (struct lsdc_pixpll *)data;

	iounmap(this->mmio);

	kfree(this->priv);

	drm_dbg(ddev, "pixpll private data freed\n");
}

/*
 * ioremap the device dependent PLL registers
 *
 * @this: point to the object where this function is called from
 */
static int lsdc_pixel_pll_setup(struct lsdc_pixpll * const this)
{
	struct lsdc_pixpll_parms *pparms;

	this->mmio = ioremap(this->reg_base, this->reg_size);
	if (!this->mmio)
		return -ENOMEM;

	pparms = kzalloc(sizeof(*pparms), GFP_KERNEL);
	if (!pparms) {
		iounmap(this->mmio);
		return -ENOMEM;
	}

	pparms->ref_clock = LSDC_PLL_REF_CLK_KHZ;

	this->priv = pparms;

	return drmm_add_action_or_reset(this->ddev, lsdc_pixel_pll_free, this);
}

/*
 * Find a set of pll parameters from a static local table which avoid
 * computing the pll parameter eachtime a modeset is triggered.
 *
 * @this: point to the object where this function is called from
 * @clock: the desired output pixel clock, the unit is kHz
 * @pout: point to where the parameters to store if found
 *
 * Return 0 if success, return -1 if not found.
 */
static int lsdc_pixpll_find(struct lsdc_pixpll * const this,
			    unsigned int clock,
			    struct lsdc_pixpll_parms *pout)
{
	unsigned int num = ARRAY_SIZE(pixpll_parms_table);
	const struct clk_to_pixpll_parms_lookup_t *pt;
	unsigned int i;

	for (i = 0; i < num; ++i) {
		pt = &pixpll_parms_table[i];

		if (clock == pt->clock) {
			pout->div_ref = pt->div_ref;
			pout->loopc   = pt->loopc;
			pout->div_out = pt->div_out;

			return 0;
		}
	}

	drm_dbg_kms(this->ddev, "pixel clock %u: miss\n", clock);

	return -1;
}

/*
 * Find a set of pll parameters which have minimal difference with the
 * desired pixel clock frequency. It does that by computing all of the
 * possible combination. Compute the diff and find the combination with
 * minimal diff.
 *
 * clock_out = refclk / div_ref * loopc / div_out
 *
 * refclk is determined by the oscillator mounted on motherboard(100MHz
 * in almost all board)
 *
 * @this: point to the object from where this function is called
 * @clock: the desired output pixel clock, the unit is kHz
 * @pout: point to the out struct of lsdc_pixpll_parms
 *
 * Return 0 if a set of parameter is found, otherwise return the error
 * between clock_kHz we wanted and the most closest candidate with it.
 */
static int lsdc_pixel_pll_compute(struct lsdc_pixpll * const this,
				  unsigned int clock,
				  struct lsdc_pixpll_parms *pout)
{
	struct lsdc_pixpll_parms *pparms = this->priv;
	unsigned int refclk = pparms->ref_clock;
	const unsigned int tolerance = 1000;
	unsigned int min = tolerance;
	unsigned int div_out, loopc, div_ref;
	unsigned int computed;

	if (!lsdc_pixpll_find(this, clock, pout))
		return 0;

	for (div_out = 6; div_out < 64; div_out++) {
		for (div_ref = 3; div_ref < 6; div_ref++) {
			for (loopc = 6; loopc < 161; loopc++) {
				unsigned int diff = 0;

				if (loopc < 12 * div_ref)
					continue;
				if (loopc > 32 * div_ref)
					continue;

				computed = refclk / div_ref * loopc / div_out;

				if (clock >= computed)
					diff = clock - computed;
				else
					diff = computed - clock;

				if (diff < min) {
					min = diff;
					pparms->div_ref = div_ref;
					pparms->div_out = div_out;
					pparms->loopc = loopc;

					if (diff == 0) {
						*pout = *pparms;
						return 0;
					}
				}
			}
		}
	}

	/* still acceptable */
	if (min < tolerance) {
		*pout = *pparms;
		return 0;
	}

	drm_dbg(this->ddev, "can't find suitable params for %u khz\n", clock);

	return min;
}

/* Pixel pll hardware related ops, per display pipe */

static void __pixpll_rreg(struct lsdc_pixpll *this,
			  union lsdc_pixpll_reg_bitmap *dst)
{
#if defined(CONFIG_64BIT)
	dst->d = readq(this->mmio);
#else
	dst->w[0] = readl(this->mmio);
	dst->w[1] = readl(this->mmio + 4);
#endif
}

static void __pixpll_wreg(struct lsdc_pixpll *this,
			  union lsdc_pixpll_reg_bitmap *src)
{
#if defined(CONFIG_64BIT)
	writeq(src->d, this->mmio);
#else
	writel(src->w[0], this->mmio);
	writel(src->w[1], this->mmio + 4);
#endif
}

static void __pixpll_ops_powerup(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.powerdown = 0;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_powerdown(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.powerdown = 1;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_on(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.sel_out = 1;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_off(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.sel_out = 0;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_bypass(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.bypass = 1;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_unbypass(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.bypass = 0;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_untoggle_param(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.set_param = 0;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_set_param(struct lsdc_pixpll * const this,
				   struct lsdc_pixpll_parms const *p)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.div_ref = p->div_ref;
	pixpll_reg.bitmap.loopc = p->loopc;
	pixpll_reg.bitmap.div_out = p->div_out;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_toggle_param(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;

	__pixpll_rreg(this, &pixpll_reg);

	pixpll_reg.bitmap.set_param = 1;

	__pixpll_wreg(this, &pixpll_reg);
}

static void __pixpll_ops_wait_locked(struct lsdc_pixpll * const this)
{
	union lsdc_pixpll_reg_bitmap pixpll_reg;
	unsigned int counter = 0;

	do {
		__pixpll_rreg(this, &pixpll_reg);

		if (pixpll_reg.bitmap.locked)
			break;

		++counter;
	} while (counter < 2000);

	drm_dbg(this->ddev, "%u loop waited\n", counter);
}

/*
 * Update the PLL parameters to the PLL hardware
 *
 * @this: point to the object from which this function is called
 * @pin: point to the struct of lsdc_pixpll_parms passed in
 *
 * return 0 if successful.
 */
static int lsdc_pixpll_update(struct lsdc_pixpll * const this,
			      struct lsdc_pixpll_parms const *pin)
{
	__pixpll_ops_bypass(this);

	__pixpll_ops_off(this);

	__pixpll_ops_powerdown(this);

	__pixpll_ops_toggle_param(this);

	__pixpll_ops_set_param(this, pin);

	__pixpll_ops_untoggle_param(this);

	__pixpll_ops_powerup(this);

	udelay(2);

	__pixpll_ops_wait_locked(this);

	__pixpll_ops_on(this);

	__pixpll_ops_unbypass(this);

	return 0;
}

static unsigned int lsdc_pixpll_get_freq(struct lsdc_pixpll * const this)
{
	struct lsdc_pixpll_parms *ppar = this->priv;
	union lsdc_pixpll_reg_bitmap pix_pll_reg;
	unsigned int freq;

	__pixpll_rreg(this, &pix_pll_reg);

	ppar->div_ref = pix_pll_reg.bitmap.div_ref;
	ppar->loopc = pix_pll_reg.bitmap.loopc;
	ppar->div_out = pix_pll_reg.bitmap.div_out;

	freq = ppar->ref_clock / ppar->div_ref * ppar->loopc / ppar->div_out;

	return freq;
}

static void lsdc_pixpll_print(struct lsdc_pixpll * const this,
			      struct drm_printer *p)
{
	struct lsdc_pixpll_parms *parms = this->priv;

	drm_printf(p, "div_ref: %u, loopc: %u, div_out: %u\n",
		   parms->div_ref, parms->loopc, parms->div_out);
}

/*
 * LS7A1000, LS7A2000 and LS2K2000's pixel PLL register layout is same,
 * we take this as default, create a new instance if a different model is
 * introduced.
 */
const struct lsdc_pixpll_funcs ls7a1000_pixpll_funcs = {
	.setup = lsdc_pixel_pll_setup,
	.compute = lsdc_pixel_pll_compute,
	.update = lsdc_pixpll_update,
	.get_rate = lsdc_pixpll_get_freq,
	.print = lsdc_pixpll_print,
};

const struct lsdc_pixpll_funcs ls7a2000_pixpll_funcs = {
	.setup = lsdc_pixel_pll_setup,
	.compute = lsdc_pixel_pll_compute,
	.update = lsdc_pixpll_update,
	.get_rate = lsdc_pixpll_get_freq,
	.print = lsdc_pixpll_print,
};

/*
 * The bit fileds of LS2K1000's pixel PLL register are different from other
 * model, due to hardware update(revision). So we introduce a new instance
 * of lsdc_pixpll_funcs object function to gear the control.
 */
const struct lsdc_pixpll_funcs ls2k1000_pixpll_funcs = {
	.setup = lsdc_pixel_pll_setup,
	.compute = lsdc_pixel_pll_compute,
	.update = ls2k1000_pixpll_param_update,
	.get_rate = ls2k1000_pixpll_get_clock_rate,
	.print = lsdc_pixpll_print,
};

/* pixel pll initialization */

int lsdc_pixpll_init(struct lsdc_pixpll * const this,
		     struct drm_device *ddev,
		     unsigned int index)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct lsdc_desc *descp = ldev->descp;
	const struct loongson_gfx_desc *gfx = to_loongson_gfx(descp);

	this->ddev = ddev;
	this->reg_size = 8;
	this->reg_base = gfx->conf_reg_base + gfx->pixpll[index].reg_offset;
	this->funcs = gfx->pixpll_funcs;

	return this->funcs->setup(this);
}
