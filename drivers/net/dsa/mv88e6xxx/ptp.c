// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Marvell 88E6xxx Switch PTP support
 *
 * Copyright (c) 2008 Marvell Semiconductor
 *
 * Copyright (c) 2017 National Instruments
 *      Erik Hons <erik.hons@ni.com>
 *      Brandon Streiff <brandon.streiff@ni.com>
 *      Dane Wagner <dane.wagner@ni.com>
 */

#include "chip.h"
#include "global1.h"
#include "global2.h"
#include "hwtstamp.h"
#include "ptp.h"

#define MV88E6XXX_MAX_ADJ_PPB	1000000

struct mv88e6xxx_cc_coeffs {
	u32 cc_shift;
	u32 cc_mult;
	u32 cc_mult_num;
	u32 cc_mult_dem;
};

/* Family MV88E6250:
 * Raw timestamps are in units of 10-ns clock periods.
 *
 * clkadj = scaled_ppm * 10*2^28 / (10^6 * 2^16)
 * simplifies to
 * clkadj = scaled_ppm * 2^7 / 5^5
 */
#define MV88E6XXX_CC_10NS_SHIFT 28
static const struct mv88e6xxx_cc_coeffs mv88e6xxx_cc_10ns_coeffs = {
	.cc_shift = MV88E6XXX_CC_10NS_SHIFT,
	.cc_mult = 10 << MV88E6XXX_CC_10NS_SHIFT,
	.cc_mult_num = 1 << 7,
	.cc_mult_dem = 3125ULL,
};

/* Other families except MV88E6393X in internal clock mode:
 * Raw timestamps are in units of 8-ns clock periods.
 *
 * clkadj = scaled_ppm * 8*2^28 / (10^6 * 2^16)
 * simplifies to
 * clkadj = scaled_ppm * 2^9 / 5^6
 */
#define MV88E6XXX_CC_8NS_SHIFT 28
static const struct mv88e6xxx_cc_coeffs mv88e6xxx_cc_8ns_coeffs = {
	.cc_shift = MV88E6XXX_CC_8NS_SHIFT,
	.cc_mult = 8 << MV88E6XXX_CC_8NS_SHIFT,
	.cc_mult_num = 1 << 9,
	.cc_mult_dem = 15625ULL
};

/* Family MV88E6393X using internal clock:
 * Raw timestamps are in units of 4-ns clock periods.
 *
 * clkadj = scaled_ppm * 4*2^28 / (10^6 * 2^16)
 * simplifies to
 * clkadj = scaled_ppm * 2^8 / 5^6
 */
#define MV88E6XXX_CC_4NS_SHIFT 28
static const struct mv88e6xxx_cc_coeffs mv88e6xxx_cc_4ns_coeffs = {
	.cc_shift = MV88E6XXX_CC_4NS_SHIFT,
	.cc_mult = 4 << MV88E6XXX_CC_4NS_SHIFT,
	.cc_mult_num = 1 << 8,
	.cc_mult_dem = 15625ULL
};

#define TAI_EVENT_WORK_INTERVAL msecs_to_jiffies(100)

#define cc_to_chip(cc) container_of(cc, struct mv88e6xxx_chip, tstamp_cc)
#define dw_overflow_to_chip(dw) container_of(dw, struct mv88e6xxx_chip, \
					     overflow_work)
#define dw_tai_event_to_chip(dw) container_of(dw, struct mv88e6xxx_chip, \
					      tai_event_work)

static int mv88e6xxx_tai_read(struct mv88e6xxx_chip *chip, int addr,
			      u16 *data, int len)
{
	if (!chip->info->ops->avb_ops->tai_read)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->tai_read(chip, addr, data, len);
}

/* TODO: places where this are called should be using pinctrl */
static int mv88e6352_set_gpio_func(struct mv88e6xxx_chip *chip, int pin,
				   int func, int input)
{
	int err;

	if (!chip->info->ops->gpio_ops)
		return -EOPNOTSUPP;

	err = chip->info->ops->gpio_ops->set_dir(chip, pin, input);
	if (err)
		return err;

	return chip->info->ops->gpio_ops->set_pctl(chip, pin, func);
}

