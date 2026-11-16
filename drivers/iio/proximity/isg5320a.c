// SPDX-License-Identifier: GPL-2.0
/*
 * Imagis ISG5320A capacitive grip (SAR proximity) sensor.
 *
 * Register map, init table and every write sequence below come from the
 * Samsung vendor driver (drivers/sensors/isg5320a.c + isg5320a_reg.h,
 * Galaxy XCover5 5.10 tree, Copyright (C) Imagis Technology). Citations of
 * the form "vendor :NNN" refer to lines in that isg5320a.c. This driver
 * writes no register the vendor driver does not write, and only with the
 * values the vendor writes; regmap enforces that through writeable_reg.
 *
 * The chip is used as one proximity channel: the vendor's "diff" value
 * (CDC16_T - BSL16_B, i.e. the B-channel signal minus its baseline) is
 * exposed as in_proximity0_raw, and the chip's CLOSE/FAR interrupt is
 * exposed as IIO threshold events (rising = grip, falling = release).
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>

/* Registers (vendor isg5320a_reg.h :19-79) */
#define ISG5320A_REG_IRQSRC		0x00	/* read releases the INT line */
#define ISG5320A_REG_IRQSTS		0x01
#define   ISG5320A_IRQSTS_PROX		BIT(4)	/* ISG5320A_PROX_STATE, reg.h :81 */
#define ISG5320A_REG_OSCCON		0x06
#define   ISG5320A_OSC_SLEEP		0xB0
#define   ISG5320A_OSC_NORMAL		0xF0
#define ISG5320A_REG_IRQFUNC		0x07
#define   ISG5320A_IRQFUNC_ENABLE	0x0D
#define   ISG5320A_IRQFUNC_DISABLE	0x13
#define ISG5320A_REG_BS_ON_WD		0x19
#define   ISG5320A_BS_WD_OFF		0x0A
#define   ISG5320A_BS_WD_ON		0x8A
#define ISG5320A_REG_SCANCTRL1		0x38
#define   ISG5320A_SCAN_STOP		0x00
#define   ISG5320A_CFCAL_START		0xDD
#define ISG5320A_REG_SCANCTRL2		0x39
#define   ISG5320A_SCAN2_RESET		0x00
#define   ISG5320A_SCAN2_CLEAR		0x60
#define   ISG5320A_BFCAL_START		0x07
/* CDC16_T_H, CDC16_T_L, BSL16_B_H, BSL16_B_L (vendor :453) */
#define ISG5320A_REG_CDC16_T_H		0x5C
/* B_PROXCTL3..5: threshold, close hysteresis, far hysteresis (vendor :890) */
#define ISG5320A_REG_B_PROXCTL3		0x9B
#define ISG5320A_REG_DIGITAL_ACC	0xA4
#define ISG5320A_REG_PROTECT		0xCD
#define   ISG5320A_PROTECT_UNLOCK	0xDE
#define ISG5320A_REG_SOFTRESET		0xFD
#define   ISG5320A_SOFTRESET_VAL	0xDE
#define ISG5320A_REG_CHIPID		0xFE
#define ISG5320A_MAX_REG		0xFE

#define ISG5320A_VENDOR_CHIP_ID		0x32	/* vendor :39, never checked */
#define ISG5320A_RESET_CONDITION	5000	/* reg.h :99, for cfcal_th */

#define ISG5320A_AUTOSUSPEND_MS		5000

struct isg5320a_reg_data {
	u8 addr;
	u8 val;
};

/*
 * Vendor setup_reg[] for "XCOVER5 ALL" (isg5320a_reg.h :283-435). The other
 * table (:130-282) is under CONFIG_SENSORS_ISG5320A_XCOVER5_KOR, which the
 * stock exynos850-xcover5xx_defconfig leaves unset; it differs only in
 * 0x9C (threshold low byte), 0xA0 (far hysteresis low byte) and 0xBA.
 */
