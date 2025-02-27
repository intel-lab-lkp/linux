// SPDX-License-Identifier: GPL-2.0
/*
 * TMC5240 Stepper motor controller driver
 *
 * Copyright (C) 2024 Protonic Holland
 *      David Jander <david@protonic.nl>
 */

#include <linux/bug.h>
#include <asm-generic/errno-base.h>
#include <linux/bitfield.h>
#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/motion.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/gfp_types.h>
#include <linux/types.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <asm/unaligned.h>

#include "motion-core.h"

/* General Configuration Registers */
#define TMC_REG_GCONF		0x00
#define TMC_REG_GSTAT		0x01
#define TMC_REG_IFCNT		0x02
#define TMC_REG_NODECONF	0x03
#define TMC_REG_IOIN		0x04
#define TMC_IOIN_VERSION_MASK	GENMASK(31, 24)
#define TMC_IOIN_REVISION_MASK	GENMASK(18, 16)
#define TMC_REG_X_COMPARE	0x05
#define TMC_REG_X_COMPARE_REP	0x06
#define TMC_REG_DRV_CONF	0x0a
#define TMC_DRV_CURRENT_RANGE	GENMASK(1, 0)
#define TMC_DRV_SLOPE_CONTROL	GENMASK(5, 4)
#define TMC_REG_GLOBAL_SCALER	0x0b
/* Velocity Dependent Configuration Registers */
#define TMC_REG_IHOLD_IRUN	0x10
#define TMC_IHOLD_IRUNDELAY	GENMASK(27, 24)
#define TMC_IHOLD_IHOLDDELAY	GENMASK(19, 16)
#define TMC_IHOLD_IRUN		GENMASK(12, 8)
#define TMC_IHOLD_IHOLD		GENMASK(4, 0)
#define TMC_REG_TPOWERDOWN	0x11
#define TMC_REG_TSTEP		0x12
#define TMC_REG_TPWMTHRS	0x13
#define TMC_REG_TCOOLTHRS	0x14
#define TMC_REG_THIGH		0x15
/* Ramp Generator Registers */
#define TMC_REG_RAMPMODE	0x20
#define TMC_RAMPMODE_POSITION	0
#define TMC_RAMPMODE_POS_VEL	1
#define TMC_RAMPMODE_NEG_VEL	2
#define TMC_RAMPMODE_HOLD	3
#define TMC_REG_XACTUAL		0x21
#define TMC_REG_VACTUAL		0x22
#define TMC_VACTUAL_SIGN_BIT	23
#define TMC_VACTUAL_MASK	GENMASK(TMC_VACTUAL_SIGN_BIT, 0)
#define TMC_REG_VSTART		0x23
#define TMC_REG_A1		0x24
#define TMC_REG_V1		0x25
#define TMC_REG_AMAX		0x26
#define TMC_MAX_ACCELERATION	GENMASK(17, 0)
#define TMC_REG_VMAX		0x27
#define TMC_REG_DMAX		0x28
#define TMC_REG_TVMAX		0x29
#define TMC_REG_D1		0x2a
#define TMC_D1_MIN		10
#define TMC_REG_VSTOP		0x2b
#define TMC_VSTOP_MIN		10
#define TMC_REG_TZEROWAIT	0x2c
#define TMC_REG_XTARGET		0x2d
#define TMC_REG_V2		0x2e
#define TMC_REG_A2		0x2f
#define TMC_REG_D2		0x30
#define TMC_D2_MIN		10
/* Ramp Generator Driver Feature Control Registers */
#define TMC_REG_VDCMIN		0x33
#define TMC_REG_SW_MODE		0x34
#define TMC_SW_VIRT_STOP_ENC	BIT(14)
#define TMC_SW_EN_VIRT_STOP_R	BIT(13)
#define TMC_SW_EN_VIRT_STOP_L	BIT(12)
#define TMC_SW_EN_SOFTSTOP	BIT(11)
#define TMC_SW_LATCH_R_ACTIVE	BIT(7)
#define TMC_SW_LATCH_L_ACTIVE	BIT(5)
#define TMC_SW_SWAP_LR		BIT(4)
#define TMC_SW_STOP_R_POL	BIT(3)
#define TMC_SW_STOP_L_POL	BIT(2)
#define TMC_SW_STOP_R_ENABLE	BIT(1)
#define TMC_SW_STOP_L_ENABLE	BIT(0)
#define TMC_REG_RAMP_STAT	0x35
#define TMC_RAMP_VIRT_R		BIT(15)
#define TMC_RAMP_VIRT_L		BIT(14)
#define TMC_VIRT_STOPS_MASK	GENMASK(15, 14)
#define TMC_RAMP_SECOND_MOVE	BIT(12)
#define TMC_RAMP_POS_REACHED	BIT(7)
#define TMC_RAMP_STOP_SG	BIT(6)
#define TMC_RAMP_STOP_R		BIT(5)
#define TMC_RAMP_STOP_L		BIT(4)
#define TMC_RAMP_LATCH_R	BIT(3)
#define TMC_RAMP_LATCH_L	BIT(2)
#define TMC_RAMP_ST_STOP_R	BIT(1)
#define TMC_RAMP_ST_STOP_L	BIT(0)
#define TMC_STOPS_MASK		GENMASK(1, 0)
#define TMC_RAMP_CLEAR_FLAGS	(TMC_RAMP_SECOND_MOVE | TMC_RAMP_POS_REACHED | \
		TMC_RAMP_STOP_SG | TMC_RAMP_LATCH_R | TMC_RAMP_LATCH_L)