static const struct mv88e6xxx_cc_coeffs *
mv88e6xxx_cc_coeff_get(struct mv88e6xxx_chip *chip)
{
	u16 period_ps;
	int err;

	err = mv88e6xxx_tai_read(chip, MV88E6XXX_TAI_CLOCK_PERIOD, &period_ps, 1);
	if (err) {
		dev_err(chip->dev, "failed to read cycle counter period: %d\n",
			err);
		return ERR_PTR(err);
	}

	switch (period_ps) {
	case 4000:
		return &mv88e6xxx_cc_4ns_coeffs;
	case 8000:
		return &mv88e6xxx_cc_8ns_coeffs;
	case 10000:
		return &mv88e6xxx_cc_10ns_coeffs;
	default:
		dev_err(chip->dev, "unexpected cycle counter period of %u ps\n",
			period_ps);
		return ERR_PTR(-ENODEV);
	}
}

static u64 mv88e6352_ptp_clock_read(struct mv88e6xxx_chip *chip)
{
	u16 phc_time[2];
	int err;

	err = mv88e6xxx_tai_read(chip, MV88E6XXX_TAI_TIME_LO, phc_time,
				 ARRAY_SIZE(phc_time));
	if (err)
		return 0;
	else
		return ((u32)phc_time[1] << 16) | phc_time[0];
}

static u64 mv88e6165_ptp_clock_read(struct mv88e6xxx_chip *chip)
{
	u16 phc_time[2];
	int err;

	err = mv88e6xxx_tai_read(chip, MV88E6XXX_PTP_GC_TIME_LO, phc_time,
				 ARRAY_SIZE(phc_time));
	if (err)
		return 0;
	else
		return ((u32)phc_time[1] << 16) | phc_time[0];
}

static int mv88e6352_ptp_pin_setup(struct mv88e6xxx_chip *chip,
				   int pin, unsigned int flags, int enable)
{
	int func, err;

	/* Reject requests to enable time stamping on both edges. */
	if (flags & PTP_STRICT_FLAGS &&
	    flags & PTP_ENABLE_FEATURE &&
	    (flags & PTP_EXTTS_EDGES) == PTP_EXTTS_EDGES)
		return -EOPNOTSUPP;

	if (enable)
		func = MV88E6352_G2_SCRATCH_GPIO_PCTL_EVREQ;
	else
		func = MV88E6352_G2_SCRATCH_GPIO_PCTL_GPIO;

	err = mv88e6352_set_gpio_func(chip, pin, func, true);

	return enable ? err : 0;
}

static int mv88e6352_ptp_verify(struct mv88e6xxx_chip *chip, unsigned int pin,
				enum ptp_pin_function func, unsigned int chan)
{
	switch (func) {
	case PTP_PF_NONE:
	case PTP_PF_EXTTS:
		break;
	case PTP_PF_PEROUT:
	case PTP_PF_PHYSYNC:
		return -EOPNOTSUPP;
	}
	return 0;
}

const struct mv88e6xxx_ptp_ops mv88e6165_ptp_ops = {
	.clock_read = mv88e6165_ptp_clock_read,
	.global_enable = mv88e6165_global_enable,
	.global_disable = mv88e6165_global_disable,
	.arr0_sts_reg = MV88E6165_PORT_PTP_ARR0_STS,
	.arr1_sts_reg = MV88E6165_PORT_PTP_ARR1_STS,
	.dep_sts_reg = MV88E6165_PORT_PTP_DEP_STS,
	.rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_DELAY_REQ),
};

const struct mv88e6xxx_ptp_ops mv88e6250_ptp_ops = {
	.clock_read = mv88e6352_ptp_clock_read,
	.ptp_pin_setup = mv88e6352_ptp_pin_setup,
	.ptp_verify = mv88e6352_ptp_verify,
	.port_enable = mv88e6352_hwtstamp_port_enable,
	.port_disable = mv88e6352_hwtstamp_port_disable,
	.n_ext_ts = 1,
	.arr0_sts_reg = MV88E6XXX_PORT_PTP_ARR0_STS,
	.arr1_sts_reg = MV88E6XXX_PORT_PTP_ARR1_STS,
	.dep_sts_reg = MV88E6XXX_PORT_PTP_DEP_STS,
	.rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_DELAY_REQ),
};

const struct mv88e6xxx_ptp_ops mv88e6352_ptp_ops = {
	.clock_read = mv88e6352_ptp_clock_read,
	.ptp_pin_setup = mv88e6352_ptp_pin_setup,
	.ptp_verify = mv88e6352_ptp_verify,
	.port_enable = mv88e6352_hwtstamp_port_enable,
	.port_disable = mv88e6352_hwtstamp_port_disable,
	.n_ext_ts = 1,
	.arr0_sts_reg = MV88E6XXX_PORT_PTP_ARR0_STS,
	.arr1_sts_reg = MV88E6XXX_PORT_PTP_ARR1_STS,
	.dep_sts_reg = MV88E6XXX_PORT_PTP_DEP_STS,
	.rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_DELAY_REQ),
};