static const struct isg5320a_reg_data isg5320a_setup_reg[] = {
	{ 0x05, 0xFC }, { 0x06, 0xF1 }, { 0x07, 0x13 }, { 0x08, 0x01 },
	{ 0x09, 0x80 }, { 0x0A, 0x20 }, { 0x0B, 0x00 }, { 0x0C, 0x00 },
	{ 0x12, 0x89 }, { 0x19, 0x8A }, { 0x1B, 0x28 }, { 0x1D, 0x20 },
	{ 0x1E, 0x00 }, { 0x1F, 0x18 }, { 0x20, 0x05 }, { 0x21, 0x10 },
	{ 0x22, 0x10 }, { 0x23, 0x00 }, { 0x24, 0x00 }, { 0x25, 0x05 },
	{ 0x26, 0x05 }, { 0x27, 0x10 }, { 0x28, 0x20 }, { 0x29, 0x00 },
	{ 0x2A, 0x20 }, { 0x2B, 0x00 }, { 0x2C, 0x00 }, { 0x2D, 0x20 },
	{ 0x2F, 0x18 }, { 0x30, 0xC0 }, { 0x41, 0x11 }, { 0x60, 0xF0 },
	{ 0x61, 0x8F }, { 0x62, 0x0C }, { 0x63, 0x00 }, { 0x64, 0x00 },
	{ 0x65, 0x44 }, { 0x66, 0x3C }, { 0x67, 0x01 }, { 0x68, 0x00 },
	{ 0x69, 0x00 }, { 0x6A, 0x10 }, { 0x6B, 0x00 }, { 0x6C, 0x10 },
	{ 0x6D, 0x40 }, { 0x6E, 0x22 }, { 0x6F, 0x00 }, { 0x70, 0x01 },
	{ 0x71, 0x60 }, { 0x72, 0xFF }, { 0x73, 0xFF }, { 0x74, 0xFF },
	{ 0x7B, 0x08 }, { 0x7C, 0x00 }, { 0x7D, 0x0C }, { 0x7E, 0x00 },
	{ 0x7F, 0xF8 }, { 0x80, 0xF8 }, { 0x81, 0x00 }, { 0x82, 0xF0 },
	{ 0x83, 0xF0 }, { 0x84, 0xF0 }, { 0x85, 0xF0 }, { 0x86, 0xF0 },
	{ 0x87, 0xF0 }, { 0x88, 0x00 }, { 0x89, 0x00 }, { 0x8A, 0x08 },
	{ 0x8B, 0x00 }, { 0x8C, 0x02 }, { 0x8D, 0x00 }, { 0x99, 0x21 },
	{ 0x9A, 0x3C }, { 0x9B, 0x01 }, { 0x9C, 0x2C }, { 0x9D, 0x00 },
	{ 0x9E, 0x00 }, { 0x9F, 0x00 }, { 0xA0, 0x96 }, { 0xA1, 0xC0 },
	{ 0xA2, 0x22 }, { 0xA3, 0x00 }, { 0xA4, 0x08 }, { 0xA5, 0x60 },
	{ 0xA6, 0xFF }, { 0xA7, 0xFF }, { 0xA8, 0xFF }, { 0xA9, 0x00 },
	{ 0xAF, 0x40 }, { 0xB0, 0x00 }, { 0xB1, 0x60 }, { 0xB2, 0x00 },
	{ 0xB3, 0xD0 }, { 0xB4, 0x10 }, { 0xB5, 0x28 }, { 0xB6, 0xF0 },
	{ 0xB7, 0xF6 }, { 0xB8, 0xFD }, { 0xB9, 0xDE }, { 0xBA, 0xFD },
	{ 0xBB, 0xF0 }, { 0xBC, 0x00 }, { 0xBD, 0xFF }, { 0xBE, 0x01 },
	{ 0xBF, 0x5E }, { 0xC0, 0x00 }, { 0xC1, 0x69 }, { 0xC2, 0xF0 },
	{ 0xC3, 0x00 }, { 0xC4, 0x00 }, { 0xC5, 0x00 }, { 0x13, 0x3A },
	{ 0x18, 0xF1 }, /* A ch : CS0, B ch : CS1 */
	{ 0x1C, 0x18 }, { 0x35, 0x00 }, { 0x36, 0x00 }, { 0x3A, 0x0A },
	{ 0x3B, 0x50 }, { 0x3C, 0x0A }, { 0x3D, 0x50 }, { 0x3E, 0x60 },
	{ 0x94, 0x10 }, { 0x95, 0xA9 }, { 0x96, 0x11 }, { 0x2E, 0x14 },
	{ 0x0E, 0xE0 }, { 0x0F, 0x1E }, { 0x10, 0xC0 }, { 0x11, 0xC1 },
	{ 0xD0, 0xE0 }, { 0xD1, 0x08 }, { 0xD2, 0x00 }, { 0xD3, 0xFF },
	{ 0xD4, 0xFF }, { 0xD5, 0x1E }, { 0xD6, 0x32 }, { 0xD7, 0xFF },
	{ 0xD8, 0x20 }, { 0xD9, 0x70 }, { 0xDA, 0x7F }, { 0xDB, 0xE0 },
	{ 0xDC, 0x00 }, { 0x76, 0x08 }, { 0x77, 0x00 }, { 0x79, 0x02 },
	{ 0x7A, 0x00 }, { 0xAA, 0x36 }, { 0xAB, 0xB0 }, { 0xAD, 0x04 },
	{ 0xAE, 0x00 },
};