#define TMC_REG_XLATCH		0x36
/* Encoder registers */
#define TMC_REG_ENCMODE		0x38
#define TMC_REG_X_ENC		0x39
#define TMC_REG_ENC_CONST	0x3a
#define TMC_REG_ENC_STATUS	0x3b
#define TMC_REG_ENC_LATCH	0x3c
#define TMC_REG_ENC_DEVIATION	0x3d
#define TMC_REG_VIRTUAL_STOP_L	0x3e
#define TMC_REG_VIRTUAL_STOP_R	0x3f
/* ADC Registers */
#define TMC_REG_ADC_VSUPLLY_AIN	0x50
#define TMC_REG_ADC_TEMP	0x51
#define TMC_REG_OTW_OV_VTH	0x52
/* Motoro Driver Registers */
#define TMC_REG_MSLUT(X)	(0x60+X)
#define TMC_REG_MSLUTSEL	0x68
#define TMC_REG_MSLUTSTART	0x69
#define TMC_REG_MSCNT		0x6a
#define TMC_REG_MSCURACT	0x6b
#define TMC_REG_CHOPCONF	0x6c
#define TMC_CHOP_DISS2VS	BIT(31)
#define TMC_CHOP_DISS2G		BIT(30)
#define TMC_CHOP_INTPOL		BIT(28)
#define TMC_CHOP_MRES		GENMASK(27, 24)
#define TMC_CHOP_TPFP		GENMASK(23, 20)
#define TMC_CHOP_VHIGHCHM	BIT(19)
#define TMC_CHOP_VHIGHFS	BIT(18)
#define TMC_CHOP_TBL		GENMASK(16, 15)
#define TMC_CHOP_CHM		BIT(14)
#define TMC_CHOP_DISFDCC	BIT(12)
#define TMC_CHOP_FD3		BIT(11)
#define TMC_CHOP_HEND_OFFSET	GENMASK(10, 7)
#define TMC_CHOP_HSTRT_TFD210	GENMASK(6, 4)
#define TMC_CHOP_TOFF		GENMASK(3, 0)
#define TMC_REG_COOLCONF	0x6d
#define TMC_REG_DCCTRL		0x6e
#define TMC_REG_DRV_STATUS	0x6f
#define TMC_REG_PWMCONF		0x70
#define TMC_REG_PWM_SCALE	0x71
#define TMC_REG_PWM_AUTO	0x72
#define TMC_REG_SG4_THRS	0x74
#define TMC_REG_SG4_RESULT	0x75
#define TMC_REG_SG4_IND		0x76

/*
 * Internal triggers: REFL, REFR, VIRTL, VIRTR and X_COMPARE
 * FIXME: For now X_COMPARE is not yet supported
 */
enum {
	TMC_STOP_REFL = 0,
	TMC_STOP_REFR,
	TMC_STOP_VIRTL,
	TMC_STOP_VIRTR,
	TMC_MAX_INTERNAL_STOPS
};

struct tmc_spi_buf {
	u8 buf[5];
} ____cacheline_aligned;

struct tmc5240 {
	struct spi_device *spi;
	struct motion_device mdev;
	struct spi_transfer wxfer;
	struct spi_transfer rxfers[2];
	struct spi_message wmsg;
	struct spi_message rmsg;
	struct tmc_spi_buf rtxbuf[2]; /* read buffers contain 2 40bit transfers */
	struct tmc_spi_buf rrxbuf[2];
	struct tmc_spi_buf wtxbuf;
	struct tmc_spi_buf wrxbuf;
	struct regmap *map;
	u8 spi_status;
	struct clk *clk;
	u32 clk_rate;
	bool use_encoder;
	int current_index;
	s32 next_dist;
	unsigned int next_index;
	s32 int_trig_dist;
	unsigned int int_trg_index;
	s32 ext_trig_dist;
	unsigned int ext_trg_index;
	ktime_t irq_ts;
	unsigned int stop_functions[TMC_MAX_INTERNAL_STOPS];
};

static int tmc5240_read_reg(void *ctx, unsigned int reg, unsigned int *val)
{
	struct tmc5240 *tmc = (struct tmc5240 *)ctx;
	int ret;
	u8 *rxb0 = &tmc->rrxbuf[0].buf[0];
	u8 *rxb1 = &tmc->rrxbuf[1].buf[0];
	u8 *txb0 = &tmc->rtxbuf[0].buf[0];
	u8 *txb1 = &tmc->rtxbuf[1].buf[0];

	txb0[0] = reg;
	txb1[0] = reg;
	ret = spi_sync(tmc->spi, &tmc->rmsg);
	if (ret)
		return ret;
	*val = get_unaligned_be32(&rxb1[1]);
	tmc->spi_status = rxb0[0];

	return 0;
}

static int tmc5240_write_reg(void *ctx, unsigned int reg, unsigned int val)
{
	struct tmc5240 *tmc = (struct tmc5240 *)ctx;
	u8 *txb = &tmc->wtxbuf.buf[0];

	txb[0] = reg | 0x80;
	put_unaligned_be32(val, &txb[1]);

	return spi_sync(tmc->spi, &tmc->wmsg);
}

/* regmap wrappers */
static int tmc5240_read(struct tmc5240 *tmc, unsigned int reg, unsigned int *val)
{
	int ret = regmap_read(tmc->map, reg, val);

	if (ret)
		dev_err_ratelimited(tmc->mdev.dev,
				"Regmap read error %d at reg: %04x.\n",
				ret, reg);
	return ret;
}

static int tmc5240_write(struct tmc5240 *tmc, unsigned int reg, unsigned int val)
{
	int ret = regmap_write(tmc->map, reg, val);

	if (ret)
		dev_err_ratelimited(tmc->mdev.dev,
				"Regmap write error %d at reg: %04x.\n",
				ret, reg);
	return ret;
}

static int tmc5240_update_bits(struct tmc5240 *tmc, unsigned int reg,
		unsigned int mask, unsigned int val)
{
	int ret = regmap_update_bits(tmc->map, reg, mask, val);

	if (ret)
		dev_err_ratelimited(tmc->mdev.dev,
				"Regmap update bits error %d at reg: %04x.\n",
				ret, reg);
	return ret;
}