const struct mv88e6xxx_ptp_ops mv88e6390_ptp_ops = {
	.clock_read = mv88e6352_ptp_clock_read,
	.ptp_pin_setup = mv88e6352_ptp_pin_setup,
	.ptp_verify = mv88e6352_ptp_verify,
	.port_enable = mv88e6352_hwtstamp_port_enable,
	.port_disable = mv88e6352_hwtstamp_port_disable,
	.set_ptp_cpu_port = mv88e6390_g1_set_ptp_cpu_port,
	.n_ext_ts = 1,
	.arr0_sts_reg = MV88E6XXX_PORT_PTP_ARR0_STS,
	.arr1_sts_reg = MV88E6XXX_PORT_PTP_ARR1_STS,
	.dep_sts_reg = MV88E6XXX_PORT_PTP_DEP_STS,
	.rx_filters = (1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_DELAY_REQ),
};

static int mv88e6xxx_set_ptp_cpu_port(struct mv88e6xxx_chip *chip)
{
	const struct mv88e6xxx_ptp_ops *ptp_ops = chip->info->ops->ptp_ops;
	struct dsa_port *dp;
	int upstream = 0;
	int err;

	dsa_switch_for_each_user_port(dp, chip->ds) {
		upstream = dsa_upstream_port(chip->ds, dp->index);
		break;
	}

	err = ptp_ops->set_ptp_cpu_port(chip, upstream);
	if (err)
		dev_err(chip->dev, "Failed to set PTP CPU destination port!\n");

	return err;
}

static struct mv88e6xxx_chip *dev_to_chip(struct device *dev)
{
	struct dsa_switch *ds = dev_get_drvdata(dev);

	return ds->priv;
}

static int mv88e6xxx_tai_enable(struct device *dev)
{
	return 0;
}

static u64 mv88e6xxx_tai_clock_read(struct device *dev,
				    struct ptp_system_timestamp *sts)
{
	struct mv88e6xxx_chip *chip = dev_to_chip(dev);
	int err = 0;

	if (chip->info->ops->ptp_ops->clock_read) {
		mv88e6xxx_reg_lock(chip);
		ptp_read_system_prets(sts);
		err = chip->info->ops->ptp_ops->clock_read(chip);
		ptp_read_system_postts(sts);
		mv88e6xxx_reg_unlock(chip);
	}

	return err;
}

static int mv88e6xxx_tai_extts_read(struct device *dev, int reg,
				    struct marvell_extts *extts)
{
	struct mv88e6xxx_chip *chip = dev_to_chip(dev);
	u16 regs[3];
	int ret;

	mv88e6xxx_reg_lock(chip);
	ret = chip->info->ops->avb_ops->tai_read(chip, reg, regs, 3);
	if (ret < 0)
		goto unlock;

	extts->status = regs[0];
	extts->time = regs[1] | regs[2] << 16;

	/* Clear valid if set */
	if (regs[0] & MV_STATUS_EVENTCAPVALID) {
		chip->info->ops->avb_ops->tai_write(chip, reg, 0);
		ret = 1;
	} else {
		ret = 0;
	}

unlock:
	mv88e6xxx_reg_unlock(chip);

	return ret;
}

static int mv88e6xxx_tai_pin_verify(struct device *dev, int pin,
				    enum ptp_pin_function func,
				    unsigned int chan)
{
	struct mv88e6xxx_chip *chip = dev_to_chip(dev);

	if (!chip->info->ops->ptp_ops->ptp_verify)
		return -EOPNOTSUPP;

	return chip->info->ops->ptp_ops->ptp_verify(chip, pin, func, chan);
}

static int mv88e6xxx_tai_pin_setup(struct device *dev, int pin,
				   unsigned int flags, int enable)
{
	struct mv88e6xxx_chip *chip = dev_to_chip(dev);

	if (!chip->info->ops->ptp_ops->ptp_pin_setup)
		return -EOPNOTSUPP;

	return chip->info->ops->ptp_ops->ptp_pin_setup(chip, pin, flags,
						       enable);
}