struct isg5320a_data {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *int_gpio;
	int irq;
	/* Serialises multi-register chip sequences and the cached sample. */
	struct mutex lock;
	/* Serialises event enable/disable (sleeps in disable_irq()). */
	struct mutex ev_lock;
	bool ev_enabled;
	bool close;
	u16 cdc;
	u16 base;
	u16 thresh;
	u16 close_hyst;
	u16 far_hyst;
	u32 cfcal_th;
};

static bool isg5320a_writeable_reg(struct device *dev, unsigned int reg)
{
	unsigned int i;

	switch (reg) {
	case ISG5320A_REG_OSCCON:
	case ISG5320A_REG_IRQFUNC:
	case ISG5320A_REG_BS_ON_WD:
	case ISG5320A_REG_SCANCTRL1:
	case ISG5320A_REG_SCANCTRL2:
	case ISG5320A_REG_PROTECT:
	case ISG5320A_REG_SOFTRESET:
		return true;
	}

	for (i = 0; i < ARRAY_SIZE(isg5320a_setup_reg); i++)
		if (isg5320a_setup_reg[i].addr == reg)
			return true;

	return false;
}

static bool isg5320a_precious_reg(struct device *dev, unsigned int reg)
{
	/* Reading IRQSRC acknowledges the interrupt; keep debugfs off it. */
	return reg == ISG5320A_REG_IRQSRC;
}

static const struct regmap_config isg5320a_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ISG5320A_MAX_REG,
	.writeable_reg = isg5320a_writeable_reg,
	.precious_reg = isg5320a_precious_reg,
	.cache_type = REGCACHE_NONE,
};

/* Wait for the INT line to reach @level: up to 10 x 5 ms (vendor :262, :281). */
static int isg5320a_wait_int(struct isg5320a_data *data, int level)
{
	int cnt;

	if (!data->int_gpio) {
		msleep(50);
		return 0;
	}

	for (cnt = 0; cnt < 10; cnt++) {
		if (gpiod_get_raw_value_cansleep(data->int_gpio) == level)
			return cnt;
		usleep_range(5000, 5100);
	}

	return -ETIMEDOUT;
}