/* FIXME: Maybe make this into MOTION_ATTR_XX() macro's? */
#define _tmc_attr_show(_name, _reg, _mask) \
static ssize_t _name##_show(struct device *dev, \
				    struct device_attribute *dev_attr, \
				    char *buf) \
{ \
	struct motion_device *mdev = dev_get_drvdata(dev); \
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev); \
	unsigned int val; \
	ssize_t err; \
	err = tmc5240_read(tmc, _reg, &val); \
	if (err) \
		return err; \
	/* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI*) */ \
	return sprintf(buf, "%lu\n", FIELD_GET(_mask, val)); \
}

#define _tmc_attr_store(_name, _reg, _mask) \
static ssize_t _name##_store(struct device *dev, \
				     struct device_attribute *dev_attr, \
				     const char *buf, size_t len) \
{ \
	struct motion_device *mdev = dev_get_drvdata(dev); \
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev); \
	unsigned long val; \
	ssize_t err; \
	err = kstrtoul(buf, 10, &val); \
	if (err) \
		return err; \
	if (!FIELD_FIT(_mask, val)) \
		return -EINVAL; \
	mutex_lock(&mdev->mutex); \
	err = tmc5240_update_bits(tmc, _reg, _mask, FIELD_PREP(_mask, val)); \
	mutex_unlock(&mdev->mutex); \
	if (err) \
		return err; \
	return len; \
}

#define TMC_ATTR_RW(_name, _reg, _mask) \
	_tmc_attr_show(_name, _reg, _mask) \
	_tmc_attr_store(_name, _reg, _mask) \
	static DEVICE_ATTR_RW(_name)

#define TMC_ATTR_RO(_name, _reg, _mask) \
	_tmc_attr_show(_name, _reg, _mask) \
	static DEVICE_ATTR_RO(_name)

TMC_ATTR_RW(sw_mode_swap_lr, TMC_REG_SW_MODE, TMC_SW_SWAP_LR);
TMC_ATTR_RW(drv_conf_current_range, TMC_REG_DRV_CONF, TMC_DRV_CURRENT_RANGE);
TMC_ATTR_RW(drv_conf_slope_control, TMC_REG_DRV_CONF, TMC_DRV_SLOPE_CONTROL);
TMC_ATTR_RW(global_scaler, TMC_REG_GLOBAL_SCALER, GENMASK(7, 0));
TMC_ATTR_RW(t_power_down, TMC_REG_TPOWERDOWN, GENMASK(7, 0));
TMC_ATTR_RO(t_step, TMC_REG_TSTEP, GENMASK(19, 0));
TMC_ATTR_RW(t_pwm_threshold, TMC_REG_TPWMTHRS, GENMASK(19, 0));
TMC_ATTR_RW(t_cool_threshold, TMC_REG_TCOOLTHRS, GENMASK(19, 0));
TMC_ATTR_RW(t_high, TMC_REG_THIGH, GENMASK(19, 0));
TMC_ATTR_RW(irun_delay, TMC_REG_IHOLD_IRUN, TMC_IHOLD_IRUNDELAY);
TMC_ATTR_RW(ihold_delay, TMC_REG_IHOLD_IRUN, TMC_IHOLD_IHOLDDELAY);
TMC_ATTR_RW(irun, TMC_REG_IHOLD_IRUN, TMC_IHOLD_IRUN);
TMC_ATTR_RW(ihold, TMC_REG_IHOLD_IRUN, TMC_IHOLD_IHOLD);
TMC_ATTR_RO(x_latch, TMC_REG_XLATCH, GENMASK(31, 0));
TMC_ATTR_RW(x_encoder, TMC_REG_X_ENC, GENMASK(31, 0));
TMC_ATTR_RW(encoder_constant, TMC_REG_ENC_CONST, GENMASK(31, 0));
TMC_ATTR_RO(encoder_latch, TMC_REG_ENC_LATCH, GENMASK(31, 0));
TMC_ATTR_RO(x_actual, TMC_REG_XACTUAL, GENMASK(31, 0));
TMC_ATTR_RW(virtual_stop_l, TMC_REG_VIRTUAL_STOP_L, GENMASK(31, 0));
TMC_ATTR_RW(virtual_stop_r, TMC_REG_VIRTUAL_STOP_R, GENMASK(31, 0));

static struct attribute *tmc5240_attributes[] = {
	&dev_attr_sw_mode_swap_lr.attr,
	&dev_attr_drv_conf_current_range.attr,
	&dev_attr_drv_conf_slope_control.attr,
	&dev_attr_global_scaler.attr,
	&dev_attr_t_power_down.attr,
	&dev_attr_t_step.attr,
	&dev_attr_t_pwm_threshold.attr,
	&dev_attr_t_cool_threshold.attr,
	&dev_attr_t_high.attr,
	&dev_attr_irun_delay.attr,
	&dev_attr_ihold_delay.attr,
	&dev_attr_irun.attr,
	&dev_attr_ihold.attr,
	&dev_attr_x_latch.attr,
	&dev_attr_x_encoder.attr,
	&dev_attr_encoder_constant.attr,
	&dev_attr_encoder_latch.attr,
	&dev_attr_x_actual.attr,
	&dev_attr_virtual_stop_l.attr,
	&dev_attr_virtual_stop_r.attr,
	NULL
};

static const struct attribute_group tmc5240_group = {
	.attrs = tmc5240_attributes,
};

static irqreturn_t tmc5240_hard_irq(int irq, void *handle)
{
	struct tmc5240 *tmc = handle;

	dev_info(tmc->mdev.dev, "Hard IRQ spistat = 0x%02x\n", tmc->spi_status);
	tmc->irq_ts = ktime_get();

	return IRQ_WAKE_THREAD;
}

static int tmc5240_motion_with_index(struct motion_device *mdev,
		unsigned int index, s32 d);