static int mv88e6xxx_tai_write(struct device *dev, u8 reg, u16 val)
{
	struct mv88e6xxx_chip *chip = dev_to_chip(dev);
	int err;

	mv88e6xxx_reg_lock(chip);
	err = chip->info->ops->avb_ops->tai_write(chip, reg, val);
	mv88e6xxx_reg_unlock(chip);

	return err;
}

static int mv88e6xxx_tai_modify(struct device *dev, u8 reg, u16 mask, u16 val)
{
	struct mv88e6xxx_chip *chip = dev_to_chip(dev);
	u16 old, new;
	int err;

	mv88e6xxx_reg_lock(chip);
	err = chip->info->ops->avb_ops->tai_read(chip, reg, &old, 1);
	if (err < 0)
		goto unlock;

	new = (old & ~mask) | val;
	if (new != old)
		err = chip->info->ops->avb_ops->tai_write(chip, reg, new);

unlock:
	mv88e6xxx_reg_unlock(chip);
	return err;
}

static int mv88e6xxx_ptp_global_write(struct device *dev, u8 reg, u16 val)
{
	return 0;
}

static int mv88e6xxx_ptp_port_read_ts(struct device *dev, struct marvell_ts *ts,
				      u8 reg)
{
	return 0;
}

static int mv88e6xxx_ptp_port_write(struct device *dev, u8 reg, u16 val)
{
	return 0;
}

static int mv88e6xxx_ptp_port_modify(struct device *dev, u8 reg, u16 mask,
				     u16 val)
{
	return 0;
}

static long mv88e6xxx_ptp_aux_work(struct device *dev)
{
	return mv88e6xxx_hwtstamp_work(dev_to_chip(dev));
}

static const struct marvell_ptp_ops mv88e6xxx_ptp_ops = {
	.tai_enable = mv88e6xxx_tai_enable,
	.tai_clock_read = mv88e6xxx_tai_clock_read,
	.tai_extts_read = mv88e6xxx_tai_extts_read,
	.tai_pin_verify = mv88e6xxx_tai_pin_verify,
	.tai_pin_setup = mv88e6xxx_tai_pin_setup,
	.tai_write = mv88e6xxx_tai_write,
	.tai_modify = mv88e6xxx_tai_modify,
	.ptp_global_write = mv88e6xxx_ptp_global_write,
	.ptp_port_read_ts = mv88e6xxx_ptp_port_read_ts,
	.ptp_port_write = mv88e6xxx_ptp_port_write,
	.ptp_port_modify = mv88e6xxx_ptp_port_modify,
	.ptp_aux_work = mv88e6xxx_ptp_aux_work,
};

int mv88e6xxx_ptp_setup(struct mv88e6xxx_chip *chip)
{
	const struct mv88e6xxx_ptp_ops *ptp_ops = chip->info->ops->ptp_ops;
	const struct mv88e6xxx_cc_coeffs *cc_coeffs;
	struct marvell_tai_param tai_param;
	int i, n_pins, err;

	/* Set up the cycle counter */
	cc_coeffs = mv88e6xxx_cc_coeff_get(chip);
	if (IS_ERR(cc_coeffs))
		return PTR_ERR(cc_coeffs);

	if (ptp_ops->set_ptp_cpu_port) {
		err = mv88e6xxx_set_ptp_cpu_port(chip);
		if (err)
			return err;
	}

	memset(&tai_param, 0, sizeof(tai_param));
	tai_param.cc_mult_num = cc_coeffs->cc_mult_num;
	tai_param.cc_mult_den = cc_coeffs->cc_mult_dem;
	tai_param.cc_mult = cc_coeffs->cc_mult;
	tai_param.cc_shift = cc_coeffs->cc_shift;
	tai_param.n_ext_ts = ptp_ops->n_ext_ts;

	n_pins = mv88e6xxx_num_gpio(chip);
	for (i = 0; i < n_pins; ++i) {
		struct ptp_pin_desc *ppd = &chip->pin_config[i];

		snprintf(ppd->name, sizeof(ppd->name), "mv88e6xxx_gpio%d", i);
		ppd->index = i;
		ppd->func = PTP_PF_NONE;
	}

	mv88e6xxx_reg_unlock(chip);
	err = marvell_tai_probe(&chip->tai, &mv88e6xxx_ptp_ops, &tai_param,
				chip->pin_config, n_pins,
				dev_name(chip->dev), chip->dev);
	mv88e6xxx_reg_lock(chip);

	return err;
}

void mv88e6xxx_ptp_free(struct mv88e6xxx_chip *chip)
{
	if (chip->tai)
		marvell_tai_remove(chip->tai);
}