/* vendor isg5320a_reset(), :247-292, plus a CHIPID read the vendor never does */
static int isg5320a_reset(struct isg5320a_data *data)
{
	struct device *dev = data->dev;
	unsigned int val;
	int ret;

	usleep_range(5000, 5100);				/* :255-256 */

	ret = regmap_read(data->regmap, ISG5320A_REG_IRQSRC, &val);	/* :258 */
	if (ret)
		return dev_err_probe(dev, ret, "no response at IRQSRC\n");

	ret = regmap_read(data->regmap, ISG5320A_REG_CHIPID, &val);
	if (ret)
		return dev_err_probe(dev, ret, "CHIPID read failed\n");
	if (val == ISG5320A_VENDOR_CHIP_ID)
		dev_info(dev, "CHIPID 0x%02x\n", val);
	else
		dev_warn(dev, "CHIPID 0x%02x (vendor header says 0x%02x; not enforced)\n",
			 val, ISG5320A_VENDOR_CHIP_ID);

	if (isg5320a_wait_int(data, 1) < 0)			/* :262-266 */
		dev_warn(dev, "INT did not go high before reset\n");

	ret = regmap_write(data->regmap, ISG5320A_REG_PROTECT,	/* :268-271 */
			   ISG5320A_PROTECT_UNLOCK);
	if (ret)
		dev_warn(dev, "unlock protect failed: %d\n", ret);

	ret = regmap_write(data->regmap, ISG5320A_REG_SOFTRESET,	/* :273-276 */
			   ISG5320A_SOFTRESET_VAL);
	if (ret)
		dev_warn(dev, "soft reset write failed: %d\n", ret);

	usleep_range(1000, 1100);				/* :278 */

	ret = isg5320a_wait_int(data, 0);			/* :280-289 */
	if (ret < 0)
		return dev_err_probe(dev, -EIO, "INT not asserted after soft reset\n");
	dev_dbg(dev, "reset done, wait cnt %d\n", ret);

	return 0;
}

/* vendor isg5320a_initialize(), :614-660, without the /sdcard override */
static int isg5320a_initialize(struct isg5320a_data *data)
{
	struct device *dev = data->dev;
	unsigned int i, val, mismatch = 0;
	u8 buf[6];
	int ret;

	guard(mutex)(&data->lock);

	ret = regmap_read(data->regmap, ISG5320A_REG_IRQSRC, &val);	/* :625 */
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, ISG5320A_REG_SCANCTRL1,	/* :626 */
			   ISG5320A_SCAN_STOP);
	if (ret)
		return ret;
	msleep(30);						/* :627 */

	for (i = 0; i < ARRAY_SIZE(isg5320a_setup_reg); i++) {	/* :631-637 */
		const struct isg5320a_reg_data *r = &isg5320a_setup_reg[i];

		ret = regmap_write(data->regmap, r->addr, r->val);
		if (ret)
			return dev_err_probe(dev, ret, "init write 0x%02x failed\n",
					     r->addr);
		ret = regmap_read(data->regmap, r->addr, &val);
		if (ret)
			return ret;
		if (val != r->val) {
			mismatch++;
			dev_dbg(dev, "reg 0x%02x wrote 0x%02x read 0x%02x\n",
				r->addr, r->val, val);
		}
	}
	dev_info(dev, "init table: %zu writes, %u read back different\n",
		 ARRAY_SIZE(isg5320a_setup_reg), mismatch);

	/* :646-650 reads the threshold; :890-897 reads all six bytes */
	ret = regmap_bulk_read(data->regmap, ISG5320A_REG_B_PROXCTL3, buf,
			       sizeof(buf));
	if (ret)
		return ret;
	data->thresh = (buf[0] << 8) | buf[1];
	data->close_hyst = (buf[2] << 8) | buf[3];
	data->far_hyst = (buf[4] << 8) | buf[5];

	ret = regmap_read(data->regmap, ISG5320A_REG_DIGITAL_ACC, &val);	/* :653-657 */
	if (ret)
		return ret;
	data->cfcal_th = ISG5320A_RESET_CONDITION * val / 8;

	dev_info(dev, "threshold %u, close hyst %u, far hyst %u, cfcal_th %u\n",
		 data->thresh, data->close_hyst, data->far_hyst, data->cfcal_th);

	return 0;
}

/* vendor isg5320a_set_mode(SLEEP), :749-755 */
static int isg5320a_mode_sleep(struct isg5320a_data *data)
{
	unsigned int val;
	int ret;

	guard(mutex)(&data->lock);

	ret = regmap_read(data->regmap, ISG5320A_REG_IRQSRC, &val);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, ISG5320A_REG_SCANCTRL1, ISG5320A_SCAN_STOP);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, ISG5320A_REG_OSCCON, ISG5320A_OSC_SLEEP);
	if (ret)
		return ret;
	return regmap_write(data->regmap, ISG5320A_REG_BS_ON_WD, ISG5320A_BS_WD_OFF);
}