static irqreturn_t tmc5240_irq(int irq, void *handle)
{
	struct tmc5240 *tmc = handle;
	struct motion_device *mdev = &tmc->mdev;
	struct mot_event evt = {0};
	u32 val;

	evt.timestamp = ktime2mot_time(tmc->irq_ts);
	tmc5240_read(tmc, TMC_REG_RAMP_STAT, &val);
	tmc5240_read(tmc, TMC_REG_XACTUAL, &evt.position);
	tmc5240_read(tmc, TMC_REG_VACTUAL, &evt.speed);

	if (val & (TMC_RAMP_STOP_L | TMC_RAMP_STOP_R)) {
		if ((val & TMC_RAMP_VIRT_L) &&
				(tmc->stop_functions[TMC_STOP_VIRTL]))
			evt.input_index = TMC_STOP_VIRTL;
		else if ((val & TMC_RAMP_VIRT_R) &&
				(tmc->stop_functions[TMC_STOP_VIRTR]))
			evt.input_index = TMC_STOP_VIRTR;
		else
			evt.input_index = (val & TMC_RAMP_STOP_R) ?
				TMC_STOP_REFR : TMC_STOP_REFL;
		evt.event = MOT_EVENT_STOP;

		/* Check if we have a trigger motion waiting */
		mutex_lock(&mdev->mutex);
		if (tmc->int_trig_dist) {
			tmc5240_motion_with_index(mdev, tmc->int_trg_index,
					tmc->int_trig_dist);
			tmc->int_trig_dist = 0;
		}
		motion_report_event(mdev, &evt);
		mutex_unlock(&mdev->mutex);
	} else if (val & TMC_RAMP_POS_REACHED) {
		evt.event = MOT_EVENT_TARGET;

		/* Check if we have a next motion waiting */
		mutex_lock(&mdev->mutex);
		if (tmc->next_dist) {
			tmc5240_motion_with_index(mdev, tmc->next_index,
					tmc->next_dist);
			tmc->next_dist = 0;
		}
		motion_report_event(mdev, &evt);
		mutex_unlock(&mdev->mutex);
	} else {
		dev_info(tmc->mdev.dev,
			"Unknown IRQ source 0x%04x. SPI status = 0x%02x\n",
			val, tmc->spi_status);
	}

	/* Clear interrupt flags */
	tmc5240_write(tmc, TMC_REG_RAMP_STAT, val & TMC_RAMP_CLEAR_FLAGS);

	return IRQ_HANDLED;
}

static void tmc5240_prepare_message(struct tmc5240 *tmc)
{
	/*
	 * TMC5240 sends back read register contents in the _next_ transfer,
	 * so for read actions we need to do 2 transfers with a CS change in
	 * between.
	 */
	tmc->rxfers[0].tx_buf = &tmc->rtxbuf[0].buf[0];
	tmc->rxfers[0].rx_buf = &tmc->rrxbuf[0].buf[0];
	tmc->rxfers[0].len = 5;
	tmc->rxfers[0].cs_change = 1;
	tmc->rxfers[1].tx_buf = &tmc->rtxbuf[1].buf[0];
	tmc->rxfers[1].rx_buf = &tmc->rrxbuf[1].buf[0];
	tmc->rxfers[1].len = 5;
	spi_message_init_with_transfers(&tmc->rmsg, &tmc->rxfers[0], 2);

	/*
	 * For write register operations, a single transfer is sufficient.
	 * We do not re-use the same buffers, in order to avoid clobbering
	 * bytes 1-4 of the _read_ TX buffer, so we don't need to clear it
	 * on each _read_ transfer.
	 */
	tmc->wxfer.tx_buf = &tmc->wtxbuf.buf[0];
	tmc->wxfer.rx_buf = &tmc->wrxbuf.buf[0];
	tmc->wxfer.len = 5;
	spi_message_init_with_transfers(&tmc->wmsg, &tmc->wxfer, 1);
}

static int tmc5240_ramp_set_vel(struct tmc5240 *tmc, u32 vstart, u32 v1, u32 v2,
		u32 vmax, u32 vstop)
{
	int ret;

	ret = tmc5240_write(tmc, TMC_REG_VSTART, vstart);
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_V1, v1);
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_V2, v2);
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_VMAX, vmax);
	if (ret)
		return ret;
	return tmc5240_write(tmc, TMC_REG_VSTOP, max(vstop, TMC_VSTOP_MIN));
}

static int tmc5240_ramp_set_acc(struct tmc5240 *tmc, u32 a0, u32 a1, u32 a2,
		u32 a3, u32 a4, u32 a5)
{
	int ret;

	ret = tmc5240_write(tmc, TMC_REG_A1, a0);
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_A2, a1);
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_AMAX, a2);
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_DMAX, a3);
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_D2, max(a4, TMC_D2_MIN));
	if (ret)
		return ret;
	return tmc5240_write(tmc, TMC_REG_D1, max(a5, TMC_D1_MIN));
}

static int tmc5240_set_ramp_simple(struct tmc5240 *tmc, u32 vmax, u32 amax)
{
	int ret;

	/*
	 * Setup the most simple profile with only one acceleration and
	 * one speed value. Meant to be used in basic movements without
	 * profile support.
	 */
	ret = tmc5240_ramp_set_vel(tmc, 0, 0, 0, vmax, 0);
	if (ret)
		return ret;
	ret = tmc5240_ramp_set_acc(tmc, 0, 0, amax, amax, 0, 0);

	return ret;
}

static int tmc5240_open(struct motion_device *mdev)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	u32 val;
	int ret;

	tmc->next_dist = 0;
	tmc->int_trig_dist = 0;
	tmc->ext_trig_dist = 0;

	ret = tmc5240_read(tmc, TMC_REG_GSTAT, &val);
	if (ret)
		return ret;
	/* Clear reset condition */
	ret = tmc5240_write(tmc, TMC_REG_GSTAT, val);
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_GCONF, 0x00000000);
	if (ret)
		return ret;
	ret = tmc5240_update_bits(tmc, TMC_REG_CHOPCONF, TMC_CHOP_TOFF,
			FIELD_PREP(TMC_CHOP_TOFF, 3));
	if (ret)
		return ret;
	ret = tmc5240_read(tmc, TMC_REG_SW_MODE, &val);
	if (ret)
		return ret;
	if (tmc->use_encoder)
		val |= TMC_SW_VIRT_STOP_ENC;
	else
		val &= ~TMC_SW_VIRT_STOP_ENC;
	return tmc5240_write(tmc, TMC_REG_SW_MODE, val);
}

static int tmc5240_release(struct motion_device *mdev)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	u32 val;

	tmc->current_index = -1;

	/* Turn off RAMP and chopper */
	tmc5240_write(tmc, TMC_REG_RAMPMODE, TMC_RAMPMODE_POS_VEL);
	tmc5240_write(tmc, TMC_REG_VMAX, 0);
	tmc5240_update_bits(tmc, TMC_REG_CHOPCONF, TMC_CHOP_TOFF,
			FIELD_PREP(TMC_CHOP_TOFF, 0));

	/* Disable event interrupts sources */
	tmc5240_read(tmc, TMC_REG_SW_MODE, &val);
	val &= ~(TMC_SW_EN_VIRT_STOP_L | TMC_SW_EN_VIRT_STOP_R |
			TMC_SW_STOP_L_ENABLE | TMC_SW_STOP_R_ENABLE);
	return tmc5240_write(tmc, TMC_REG_SW_MODE, val);
}

static int _sign_extend(u32 val, u8 sbit)
{
	return ((int)(val << (31 - sbit)) >> (31 - sbit));
}

static int tmc5240_get_status(struct motion_device *mdev, struct mot_status *st)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	u32 val;
	int ret;

	ret = tmc5240_read(tmc, TMC_REG_XACTUAL, &val);
	if (ret)
		return ret;
	st->position = val;
	ret = tmc5240_read(tmc, TMC_REG_VACTUAL, &val);
	if (ret)
		return ret;
	st->speed = _sign_extend(val, TMC_VACTUAL_SIGN_BIT);
	ret = tmc5240_read(tmc, TMC_REG_RAMP_STAT, &val);
	if (ret)
		return ret;
	st->local_inputs = (FIELD_GET(TMC_VIRT_STOPS_MASK, val) << 2) |
		FIELD_GET(TMC_STOPS_MASK, val);

	return 0;
}

static int tmc5240_basic_run(struct motion_device *mdev, u32 ch, s32 v)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	int ret;

	tmc->current_index = -1; /* Invalidate current index */
	if (v >= 0) {
		ret = tmc5240_write(tmc, TMC_REG_RAMPMODE, TMC_RAMPMODE_POS_VEL);
		if (ret)
			return ret;
	} else {
		v = -v;
		ret = tmc5240_write(tmc, TMC_REG_RAMPMODE, TMC_RAMPMODE_NEG_VEL);
		if (ret)
			return ret;
	}
	ret = tmc5240_write(tmc, TMC_REG_AMAX, TMC_MAX_ACCELERATION / 2);
	if (ret)
		return ret;
	return tmc5240_write(tmc, TMC_REG_VMAX, v);
}

static int tmc5240_basic_stop(struct motion_device *mdev, channel_mask_t ch)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);

	tmc5240_write(tmc, TMC_REG_RAMPMODE, TMC_RAMPMODE_POS_VEL);
	tmc5240_write(tmc, TMC_REG_AMAX, TMC_MAX_ACCELERATION / 2);
	tmc->current_index = -1; /* Invalidate current index */

	return tmc5240_write(tmc, TMC_REG_VMAX, 0);
}

static int tmc5240_set_distance(struct tmc5240 *tmc, s32 d)
{
	u32 x;
	int ret;

	ret = tmc5240_write(tmc, TMC_REG_RAMPMODE, TMC_RAMPMODE_POSITION);
	if (ret)
		return ret;

	/* Clear position reached */
	ret = tmc5240_write(tmc, TMC_REG_RAMP_STAT, TMC_RAMP_POS_REACHED);
	if (ret)
		return ret;
	ret = tmc5240_read(tmc, TMC_REG_XACTUAL, &x);
	if (ret)
		return ret;
	return tmc5240_write(tmc, TMC_REG_XTARGET, x + d);
}

static int tmc5240_move_distance(struct motion_device *mdev, u32 ch, s32 v, u32 d)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	int ret;

	ret = tmc5240_set_ramp_simple(tmc, v, 1<<17);
	if (ret)
		return ret;
	return tmc5240_set_distance(tmc, d);
}

static int tmc5240_move_timed(struct motion_device *mdev, u32 ch, s32 v, ktime_t t)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);

	dev_info(mdev->dev, "timed ch: 0x%08x: v: %d, duration: %llu\n", ch, v, t);
	return 0;
}

#define _check_u(v, n) ((v < 0) || (v >= (1 << n)))

static int tmc5240_validate_profile(struct motion_device *mdev,
		struct mot_profile *p)
{
	speed_raw_t *v = &p->vel[0];
	accel_raw_t *a = &p->acc[0];

	if (_check_u(v[0], 18) || _check_u(a[0], 18) || _check_u(a[1], 18))
		return -EINVAL;

	/* Trapezoidal profile */
	if ((p->na == 2) && (p->nv == 3)) {
		if (_check_u(v[1], 23) || _check_u(v[2], 18))
			return -EINVAL;
		v[2] = max(v[2], TMC_VSTOP_MIN); /* VSTOP (see DS), can't be 0 */
		return 0;
	}

	if (_check_u(v[1], 20) || _check_u(a[2], 18) || _check_u(a[3], 18))
		return -EINVAL;

	/* Dual slope profile */
	if ((p->na == 4) && (p->nv == 4)) {
		if (_check_u(v[2], 23) || _check_u(v[3], 18))
			return -EINVAL;
		a[3] = max(a[3], TMC_D1_MIN); /* D1 (see DS), can't be 0 */
		v[3] = max(v[3], TMC_VSTOP_MIN); /* VSTOP (see DS), can't be 0 */
		return 0;

	}