/*
 * vendor isg5320a_set_mode(NORMAL), :749 + :757-758, which runs
 * isg5320a_force_calibration(data, false), :302-321.
 */
static int isg5320a_mode_normal(struct isg5320a_data *data)
{
	struct regmap *map = data->regmap;
	unsigned int val;
	int ret;

	guard(mutex)(&data->lock);

	ret = regmap_read(map, ISG5320A_REG_IRQSRC, &val);			/* :749 */
	if (ret)
		return ret;
	ret = regmap_write(map, ISG5320A_REG_BS_ON_WD, ISG5320A_BS_WD_ON);	/* :757 */
	if (ret)
		return ret;

	ret = regmap_write(map, ISG5320A_REG_SCANCTRL1, ISG5320A_SCAN_STOP);	/* :303 */
	if (ret)
		return ret;
	msleep(30);								/* :305 */
	ret = regmap_write(map, ISG5320A_REG_BS_ON_WD, ISG5320A_BS_WD_OFF);	/* :306 */
	if (ret)
		return ret;
	ret = regmap_write(map, ISG5320A_REG_SCANCTRL2, ISG5320A_SCAN2_CLEAR);	/* :307 */
	if (ret)
		return ret;
	msleep(100);								/* :309 */
	ret = regmap_write(map, ISG5320A_REG_OSCCON, ISG5320A_OSC_NORMAL);	/* :310 */
	if (ret)
		return ret;
	ret = regmap_write(map, ISG5320A_REG_BS_ON_WD, ISG5320A_BS_WD_ON);	/* :312 */
	if (ret)
		return ret;
	ret = regmap_write(map, ISG5320A_REG_SCANCTRL2, ISG5320A_SCAN2_RESET);	/* :313 */
	if (ret)
		return ret;
	ret = regmap_write(map, ISG5320A_REG_SCANCTRL1, ISG5320A_CFCAL_START);	/* :316 */
	if (ret)
		return ret;
	msleep(500);								/* :318 */
	ret = regmap_write(map, ISG5320A_REG_SCANCTRL2, ISG5320A_BFCAL_START);	/* :321 */
	if (ret)
		return ret;

	/* The vendor sw_reset path waits 300 ms after BFCAL before reading (:850-852). */
	msleep(300);

	return 0;
}

/* vendor isg5320a_get_raw_data(), :445-466; caller holds data->lock */
static int isg5320a_read_sample(struct isg5320a_data *data)
{
	u8 buf[4];
	u16 v;
	int ret;

	lockdep_assert_held(&data->lock);

	ret = regmap_bulk_read(data->regmap, ISG5320A_REG_CDC16_T_H, buf,
			       sizeof(buf));
	if (ret)
		return ret;

	/* 0x0000 and 0xFFFF are discarded and the last value kept (:459-464) */
	v = (buf[0] << 8) | buf[1];
	if (v != 0 && v != 0xFFFF)
		data->cdc = v;
	v = (buf[2] << 8) | buf[3];
	if (v != 0 && v != 0xFFFF)
		data->base = v;

	return 0;
}

static int isg5320a_get_sample(struct isg5320a_data *data, u16 *cdc, u16 *base)
{
	struct device *dev = data->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	scoped_guard(mutex, &data->lock) {
		ret = isg5320a_read_sample(data);
		*cdc = data->cdc;
		*base = data->base;
	}

	pm_runtime_put_autosuspend(dev);

	/*
	 * The vendor cal_work (:1640-1647) force-recalibrates when cdc drops
	 * below cfcal_th. That loop is not ported; only report the condition.
	 */
	if (!ret && *cdc < data->cfcal_th)
		dev_warn_ratelimited(dev, "cdc %u below cfcal_th %u (vendor would recalibrate)\n",
				     *cdc, data->cfcal_th);

	return ret;
}

static irqreturn_t isg5320a_irq_thread(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct isg5320a_data *data = iio_priv(indio_dev);
	s64 timestamp = iio_get_time_ns(indio_dev);
	unsigned int src, sts;
	bool changed = false;
	bool close = false;
	int ret;

	scoped_guard(mutex, &data->lock) {
		isg5320a_read_sample(data);				/* :581 */

		ret = regmap_read(data->regmap, ISG5320A_REG_IRQSRC, &src);	/* :583 */
		if (ret)
			break;
		ret = regmap_read(data->regmap, ISG5320A_REG_IRQSTS, &sts);	/* :598 */
		if (ret)
			break;

		close = sts & ISG5320A_IRQSTS_PROX;			/* :518 */
		changed = close != data->close;
		data->close = close;
	}

	if (ret) {
		dev_err_ratelimited(data->dev, "irq status read failed: %d\n", ret);
		return IRQ_HANDLED;
	}

	dev_dbg(data->dev, "irq src 0x%02x sts 0x%02x cdc %u base %u\n",
		src, sts, data->cdc, data->base);

	if (changed && READ_ONCE(data->ev_enabled)) {
		enum iio_event_direction dir;

		dir = close ? IIO_EV_DIR_RISING : IIO_EV_DIR_FALLING;
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
						    IIO_EV_TYPE_THRESH, dir),
			       timestamp);
	}

	return IRQ_HANDLED;
}

static int isg5320a_read_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     int *val, int *val2, long mask)
{
	struct isg5320a_data *data = iio_priv(indio_dev);
	u16 cdc, base;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = isg5320a_get_sample(data, &cdc, &base);
		if (ret)
			return ret;
		*val = (int)cdc - (int)base;				/* :465 */
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int isg5320a_read_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir)
{
	struct isg5320a_data *data = iio_priv(indio_dev);

	if (type != IIO_EV_TYPE_THRESH || dir != IIO_EV_DIR_EITHER)
		return -EINVAL;

	return READ_ONCE(data->ev_enabled);
}

/* vendor isg5320a_set_enable(), :681-742 (minus the work items) */
static int isg5320a_events_on(struct isg5320a_data *data)
{
	struct device *dev = data->dev;
	unsigned int val;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	scoped_guard(mutex, &data->lock) {
		ret = regmap_read(data->regmap, ISG5320A_REG_IRQSTS, &val);	/* :700 */
		if (ret)
			break;
		data->close = val & ISG5320A_IRQSTS_PROX;		/* :708-713 */
		isg5320a_read_sample(data);				/* :704 */
		ret = regmap_read(data->regmap, ISG5320A_REG_IRQSRC, &val);	/* :718 */
		if (ret)
			break;
		ret = regmap_write(data->regmap, ISG5320A_REG_IRQFUNC,	/* :719 */
				   ISG5320A_IRQFUNC_ENABLE);
	}
	if (ret) {
		pm_runtime_put_autosuspend(dev);
		return ret;
	}

	WRITE_ONCE(data->ev_enabled, true);
	enable_irq(data->irq);						/* :721 */
	if (device_may_wakeup(dev))
		enable_irq_wake(data->irq);				/* :722 */

	dev_dbg(dev, "events on, initial state %s\n", data->close ? "close" : "far");

	return 0;
}

static void isg5320a_events_off(struct isg5320a_data *data)
{
	struct device *dev = data->dev;
	int ret = 0;

	scoped_guard(mutex, &data->lock)
		ret = regmap_write(data->regmap, ISG5320A_REG_IRQFUNC,	/* :726 */
				   ISG5320A_IRQFUNC_DISABLE);
	if (ret)
		dev_warn(dev, "IRQFUNC disable failed: %d\n", ret);

	WRITE_ONCE(data->ev_enabled, false);
	disable_irq(data->irq);						/* :728 */
	if (device_may_wakeup(dev))
		disable_irq_wake(data->irq);				/* :729 */
}