	/* S-curve (8-point) profile */
	if ((p->na == 6) && (p->nv == 5)) {
		if (_check_u(v[2], 20) || _check_u(v[3], 23) || _check_u(v[4], 18))
			return -EINVAL;
		if (_check_u(a[4], 18) || _check_u(a[5], 18))
			return -EINVAL;
		a[4] = max(a[4], TMC_D2_MIN); /* D2 (see DS), can't be 0 */
		a[5] = max(a[5], TMC_D1_MIN); /* D1 (see DS), can't be 0 */
		v[4] = max(v[4], TMC_VSTOP_MIN); /* VSTOP (see DS), can't be 0 */
		return 0;
	}

	return -EINVAL;
}

static inline u32 tmc5240_time2tv(struct tmc5240 *tmc, mot_time_t t)
{
	/* TMC5240 time values are in 512 clock cycle units */
	return (u32)div_u64((t >> 9) * tmc->clk_rate, NSEC_PER_SEC);
}

static int tmc5240_set_profile(struct motion_device *mdev, struct mot_profile *p)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	speed_raw_t *v = &p->vel[0];
	accel_raw_t *a = &p->acc[0];
	int ret;

	ret = tmc5240_write(tmc, TMC_REG_TVMAX,
			tmc5240_time2tv(tmc, p->tvmax));
	if (ret)
		return ret;
	ret = tmc5240_write(tmc, TMC_REG_TZEROWAIT,
			tmc5240_time2tv(tmc, p->tvzero));
	if (ret)
		return ret;

	/* Trapezoidal profile */
	if ((p->na == 2) && (p->nv == 3)) {
		ret = tmc5240_ramp_set_vel(tmc, v[0], 0, 0, v[1], v[2]);
		if (ret)
			return ret;
		return tmc5240_ramp_set_acc(tmc, 0, 0, a[0], a[1], 0, 0);
	}

	/* Dual slope profile */
	if ((p->na == 4) && (p->nv == 4)) {
		ret = tmc5240_ramp_set_vel(tmc, v[0], v[1], 0, v[2], v[3]);
		if (ret)
			return ret;
		return tmc5240_ramp_set_acc(tmc, a[0], 0, a[1], a[2], 0, a[3]);
	}

	/* S-curve (8-point) profile */
	if ((p->na == 6) && (p->nv == 5)) {
		tmc5240_ramp_set_vel(tmc, v[0], v[1], v[2], v[3], v[4]);
		if (ret)
			return ret;
		return tmc5240_ramp_set_acc(tmc, a[0], a[1], a[2], a[3], a[4],
				a[5]);
	}

	/* We should never get here, since the validate handler
	 * is supposed to have been called on this profile before.
	 */
	WARN_ONCE(1, "motion profile should be valid, but isn't.");

	return -EINVAL;
}

static int tmc5240_motion_with_index(struct motion_device *mdev,
		unsigned int index, s32 d)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	int ret;

	if (tmc->current_index != index) {
		ret = tmc5240_set_profile(mdev, &mdev->profiles[index]);
		if (ret)
			return ret;
	}
	tmc->current_index = index;

	return tmc5240_set_distance(tmc, d);
}

static int tmc5240_motion_distance(struct motion_device *mdev,
		unsigned int channel, unsigned int index, s32 d, unsigned int when)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	int ret = 0;

	switch (when) {
	case MOT_WHEN_NEXT:
		if (tmc->next_dist) {
			ret = -EAGAIN;
		} else {
			tmc->next_dist = d;
			tmc->next_index = index;
		}
		break;
	case MOT_WHEN_EXT_TRIGGER:
		if (tmc->ext_trig_dist) {
			ret = -EAGAIN;
		} else {
			tmc->ext_trig_dist = d;
			tmc->ext_trg_index = index;
		}
		break;
	case MOT_WHEN_INT_TRIGGER:
		if (tmc->int_trig_dist) {
			ret = -EAGAIN;
		} else {
			tmc->int_trig_dist = d;
			tmc->int_trg_index = index;
		}
		break;
	case MOT_WHEN_IMMEDIATE:
		tmc5240_motion_with_index(mdev, index, d);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int tmc5240_motion_stop(struct motion_device *mdev, channel_mask_t chmsk,
		unsigned int when)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);

	if (when != MOT_WHEN_IMMEDIATE)
		return -EINVAL;
	/*
	 * See DS page 55 "Early Ramp Termination".
	 * The correct way to stop using all *decelaration* parameters is
	 * weirdly complex and not practial. Best thing we can do here, is to
	 * use the second closest option which uses AMAX, A1 and A2:
	 */
	tmc->current_index = -1; /* Invalidate current index */
	tmc5240_write(tmc, TMC_REG_VSTART, 0);

	return tmc5240_write(tmc, TMC_REG_VMAX, 0);

}

static int tmc5240_config_trigger(struct motion_device *mdev, unsigned int index,
		unsigned int func, unsigned int edge, channel_mask_t ch)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);
	u32 val;
	int idxbit;
	int ret;

	ret = tmc5240_read(tmc, TMC_REG_SW_MODE, &val);
	if (ret)
		return ret;

	switch (index) {
	case TMC_STOP_REFL:
		idxbit = TMC_SW_STOP_L_ENABLE;
		if (edge == MOT_EDGE_FALLING)
			val |= TMC_SW_STOP_L_POL;
		else
			val &= ~TMC_SW_STOP_L_POL;
		break;
	case TMC_STOP_REFR:
		idxbit = TMC_SW_STOP_R_ENABLE;
		if (edge == MOT_EDGE_FALLING)
			val |= TMC_SW_STOP_R_POL;
		else
			val &= ~TMC_SW_STOP_R_POL;
		break;
	case TMC_STOP_VIRTL:
		idxbit = TMC_SW_EN_VIRT_STOP_L;
		break;
	case TMC_STOP_VIRTR:
		idxbit = TMC_SW_EN_VIRT_STOP_R;
		break;
	default:
		/* Should never occur */
		return -EINVAL;
	}

	/* Store stop function for interrupt handler */
	tmc->stop_functions[index] = func;

	switch (func) {
	case MOT_INP_FUNC_NONE:
		val &= ~idxbit;
		break;
	case MOT_INP_FUNC_DECEL:
	case MOT_INP_FUNC_DECEL_NEG:
	case MOT_INP_FUNC_DECEL_POS:
		val |= TMC_SW_EN_SOFTSTOP;
		val |= idxbit;
		break;
	case MOT_INP_FUNC_STOP:
	case MOT_INP_FUNC_STOP_NEG:
	case MOT_INP_FUNC_STOP_POS:
		val &= ~TMC_SW_EN_SOFTSTOP;
		val |= idxbit;
		break;
	default:
		return -EINVAL;
	}
	return tmc5240_write(tmc, TMC_REG_SW_MODE, val);
}