static int isg5320a_write_event_config(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan,
				       enum iio_event_type type,
				       enum iio_event_direction dir,
				       bool state)
{
	struct isg5320a_data *data = iio_priv(indio_dev);

	if (type != IIO_EV_TYPE_THRESH || dir != IIO_EV_DIR_EITHER)
		return -EINVAL;

	guard(mutex)(&data->ev_lock);

	if (state == data->ev_enabled)
		return 0;

	if (state)
		return isg5320a_events_on(data);

	isg5320a_events_off(data);
	pm_runtime_put_autosuspend(data->dev);

	return 0;
}

static int isg5320a_read_event_value(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info,
				     int *val, int *val2)
{
	struct isg5320a_data *data = iio_priv(indio_dev);

	if (type != IIO_EV_TYPE_THRESH || info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	/* vendor normal_threshold_show, :896-897 */
	switch (dir) {
	case IIO_EV_DIR_RISING:
		*val = data->thresh + data->close_hyst;
		return IIO_VAL_INT;
	case IIO_EV_DIR_FALLING:
		*val = (int)data->thresh - (int)data->far_hyst;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_event_spec isg5320a_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		/* One chip-level IRQ enable (IRQFUNC) covers both directions. */
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

#define ISG5320A_CHANNEL(_events, _nevents)			\
	{							\
		.type = IIO_PROXIMITY,				\
		.indexed = 1,					\
		.channel = 0,					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
		.event_spec = _events,				\
		.num_event_specs = _nevents,			\
	}

static const struct iio_chan_spec isg5320a_channels[] = {
	ISG5320A_CHANNEL(isg5320a_events, ARRAY_SIZE(isg5320a_events)),
};

static const struct iio_chan_spec isg5320a_channels_noirq[] = {
	ISG5320A_CHANNEL(NULL, 0),
};

static const struct iio_info isg5320a_info = {
	.read_raw = isg5320a_read_raw,
	.read_event_config = isg5320a_read_event_config,
	.write_event_config = isg5320a_write_event_config,
	.read_event_value = isg5320a_read_event_value,
};

static const struct iio_info isg5320a_info_noirq = {
	.read_raw = isg5320a_read_raw,
};

/* vendor isg5320a_remove(), :2030-2033 */
static void isg5320a_teardown(void *arg)
{
	struct isg5320a_data *data = arg;
	struct device *dev = data->dev;

	pm_runtime_disable(dev);

	if (data->ev_enabled) {
		isg5320a_events_off(data);
		pm_runtime_put_noidle(dev);
	}

	if (!pm_runtime_status_suspended(dev))
		isg5320a_mode_sleep(data);

	pm_runtime_set_suspended(dev);
	pm_runtime_dont_use_autosuspend(dev);
}

static int isg5320a_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct isg5320a_data *data;
	struct iio_dev *indio_dev;
	unsigned long irqflags;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->dev = dev;
	data->irq = client->irq;
	i2c_set_clientdata(client, indio_dev);

	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;
	ret = devm_mutex_init(dev, &data->ev_lock);
	if (ret)
		return ret;

	/* The vendor node has no supply; the board relies on an always-on LDO. */
	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "failed to enable vdd\n");
	/*
	 * If the rail was just switched on, give the chip time to come out
	 * of power-on reset. The vendor never switches it, so this value
	 * (10 ms) is a guess, not from the vendor source.
	 */
	if (!ret)
		usleep_range(10000, 11000);

	data->regmap = devm_regmap_init_i2c(client, &isg5320a_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap), "regmap init failed\n");

	/*
	 * Optional: lets the reset sequence watch the INT level as the vendor
	 * does. It is requested before the IRQ (same order as vendor :1873 vs
	 * :1931), since on Samsung pinctrl the IRQ request switches the pin
	 * back to its EINT function.
	 */
	data->int_gpio = devm_gpiod_get_optional(dev, "int", GPIOD_IN);
	if (IS_ERR(data->int_gpio))
		return dev_err_probe(dev, PTR_ERR(data->int_gpio), "int gpio\n");
	if (!data->int_gpio)
		dev_info(dev, "no int-gpios, reset uses fixed delays\n");

	ret = isg5320a_reset(data);				/* :1915 */
	if (ret)
		return ret;

	msleep(300);			/* vendor runs init_work 300 ms later, :1981 */

	ret = isg5320a_initialize(data);			/* :1620 */
	if (ret)
		return dev_err_probe(dev, ret, "init failed\n");

	ret = isg5320a_mode_normal(data);			/* :1621 */
	if (ret)
		return dev_err_probe(dev, ret, "start failed\n");

	if (data->irq > 0) {
		irqflags = irq_get_trigger_type(data->irq);
		if (!irqflags)
			irqflags = IRQF_TRIGGER_FALLING;	/* :1932 */

		ret = devm_request_threaded_irq(dev, data->irq, NULL,
						isg5320a_irq_thread,
						irqflags | IRQF_ONESHOT |
						IRQF_NO_AUTOEN,
						"isg5320a", indio_dev);
		if (ret)
			return dev_err_probe(dev, ret, "irq request failed\n");

		if (device_property_read_bool(dev, "wakeup-source"))
			device_init_wakeup(dev, true);
	}

	pm_runtime_set_autosuspend_delay(dev, ISG5320A_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_put_autosuspend(dev);

	ret = devm_add_action_or_reset(dev, isg5320a_teardown, data);
	if (ret)
		return ret;

	indio_dev->name = "isg5320a";
	indio_dev->modes = INDIO_DIRECT_MODE;
	if (data->irq > 0) {
		indio_dev->info = &isg5320a_info;
		indio_dev->channels = isg5320a_channels;
		indio_dev->num_channels = ARRAY_SIZE(isg5320a_channels);
	} else {
		indio_dev->info = &isg5320a_info_noirq;
		indio_dev->channels = isg5320a_channels_noirq;
		indio_dev->num_channels = ARRAY_SIZE(isg5320a_channels_noirq);
	}

	return devm_iio_device_register(dev, indio_dev);
}

/* vendor isg5320a_shutdown(), :2071-2080 */
static void isg5320a_shutdown(struct i2c_client *client)
{
	struct isg5320a_data *data = iio_priv(i2c_get_clientdata(client));

	guard(mutex)(&data->ev_lock);

	if (data->ev_enabled)
		regmap_write(data->regmap, ISG5320A_REG_IRQFUNC,
			     ISG5320A_IRQFUNC_DISABLE);
	isg5320a_mode_sleep(data);
}

static int isg5320a_runtime_suspend(struct device *dev)
{
	struct isg5320a_data *data = iio_priv(dev_get_drvdata(dev));

	return isg5320a_mode_sleep(data);
}

static int isg5320a_runtime_resume(struct device *dev)
{
	struct isg5320a_data *data = iio_priv(dev_get_drvdata(dev));

	return isg5320a_mode_normal(data);
}

/*
 * Runtime PM only. System sleep leaves the chip as it is, as the vendor
 * does (its suspend/resume only stop and restart the work items), so an
 * enabled grip event can still wake the system.
 */
static const struct dev_pm_ops isg5320a_pm_ops = {
	RUNTIME_PM_OPS(isg5320a_runtime_suspend, isg5320a_runtime_resume, NULL)
};

static const struct of_device_id isg5320a_of_match[] = {
	{ .compatible = "imagis,isg5320a" },
	{ }
};
MODULE_DEVICE_TABLE(of, isg5320a_of_match);

static const struct i2c_device_id isg5320a_id[] = {
	{ "isg5320a" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, isg5320a_id);

static struct i2c_driver isg5320a_driver = {
	.driver = {
		.name = "isg5320a",
		.of_match_table = isg5320a_of_match,
		.pm = pm_ptr(&isg5320a_pm_ops),
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = isg5320a_probe,
	.shutdown = isg5320a_shutdown,
	.id_table = isg5320a_id,
};
module_i2c_driver(isg5320a_driver);

MODULE_AUTHOR("Adil Hüsrev Tepe <tadilhusrev@gmail.com>");
MODULE_DESCRIPTION("Imagis ISG5320A capacitive grip sensor");
MODULE_LICENSE("GPL");