static void tmc5240_ext_trigger(struct motion_device *mdev, unsigned int index,
			channel_mask_t ch)
{
	struct tmc5240 *tmc = container_of(mdev, struct tmc5240, mdev);

	if (tmc->ext_trig_dist) {
		tmc5240_motion_with_index(mdev, tmc->ext_trg_index,
				tmc->ext_trig_dist);
		tmc->ext_trig_dist = 0;
	}
}

static const struct motion_ops tmc5240_motion_ops = {
	.device_open = tmc5240_open,
	.device_release = tmc5240_release,
	.get_status = tmc5240_get_status,
	.basic_run = tmc5240_basic_run,
	.basic_stop = tmc5240_basic_stop,
	.move_timed = tmc5240_move_timed,
	.move_distance = tmc5240_move_distance,
	.validate_profile = tmc5240_validate_profile,
	.set_profile = tmc5240_set_profile,
	.motion_distance = tmc5240_motion_distance,
	.motion_stop = tmc5240_motion_stop,
	.config_trigger = tmc5240_config_trigger,
	.external_trigger = tmc5240_ext_trigger
};

static const struct regmap_range tmc_readable_reg_ranges[] = {
	regmap_reg_range(TMC_REG_GCONF, TMC_REG_X_COMPARE_REP),
	regmap_reg_range(TMC_REG_DRV_CONF, TMC_REG_GLOBAL_SCALER),
	regmap_reg_range(TMC_REG_IHOLD_IRUN, TMC_REG_THIGH),
	regmap_reg_range(TMC_REG_RAMPMODE, TMC_REG_D2),
	regmap_reg_range(TMC_REG_VDCMIN, TMC_REG_XLATCH),
	regmap_reg_range(TMC_REG_ENCMODE, TMC_REG_VIRTUAL_STOP_R),
	regmap_reg_range(TMC_REG_ADC_VSUPLLY_AIN, TMC_REG_OTW_OV_VTH),
	regmap_reg_range(TMC_REG_MSLUT(0), TMC_REG_PWM_AUTO),
	regmap_reg_range(TMC_REG_SG4_THRS, TMC_REG_SG4_IND),
};

static const struct regmap_access_table tmc_regmap_readable_regs = {
	.yes_ranges = tmc_readable_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(tmc_readable_reg_ranges),
};

static const struct regmap_range tmc_readonly_reg_ranges[] = {
	regmap_reg_range(TMC_REG_IFCNT, TMC_REG_IFCNT),
	regmap_reg_range(TMC_REG_IOIN, TMC_REG_IOIN),
	regmap_reg_range(TMC_REG_TSTEP, TMC_REG_TSTEP),
	regmap_reg_range(TMC_REG_VACTUAL, TMC_REG_VACTUAL),
	regmap_reg_range(TMC_REG_XLATCH, TMC_REG_XLATCH),
	regmap_reg_range(TMC_REG_ENC_LATCH, TMC_REG_ENC_LATCH),
	regmap_reg_range(TMC_REG_ADC_VSUPLLY_AIN, TMC_REG_ADC_TEMP),
	regmap_reg_range(TMC_REG_MSCNT, TMC_REG_MSCURACT),
	regmap_reg_range(TMC_REG_DRV_STATUS, TMC_REG_DRV_STATUS),
	regmap_reg_range(TMC_REG_PWM_SCALE, TMC_REG_PWM_AUTO),
	regmap_reg_range(TMC_REG_SG4_RESULT, TMC_REG_SG4_IND),
};

static const struct regmap_access_table tmc_regmap_writeable_regs = {
	.no_ranges = tmc_readonly_reg_ranges,
	.n_no_ranges = ARRAY_SIZE(tmc_readonly_reg_ranges),
};

static const struct regmap_range tmc_non_volatile_reg_ranges[] = {
	regmap_reg_range(TMC_REG_GCONF, TMC_REG_GCONF),
	regmap_reg_range(TMC_REG_NODECONF, TMC_REG_NODECONF),
	regmap_reg_range(TMC_REG_X_COMPARE, TMC_REG_TPOWERDOWN),
	regmap_reg_range(TMC_REG_TPWMTHRS, TMC_REG_RAMPMODE),
	regmap_reg_range(TMC_REG_VSTART, TMC_REG_SW_MODE),
	regmap_reg_range(TMC_REG_ENCMODE, TMC_REG_ENCMODE),
	regmap_reg_range(TMC_REG_ENC_CONST, TMC_REG_ENC_CONST),
	regmap_reg_range(TMC_REG_ENC_DEVIATION, TMC_REG_VIRTUAL_STOP_R),
	regmap_reg_range(TMC_REG_OTW_OV_VTH, TMC_REG_MSLUTSTART),
	regmap_reg_range(TMC_REG_CHOPCONF, TMC_REG_DCCTRL),
	regmap_reg_range(TMC_REG_PWMCONF, TMC_REG_PWMCONF),
	regmap_reg_range(TMC_REG_SG4_THRS, TMC_REG_SG4_THRS),
};

static const struct regmap_access_table tmc_regmap_volatile_regs = {
	.no_ranges = tmc_non_volatile_reg_ranges,
	.n_no_ranges = ARRAY_SIZE(tmc_non_volatile_reg_ranges),
};

static const struct regmap_config mapcfg = {
	.reg_bits = 8,
	.val_bits = 32,
	.max_register = TMC_REG_SG4_IND,
	.write_flag_mask = BIT(7),
	.reg_read = tmc5240_read_reg,
	.reg_write = tmc5240_write_reg,
	.use_single_read = true,
	.use_single_write = true,
	.rd_table = &tmc_regmap_readable_regs,
	.wr_table = &tmc_regmap_writeable_regs,
	.volatile_table = &tmc_regmap_volatile_regs,
	// .cache_type = REGCACHE_MAPLE,
	.cache_type = REGCACHE_NONE,
};

static int tmc5240_probe(struct spi_device *spi)
{
	struct tmc5240 *tmc;
	struct device *dev = &spi->dev;
	unsigned long irq_flags;
	int err;
	u32 val, ver, rev;

	/* FIXME: Limited functionality should be possible without IRQ */
	if (spi->irq <= 0) {
		dev_dbg(dev, "no IRQ?\n");
		return -EINVAL;
	}

	spi->bits_per_word = 8;
	spi->mode &= ~SPI_MODE_X_MASK;
	spi->mode |= SPI_MODE_3;
	err = spi_setup(spi);
	if (err < 0)
		return err;

	tmc = devm_kzalloc(dev, sizeof(struct tmc5240), GFP_KERNEL);
	if (!tmc)
		return -ENOMEM;

	tmc->mdev.parent = dev;
	tmc->mdev.ops = tmc5240_motion_ops;
	tmc->mdev.groups[0] = &tmc5240_group;
	tmc->mdev.capabilities.features = MOT_FEATURE_SPEED |
		MOT_FEATURE_ACCEL | MOT_FEATURE_ENCODER | MOT_FEATURE_PROFILE;
	tmc->mdev.capabilities.num_channels = 1;
	tmc->mdev.capabilities.max_apoints = 6;
	tmc->mdev.capabilities.max_vpoints = 5;
	tmc->mdev.capabilities.num_int_triggers = TMC_MAX_INTERNAL_STOPS;
	tmc->mdev.capabilities.num_ext_triggers = 0; /* Filled in by core */
	tmc->mdev.capabilities.type = MOT_TYPE_STEPPER;
	tmc->mdev.capabilities.subdiv = 256;

	tmc->current_index = -1;

	spi_set_drvdata(spi, tmc);
	tmc->spi = spi;

	tmc->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(tmc->clk)) {
		err = PTR_ERR(tmc->clk);
		dev_err(dev, "clk get failed: %d\n", err);
		return err;
	}

	err = clk_prepare_enable(tmc->clk);
	if (err) {
		dev_err(dev, "clk enable failed: %d\n", err);
		return err;
	}
	tmc->clk_rate = clk_get_rate(tmc->clk);
	if (!tmc->clk_rate) {
		dev_err(dev, "clk rate = 0\n");
		err = -EINVAL;
		goto err_clk_disable;
	}

	/*
	 * See datasheet table 19:
	 * v[Hz] = v[TMC5240] * ( fCLK[Hz] / 2^24 )
	 * a[Hz/s] = a[TMC5240] * ( fCLK[Hz]^2 / 2^42 )
	 * Since for acceleration, numbers are squared and too big for u32,
	 * we need to pre-scale both operands by the same amount.
	 * Clock frequencies are almost always whole kHz values, so we can
	 * divide by 1000 without losing precision. The divider would then
	 * become (1ULL<<42)/1000000, which is (rounded) 4398047.
	 */
	tmc->mdev.capabilities.speed_conv_mul = tmc->clk_rate;
	tmc->mdev.capabilities.speed_conv_div = (1UL << 24);
	tmc->mdev.capabilities.accel_conv_mul = (tmc->clk_rate / 1000); /* Pre-scale */
	tmc->mdev.capabilities.accel_conv_mul *=
		tmc->mdev.capabilities.accel_conv_mul; /* squared */
	tmc->mdev.capabilities.accel_conv_div = 4398047; /* (1 << 42) / 1000000 */

	tmc->map = devm_regmap_init(&spi->dev, NULL, tmc, &mapcfg);
	if (IS_ERR(tmc->map)) {
		err = PTR_ERR(tmc->map);
		goto err_clk_disable;
	}

	tmc5240_prepare_message(tmc);
	err = tmc5240_read(tmc, TMC_REG_IOIN, &val);
	if (err)
		goto err_clk_disable;

	ver = FIELD_GET(TMC_IOIN_VERSION_MASK, val);

	/* Sanity check. Currently 0x40 and 0x41 are known valid versions */
	if ((ver < 0x40) || (ver > 0x5f)) {
		dev_err(dev, "TMC5240 version number invalid (%02x)\n", ver);
		err = -EINVAL;
		goto err_clk_disable;
	}

	rev = FIELD_GET(TMC_IOIN_REVISION_MASK, val);
	dev_info(dev, "TMC5240 version %02x, rev %d, clock %dHz detected.\n",
			ver, rev, tmc->clk_rate);

	irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;

	err = devm_request_threaded_irq(dev, spi->irq,
					tmc5240_hard_irq, tmc5240_irq,
					irq_flags, dev->driver->name, tmc);
	if (err)
		goto err_clk_disable;

	return motion_register_device(&tmc->mdev);
err_clk_disable:
	clk_disable_unprepare(tmc->clk);

	return err;
}

static void tmc5240_remove(struct spi_device *spi)
{
	struct tmc5240 *tmc = spi_get_drvdata(spi);

	clk_disable_unprepare(tmc->clk);
}

static const struct of_device_id tmc5240_of_match[] = {
	{ .compatible = "adi,tmc5240" },
	{}
};
MODULE_DEVICE_TABLE(of, tmc5240_of_match);

static struct spi_driver tmc5240_driver = {
	.driver = {
		.name = "tmc5240",
		.of_match_table = tmc5240_of_match,
	},
	.probe = tmc5240_probe,
	.remove = tmc5240_remove,
};
module_spi_driver(tmc5240_driver);

MODULE_AUTHOR("David Jander <david@protonic.nl>");
MODULE_DESCRIPTION("Analog Devices TMC5240 stepper motor controller driver");
MODULE_LICENSE("GPL");
