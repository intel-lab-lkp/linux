// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for JEDEC PMIC50x0 compliant DDR5 PMICs
 *
 * Specification: https://www.jedec.org/standards-documents/docs/jesd301-1a03
 *
 * Copyright (C) 2026 Almog Ben Shaul <almogbs@amazon.com>
 */

#include <linux/bitfield.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/panic_notifier.h>
#include <linux/regmap.h>

/* PMIC50X0 Register Mapping */
#define PMIC50X0_REG_CURR_POW		0x0C
#define PMIC50X0_REG_NODE_A_SUM_SELECT	0x1A
#define PMIC50X0_REG_CURR_POW_SELECT	0x1B
#define PMIC50X0_REG_VOL_NODE_SELECT	0x30
#define PMIC50X0_REG_VOL		0x31
#define PMIC50X0_REG_TEMP		0x33
#define PMIC50X0_MAX_REG_ADDR		0xFF

/* PMIC50X0 Error Registers & Bits Mapping */
#define PMIC50X0_ERR_REG0_ADDR	0x04
#define PMIC50X0_ERR_REG1_ADDR	0x08
#define PMIC50X0_ERR_REG2_ADDR	0x09
#define PMIC50X0_ERR_REG3_ADDR	0x0A
#define PMIC50X0_ERR_REG4_ADDR	0x0B
#define PMIC50X0_ERR_REG5_ADDR	0x33

#define PMIC50X0_ERR_REG0_SBIT	4
#define PMIC50X0_ERR_REG1_SBIT	0
#define PMIC50X0_ERR_REG2_SBIT	0
#define PMIC50X0_ERR_REG3_SBIT	4
#define PMIC50X0_ERR_REG4_SBIT	0
#define PMIC50X0_ERR_REG5_SBIT	2

#define PMIC50X0_ERR_REG0_EBIT	6
#define PMIC50X0_ERR_REG1_EBIT	7
#define PMIC50X0_ERR_REG2_EBIT	7
#define PMIC50X0_ERR_REG3_EBIT	7
#define PMIC50X0_ERR_REG4_EBIT	7
#define PMIC50X0_ERR_REG5_EBIT	2

#define ERR_REG_NUM_BITS(bit)	(PMIC50X0_ERR_REG##bit##_EBIT - PMIC50X0_ERR_REG##bit##_SBIT + 1)
#define PMIC50X0_ERR_REG0_NUM_BITS ERR_REG_NUM_BITS(0)
#define PMIC50X0_ERR_REG1_NUM_BITS ERR_REG_NUM_BITS(1)
#define PMIC50X0_ERR_REG2_NUM_BITS ERR_REG_NUM_BITS(2)
#define PMIC50X0_ERR_REG3_NUM_BITS ERR_REG_NUM_BITS(3)
#define PMIC50X0_ERR_REG4_NUM_BITS ERR_REG_NUM_BITS(4)
#define PMIC50X0_ERR_REG5_NUM_BITS ERR_REG_NUM_BITS(5)

/* PMIC50X0 Masks and offsets etc. */
#define PMIC50X0_VOL_SELECT_MASK	GENMASK(6, 3)
#define PMIC50X0_VOL_MASK		GENMASK(7, 0)
#define PMIC50X0_TEMP_MASK		GENMASK(7, 5)
#define PMIC50X0_CURR_POW_A_MASK	GENMASK(7, 0)
#define PMIC50X0_CURR_POW_SUM_MASK	GENMASK(7, 0)
#define PMIC50X0_CURR_POW_BCD_MASK	GENMASK(5, 0)
#define PMIC50X0_CURR_POW_SELECT	BIT(6)
#define PMIC50X0_CURR_POW_A_SUM_SELECT	BIT(1)
#define PMIC50X0_VOL_OFFSET		3
#define PMIC50X0_CURR_POW_SELECT_OFFSET	6
#define PMIC50X0_CURR_POW_A_SUM_OFFSET	1
#define	PMIC50X0_SELECT_POWER_A		0
#define	PMIC50X0_SELECT_POWER_SUM	BIT(PMIC50X0_CURR_POW_A_SUM_OFFSET)
#define PMIC50X0_VOL_SELECT_DELAY	9

/* PMIC50X0 consts etc. */
#define PMIC50X0_DRIVER_NAME		"pmic50x0"
#define PMIC50X0_REG_BITS		8
#define PMIC50X0_VAL_BITS		8
#define PMIC50X0_VOL_FACTOR		15
#define PMIC50X0_TEMP_FACTOR		10
#define PMIC50X0_TEMP_BASE		75
#define PMIC50X0_CURR_POWER_FACTOR	125
#define PMIC50X0_ERR_POLL_INTERVAL_MSEC	1000
#define PMIC50X0_VOLTAGE_CHANNELS	4
#define PMIC50X0_PASS0_UNLOCK_VAL	0x73
#define PMIC50X0_PASS1_UNLOCK_VAL	0x94
#define PMIC50X0_PROT_UNLOCK_VAL	0x40
#define PMIC50X0_PROT_LOCK_VAL		0x0

#define MILLIDEGREE_PER_DEGREE          1000
#define PMIC50X0_DEV_ATTR(name, index)	\
	static SENSOR_DEVICE_ATTR(name, 0444, pmic50x0_err_show, 0, (index))

enum pmic50x0_curr_pow_node {
	PMIC50X0_CURR_POW_NODE_A,
	PMIC50X0_CURR_POW_NODE_B,
	PMIC50X0_CURR_POW_NODE_C,
	PMIC50X0_CURR_POW_NODE_D,
	PMIC50X0_CURR_POW_SUM
};

enum pmic50x0_select_current_power {
	PMIC50X0_SELECT_CURRENT,
	PMIC50X0_SELECT_POWER,
};

enum pmic50x0_error_list {
	PMIC50X0_ERR_GLOBAL_LOG_VIN_BULK_OVER_VOL,
	PMIC50X0_ERR_GLOBAL_LOG_CRIT_TEMP,
	PMIC50X0_ERR_GLOBAL_LOG_BUCK_OV_OR_UV,
	PMIC50X0_ERR_VIN_BULK_INPUT_OVER_VOL_STAT,
	PMIC50X0_ERR_VIN_MGMT_INPUT_OVER_VOL_STAT,
	PMIC50X0_ERR_SWD_OUT_POW_GOOD_STAT,
	PMIC50X0_ERR_SWC_OUT_POW_GOOD_STAT,
	PMIC50X0_ERR_SWB_OUT_POW_GOOD_STAT,
	PMIC50X0_ERR_SWA_OUT_POW_GOOD_STAT,
	PMIC50X0_ERR_CRIT_TEMP_SHUTDOWN_STAT,
	PMIC50X0_ERR_VIN_BULK_INPUT_POW_GOOD_STAT,
	PMIC50X0_ERR_SWD_HIGH_OUT_CURR_CONSUMP_STAT,
	PMIC50X0_ERR_SWC_HIGH_OUT_CURR_CONSUMP_STAT,
	PMIC50X0_ERR_SWB_HIGH_OUT_CURR_CONSUMP_STAT,
	PMIC50X0_ERR_SWA_HIGH_OUT_CURR_CONSUMP_STAT,
	PMIC50X0_ERR_VIN_MGMT_TO_VIN_BULK_STAT,
	PMIC50X0_ERR_VOUT_1_8V_OUT_POWER_GOOD_STAT,
	PMIC50X0_ERR_VBIAS_POWER_GOOD_STAT,
	PMIC50X0_ERR_PMIC_HIGH_TEMP_WARN_STAT,
	PMIC50X0_ERR_SWD_OUT_OVER_VOL_STAT,
	PMIC50X0_ERR_SWC_OUT_OVER_VOL_STAT,
	PMIC50X0_ERR_SWB_OUT_OVER_VOL_STAT,
	PMIC50X0_ERR_SWA_OUT_OVER_VOL_STAT,
	PMIC50X0_ERR_SWD_OUT_UNDER_VOL_LOCKOUT_STAT,
	PMIC50X0_ERR_SWC_OUT_UNDER_VOL_LOCKOUT_STAT,
	PMIC50X0_ERR_SWB_OUT_UNDER_VOL_LOCKOUT_STAT,
	PMIC50X0_ERR_SWA_OUT_UNDER_VOL_LOCKOUT_STAT,
	PMIC50X0_ERR_SWD_OUT_CURR_LIMITER_WARN_STAT,
	PMIC50X0_ERR_SWC_OUT_CURR_LIMITER_WARN_STAT,
	PMIC50X0_ERR_SWB_OUT_CURR_LIMITER_WARN_STAT,
	PMIC50X0_ERR_SWA_OUT_CURR_LIMITER_WARN_STAT,
	PMIC50X0_ERR_VOUT_1V_OUT_POWER_GOOD_STAT,
	PMIC50X0_ERR_MAX
};

struct pmic50x0_error {
	u8 reg;
	u8 bit;
};

static const struct pmic50x0_error pmic50x0_error_reg[PMIC50X0_ERR_MAX] = {
	[PMIC50X0_ERR_GLOBAL_LOG_VIN_BULK_OVER_VOL] =	{.reg = 0, .bit = 4},
	[PMIC50X0_ERR_GLOBAL_LOG_CRIT_TEMP] =		{.reg = 0, .bit = 5},
	[PMIC50X0_ERR_GLOBAL_LOG_BUCK_OV_OR_UV] =	{.reg = 0, .bit = 6},
	[PMIC50X0_ERR_VIN_BULK_INPUT_OVER_VOL_STAT] =   {.reg = 1, .bit = 0},
	[PMIC50X0_ERR_VIN_MGMT_INPUT_OVER_VOL_STAT] =   {.reg = 1, .bit = 1},
	[PMIC50X0_ERR_SWD_OUT_POW_GOOD_STAT] =          {.reg = 1, .bit = 2},
	[PMIC50X0_ERR_SWC_OUT_POW_GOOD_STAT] =          {.reg = 1, .bit = 3},
	[PMIC50X0_ERR_SWB_OUT_POW_GOOD_STAT] =          {.reg = 1, .bit = 4},
	[PMIC50X0_ERR_SWA_OUT_POW_GOOD_STAT] =          {.reg = 1, .bit = 5},
	[PMIC50X0_ERR_CRIT_TEMP_SHUTDOWN_STAT] =        {.reg = 1, .bit = 6},
	[PMIC50X0_ERR_VIN_BULK_INPUT_POW_GOOD_STAT] =   {.reg = 1, .bit = 7},
	[PMIC50X0_ERR_SWD_HIGH_OUT_CURR_CONSUMP_STAT] = {.reg = 2, .bit = 0},
	[PMIC50X0_ERR_SWC_HIGH_OUT_CURR_CONSUMP_STAT] = {.reg = 2, .bit = 1},
	[PMIC50X0_ERR_SWB_HIGH_OUT_CURR_CONSUMP_STAT] = {.reg = 2, .bit = 2},
	[PMIC50X0_ERR_SWA_HIGH_OUT_CURR_CONSUMP_STAT] = {.reg = 2, .bit = 3},
	[PMIC50X0_ERR_VIN_MGMT_TO_VIN_BULK_STAT] =      {.reg = 2, .bit = 4},
	[PMIC50X0_ERR_VOUT_1_8V_OUT_POWER_GOOD_STAT] =  {.reg = 2, .bit = 5},
	[PMIC50X0_ERR_VBIAS_POWER_GOOD_STAT] =          {.reg = 2, .bit = 6},
	[PMIC50X0_ERR_PMIC_HIGH_TEMP_WARN_STAT] =       {.reg = 2, .bit = 7},
	[PMIC50X0_ERR_SWD_OUT_OVER_VOL_STAT] =          {.reg = 3, .bit = 4},
	[PMIC50X0_ERR_SWC_OUT_OVER_VOL_STAT] =          {.reg = 3, .bit = 5},
	[PMIC50X0_ERR_SWB_OUT_OVER_VOL_STAT] =          {.reg = 3, .bit = 6},
	[PMIC50X0_ERR_SWA_OUT_OVER_VOL_STAT] =          {.reg = 3, .bit = 7},
	[PMIC50X0_ERR_SWD_OUT_UNDER_VOL_LOCKOUT_STAT] = {.reg = 4, .bit = 0},
	[PMIC50X0_ERR_SWC_OUT_UNDER_VOL_LOCKOUT_STAT] = {.reg = 4, .bit = 1},
	[PMIC50X0_ERR_SWB_OUT_UNDER_VOL_LOCKOUT_STAT] = {.reg = 4, .bit = 2},
	[PMIC50X0_ERR_SWA_OUT_UNDER_VOL_LOCKOUT_STAT] = {.reg = 4, .bit = 3},
	[PMIC50X0_ERR_SWD_OUT_CURR_LIMITER_WARN_STAT] = {.reg = 4, .bit = 4},
	[PMIC50X0_ERR_SWC_OUT_CURR_LIMITER_WARN_STAT] = {.reg = 4, .bit = 5},
	[PMIC50X0_ERR_SWB_OUT_CURR_LIMITER_WARN_STAT] = {.reg = 4, .bit = 6},
	[PMIC50X0_ERR_SWA_OUT_CURR_LIMITER_WARN_STAT] = {.reg = 4, .bit = 7},
	[PMIC50X0_ERR_VOUT_1V_OUT_POWER_GOOD_STAT]    = {.reg = 5, .bit = 2},
};

static const char *pmic50x0_reg0_err_msgs[PMIC50X0_ERR_REG0_NUM_BITS] = {
	"Global Error Log History for Critical Temperature",
	"Global Error Log History for VIN_Bulk Over",
	"Global Error Log History for Buck Regulator Output O/U Voltage",
};

static const char *pmic50x0_reg1_err_msgs[PMIC50X0_ERR_REG1_NUM_BITS] = {
	"VIN_Bulk Input Supply Over Voltage Status",
	"VIN_Mgmt Input Supply Over Voltage Status",
	"Switch Node D Output Power Good Status",
	"Switch Node C Output Power Good Status",
	"Switch Node B Output Power Good Status",
	"Switch Node A Output Power Good Status",
	"Critical Temperature Shutdown Status",
	"VIN_Bulk Input Power Good Status"
};

static const char *pmic50x0_reg2_err_msgs[PMIC50X0_ERR_REG2_NUM_BITS] = {
	"Switch Node D High Output Current Consumption Warning Status",
	"Switch Node C High Output Current Consumption Warning Status",
	"Switch Node B High Output Current Consumption Warning Status",
	"Switch Node A High Output Current Consumption Warning Status",
	"VIN_Mgmt to VIN_Bulk Input Supply Automatic Switchover Status",
	"VOUT_1.8V LDO Output Power Good Status",
	"VBias Power Good Status",
	"PMIC High Temperature Warning Status"
};

static const char *pmic50x0_reg3_err_msgs[PMIC50X0_ERR_REG3_NUM_BITS] = {
	"Switch Node D Output Over Voltage Status",
	"Switch Node C Output Over Voltage Status",
	"Switch Node B Output Over Voltage Status",
	"Switch Node A Output Over Voltage Status"
};

static const char *pmic50x0_reg4_err_msgs[PMIC50X0_ERR_REG4_NUM_BITS] = {
	"Switch Node D Output Under Voltage Lockout Status",
	"Switch Node C Output Under Voltage Lockout Status",
	"Switch Node B Output Under Voltage Lockout Status",
	"Switch Node A Output Under Voltage Lockout Status",
	"Switch Node D Output Current Limiter Warning Status",
	"Switch Node C Output Current Limiter Warning Status",
	"Switch Node B Output Current Limiter Warning Status",
	"Switch Node A Output Current Limiter Warning Status"
};

static const char *pmic50x0_reg5_err_msgs[PMIC50X0_ERR_REG5_NUM_BITS] = {
	"VOUT_1.0V LDO Output Power Good"
};

struct pmic50x0_err_reg {
	unsigned int addr;
	unsigned int *counters;
	const char **err_msgs;
	u8 sbit;
	u8 ebit;
	u8 size;
	u8 err_active;
};

/* Main driver struct */
struct pmic50x0 {
	struct device *dev;
	struct regmap *regmap;
	struct delayed_work work;
	struct pmic50x0_err_reg *err_regs;
	struct notifier_block panic_notifier;
	long last_voltage[PMIC50X0_VOLTAGE_CHANNELS];
	/* Mutex for reading the voltage registers */
	struct mutex voltage_mutex;
	/* Mutex for reading the current and power registers */
	struct mutex curr_power_mutex;

};

static unsigned int error_polling_ms = PMIC50X0_ERR_POLL_INTERVAL_MSEC;
module_param(error_polling_ms, uint, 0644);
MODULE_PARM_DESC(error_polling_ms, "PMIC error polling interval in msec, default = 1000");

static ssize_t pmic50x0_err_show(struct device *dev,
				 struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct pmic50x0 *pmic50x0 = dev_get_drvdata(dev);
	unsigned int idx = attr->index;
	struct pmic50x0_error err = pmic50x0_error_reg[idx];
	u8 reg, bit;

	reg = err.reg;
	bit = err.bit - pmic50x0->err_regs[reg].sbit;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pmic50x0->err_regs[reg].counters[bit]);
}

/* Error Sysfs group */
PMIC50X0_DEV_ATTR(err_global_log_vin_bulk_over_vol, PMIC50X0_ERR_GLOBAL_LOG_VIN_BULK_OVER_VOL);
PMIC50X0_DEV_ATTR(err_global_log_crit_temp, PMIC50X0_ERR_GLOBAL_LOG_CRIT_TEMP);
PMIC50X0_DEV_ATTR(err_global_log_buck_ov_or_uv, PMIC50X0_ERR_GLOBAL_LOG_BUCK_OV_OR_UV);
PMIC50X0_DEV_ATTR(err_vin_bulk_input_over_vol_stat, PMIC50X0_ERR_VIN_BULK_INPUT_OVER_VOL_STAT);
PMIC50X0_DEV_ATTR(err_vin_mgmt_input_over_vol_stat, PMIC50X0_ERR_VIN_MGMT_INPUT_OVER_VOL_STAT);
PMIC50X0_DEV_ATTR(err_swd_out_pow_good_stat, PMIC50X0_ERR_SWD_OUT_POW_GOOD_STAT);
PMIC50X0_DEV_ATTR(err_swc_out_pow_good_stat, PMIC50X0_ERR_SWC_OUT_POW_GOOD_STAT);
PMIC50X0_DEV_ATTR(err_swb_out_pow_good_stat, PMIC50X0_ERR_SWB_OUT_POW_GOOD_STAT);
PMIC50X0_DEV_ATTR(err_swa_out_pow_good_stat, PMIC50X0_ERR_SWA_OUT_POW_GOOD_STAT);
PMIC50X0_DEV_ATTR(err_crit_temp_shutdown_stat, PMIC50X0_ERR_CRIT_TEMP_SHUTDOWN_STAT);
PMIC50X0_DEV_ATTR(err_vin_bulk_input_pow_good_stat, PMIC50X0_ERR_VIN_BULK_INPUT_POW_GOOD_STAT);
PMIC50X0_DEV_ATTR(err_swd_high_out_curr_consump_stat, PMIC50X0_ERR_SWD_HIGH_OUT_CURR_CONSUMP_STAT);
PMIC50X0_DEV_ATTR(err_swc_high_out_curr_consump_stat, PMIC50X0_ERR_SWC_HIGH_OUT_CURR_CONSUMP_STAT);
PMIC50X0_DEV_ATTR(err_swb_high_out_curr_consump_stat, PMIC50X0_ERR_SWB_HIGH_OUT_CURR_CONSUMP_STAT);
PMIC50X0_DEV_ATTR(err_swa_high_out_curr_consump_stat, PMIC50X0_ERR_SWA_HIGH_OUT_CURR_CONSUMP_STAT);
PMIC50X0_DEV_ATTR(err_vin_mgmt_to_vin_bulk_stat, PMIC50X0_ERR_VIN_MGMT_TO_VIN_BULK_STAT);
PMIC50X0_DEV_ATTR(err_vout_1_8v_out_power_good_stat, PMIC50X0_ERR_VOUT_1_8V_OUT_POWER_GOOD_STAT);
PMIC50X0_DEV_ATTR(err_vbias_power_good_stat, PMIC50X0_ERR_VBIAS_POWER_GOOD_STAT);
PMIC50X0_DEV_ATTR(err_pmic_high_temp_warn_stat, PMIC50X0_ERR_PMIC_HIGH_TEMP_WARN_STAT);
PMIC50X0_DEV_ATTR(err_swd_out_over_vol_stat, PMIC50X0_ERR_SWD_OUT_OVER_VOL_STAT);
PMIC50X0_DEV_ATTR(err_swc_out_over_vol_stat, PMIC50X0_ERR_SWC_OUT_OVER_VOL_STAT);
PMIC50X0_DEV_ATTR(err_swb_out_over_vol_stat, PMIC50X0_ERR_SWB_OUT_OVER_VOL_STAT);
PMIC50X0_DEV_ATTR(err_swa_out_over_vol_stat, PMIC50X0_ERR_SWA_OUT_OVER_VOL_STAT);
PMIC50X0_DEV_ATTR(err_swd_out_under_vol_lockout_stat, PMIC50X0_ERR_SWD_OUT_UNDER_VOL_LOCKOUT_STAT);
PMIC50X0_DEV_ATTR(err_swc_out_under_vol_lockout_stat, PMIC50X0_ERR_SWC_OUT_UNDER_VOL_LOCKOUT_STAT);
PMIC50X0_DEV_ATTR(err_swb_out_under_vol_lockout_stat, PMIC50X0_ERR_SWB_OUT_UNDER_VOL_LOCKOUT_STAT);
PMIC50X0_DEV_ATTR(err_swa_out_under_vol_lockout_stat, PMIC50X0_ERR_SWA_OUT_UNDER_VOL_LOCKOUT_STAT);
PMIC50X0_DEV_ATTR(err_swd_out_curr_limiter_warn_stat, PMIC50X0_ERR_SWD_OUT_CURR_LIMITER_WARN_STAT);
PMIC50X0_DEV_ATTR(err_swc_out_curr_limiter_warn_stat, PMIC50X0_ERR_SWC_OUT_CURR_LIMITER_WARN_STAT);
PMIC50X0_DEV_ATTR(err_swb_out_curr_limiter_warn_stat, PMIC50X0_ERR_SWB_OUT_CURR_LIMITER_WARN_STAT);
PMIC50X0_DEV_ATTR(err_swa_out_curr_limiter_warn_stat, PMIC50X0_ERR_SWA_OUT_CURR_LIMITER_WARN_STAT);
PMIC50X0_DEV_ATTR(err_vout_1v_out_power_good_stat, PMIC50X0_ERR_VOUT_1V_OUT_POWER_GOOD_STAT);

static struct attribute *pmic50x0_err_attrs[] = {
	&sensor_dev_attr_err_global_log_vin_bulk_over_vol.dev_attr.attr,
	&sensor_dev_attr_err_global_log_crit_temp.dev_attr.attr,
	&sensor_dev_attr_err_global_log_buck_ov_or_uv.dev_attr.attr,
	&sensor_dev_attr_err_vin_bulk_input_over_vol_stat.dev_attr.attr,
	&sensor_dev_attr_err_vin_mgmt_input_over_vol_stat.dev_attr.attr,
	&sensor_dev_attr_err_swd_out_pow_good_stat.dev_attr.attr,
	&sensor_dev_attr_err_swc_out_pow_good_stat.dev_attr.attr,
	&sensor_dev_attr_err_swb_out_pow_good_stat.dev_attr.attr,
	&sensor_dev_attr_err_swa_out_pow_good_stat.dev_attr.attr,
	&sensor_dev_attr_err_crit_temp_shutdown_stat.dev_attr.attr,
	&sensor_dev_attr_err_vin_bulk_input_pow_good_stat.dev_attr.attr,
	&sensor_dev_attr_err_swd_high_out_curr_consump_stat.dev_attr.attr,
	&sensor_dev_attr_err_swc_high_out_curr_consump_stat.dev_attr.attr,
	&sensor_dev_attr_err_swb_high_out_curr_consump_stat.dev_attr.attr,
	&sensor_dev_attr_err_swa_high_out_curr_consump_stat.dev_attr.attr,
	&sensor_dev_attr_err_vin_mgmt_to_vin_bulk_stat.dev_attr.attr,
	&sensor_dev_attr_err_vout_1_8v_out_power_good_stat.dev_attr.attr,
	&sensor_dev_attr_err_vbias_power_good_stat.dev_attr.attr,
	&sensor_dev_attr_err_pmic_high_temp_warn_stat.dev_attr.attr,
	&sensor_dev_attr_err_swd_out_over_vol_stat.dev_attr.attr,
	&sensor_dev_attr_err_swc_out_over_vol_stat.dev_attr.attr,
	&sensor_dev_attr_err_swb_out_over_vol_stat.dev_attr.attr,
	&sensor_dev_attr_err_swa_out_over_vol_stat.dev_attr.attr,
	&sensor_dev_attr_err_swd_out_under_vol_lockout_stat.dev_attr.attr,
	&sensor_dev_attr_err_swc_out_under_vol_lockout_stat.dev_attr.attr,
	&sensor_dev_attr_err_swb_out_under_vol_lockout_stat.dev_attr.attr,
	&sensor_dev_attr_err_swa_out_under_vol_lockout_stat.dev_attr.attr,
	&sensor_dev_attr_err_swd_out_curr_limiter_warn_stat.dev_attr.attr,
	&sensor_dev_attr_err_swc_out_curr_limiter_warn_stat.dev_attr.attr,
	&sensor_dev_attr_err_swb_out_curr_limiter_warn_stat.dev_attr.attr,
	&sensor_dev_attr_err_swa_out_curr_limiter_warn_stat.dev_attr.attr,
	&sensor_dev_attr_err_vout_1v_out_power_good_stat.dev_attr.attr,
	NULL,
};

ATTRIBUTE_GROUPS(pmic50x0_err);

static struct pmic50x0_err_reg pmic50x0_err_regs[] = {
	{
		.addr = PMIC50X0_ERR_REG0_ADDR,
		.err_msgs = pmic50x0_reg0_err_msgs,
		.sbit = PMIC50X0_ERR_REG0_SBIT,
		.ebit = PMIC50X0_ERR_REG0_EBIT,
		.size = PMIC50X0_ERR_REG0_NUM_BITS,
	},
	{
		.addr = PMIC50X0_ERR_REG1_ADDR,
		.err_msgs = pmic50x0_reg1_err_msgs,
		.sbit = PMIC50X0_ERR_REG1_SBIT,
		.ebit = PMIC50X0_ERR_REG1_EBIT,
		.size = PMIC50X0_ERR_REG1_NUM_BITS,
	},
	{
		.addr = PMIC50X0_ERR_REG2_ADDR,
		.err_msgs = pmic50x0_reg2_err_msgs,
		.sbit = PMIC50X0_ERR_REG2_SBIT,
		.ebit = PMIC50X0_ERR_REG2_EBIT,
		.size = PMIC50X0_ERR_REG2_NUM_BITS,
	},
	{
		.addr = PMIC50X0_ERR_REG3_ADDR,
		.err_msgs = pmic50x0_reg3_err_msgs,
		.sbit = PMIC50X0_ERR_REG3_SBIT,
		.ebit = PMIC50X0_ERR_REG3_EBIT,
		.size = PMIC50X0_ERR_REG3_NUM_BITS,
	},
	{
		.addr = PMIC50X0_ERR_REG4_ADDR,
		.err_msgs = pmic50x0_reg4_err_msgs,
		.sbit = PMIC50X0_ERR_REG4_SBIT,
		.ebit = PMIC50X0_ERR_REG4_EBIT,
		.size = PMIC50X0_ERR_REG4_NUM_BITS,
	},
	{
		.addr = PMIC50X0_ERR_REG5_ADDR,
		.err_msgs = pmic50x0_reg5_err_msgs,
		.sbit = PMIC50X0_ERR_REG5_SBIT,
		.ebit = PMIC50X0_ERR_REG5_EBIT,
		.size = PMIC50X0_ERR_REG5_NUM_BITS,
	},
};

static int pmic50x0_temp_read(struct device *dev, long *val)
{
	struct pmic50x0 *pmic50x0 = dev_get_drvdata(dev);
	unsigned int regval;
	long temp;
	int err;

	err = regmap_read(pmic50x0->regmap, PMIC50X0_REG_TEMP, &regval);
	if (err < 0)
		return err;

	temp = FIELD_GET(PMIC50X0_TEMP_MASK, regval) * PMIC50X0_TEMP_FACTOR;
	*val = MILLIDEGREE_PER_DEGREE * (PMIC50X0_TEMP_BASE + temp);

	return 0;
}

static int pmic50x0_in_read(struct device *dev, long *val, int channel)
{
	struct pmic50x0 *pmic50x0 = dev_get_drvdata(dev);
	unsigned int regval;
	int err;

	mutex_lock(&pmic50x0->voltage_mutex);

	/* Select the node */
	err = regmap_update_bits(pmic50x0->regmap, PMIC50X0_REG_VOL_NODE_SELECT,
				 PMIC50X0_VOL_SELECT_MASK, channel << PMIC50X0_VOL_OFFSET);
	if (err < 0)
		goto ret_unlock;

	/* The spec requires 9ms delay between the node selection and the reading */
	msleep(PMIC50X0_VOL_SELECT_DELAY);

	/* Read the voltage register after selecting the node */
	err = regmap_read(pmic50x0->regmap, PMIC50X0_REG_VOL, &regval);
	if (err < 0)
		goto ret_unlock;

	*val = FIELD_GET(PMIC50X0_VOL_MASK, regval) * PMIC50X0_VOL_FACTOR;
	pmic50x0->last_voltage[channel] = *val;

ret_unlock:
	mutex_unlock(&pmic50x0->voltage_mutex);

	return err;
}

static int pmic50x0_curr_power_read(struct device *dev, long *val, int channel,
				    enum pmic50x0_select_current_power node)
{
	struct pmic50x0 *pmic50x0 = dev_get_drvdata(dev);
	unsigned int regval, reg;
	long mask;
	int err;

	mutex_lock(&pmic50x0->curr_power_mutex);

	/* Select power/current mode */
	err = regmap_update_bits(pmic50x0->regmap, PMIC50X0_REG_CURR_POW_SELECT,
				 PMIC50X0_CURR_POW_SELECT, node << PMIC50X0_CURR_POW_SELECT_OFFSET);
	if (err < 0)
		goto ret_unlock;

	switch (channel) {
	case PMIC50X0_CURR_POW_NODE_A:
		mask = PMIC50X0_CURR_POW_A_MASK;
		reg = PMIC50X0_REG_CURR_POW;

		/* Select node A */
		err = regmap_update_bits(pmic50x0->regmap, PMIC50X0_REG_NODE_A_SUM_SELECT,
					 PMIC50X0_CURR_POW_A_SUM_SELECT, PMIC50X0_SELECT_POWER_A);
		if (err < 0)
			goto ret_unlock;
		break;
	case PMIC50X0_CURR_POW_NODE_B:
	case PMIC50X0_CURR_POW_NODE_C:
	case PMIC50X0_CURR_POW_NODE_D:
		mask = PMIC50X0_CURR_POW_BCD_MASK;
		reg = PMIC50X0_REG_CURR_POW + channel;
		break;
	case PMIC50X0_CURR_POW_SUM:
		mask = PMIC50X0_CURR_POW_SUM_MASK;
		reg = PMIC50X0_REG_CURR_POW;

		/* Select the sum of A,B,C and D nodes */
		err = regmap_update_bits(pmic50x0->regmap, PMIC50X0_REG_NODE_A_SUM_SELECT,
					 PMIC50X0_CURR_POW_A_SUM_SELECT, PMIC50X0_SELECT_POWER_SUM);
		if (err < 0)
			goto ret_unlock;
		break;
	default:
		err = -EOPNOTSUPP;
		goto ret_unlock;
	}

	err = regmap_read(pmic50x0->regmap, reg, &regval);
	if (err < 0)
		goto ret_unlock;

	*val = (regval & mask) * PMIC50X0_CURR_POWER_FACTOR;

ret_unlock:
	mutex_unlock(&pmic50x0->curr_power_mutex);

	return err;
}

static int pmic50x0_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			 long *val)
{
	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		return pmic50x0_temp_read(dev, val);
	case hwmon_in:
		if (attr != hwmon_in_input)
			return -EOPNOTSUPP;
		return pmic50x0_in_read(dev, val, channel);
	case hwmon_curr:
		if (attr != hwmon_curr_input)
			return -EOPNOTSUPP;
		return pmic50x0_curr_power_read(dev, val, channel, PMIC50X0_SELECT_CURRENT);
	case hwmon_power:
		if (attr != hwmon_power_input)
			return -EOPNOTSUPP;
		return pmic50x0_curr_power_read(dev, val, channel, PMIC50X0_SELECT_POWER);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t pmic50x0_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
				   int channel)
{
	switch (type) {
	case hwmon_temp:
		return (attr == hwmon_temp_input) ? 0444 : 0;
	case hwmon_in:
		return (attr == hwmon_in_input) ? 0444 : 0;
	case hwmon_curr:
		return (attr == hwmon_curr_input) ? 0444 : 0;
	case hwmon_power:
		return (attr == hwmon_power_input) ? 0444 : 0;
	default:
		return 0;
	}
}

static const u32 pmic50x0_temp_config[] = {
	HWMON_T_INPUT,
	0
};

static const struct hwmon_channel_info pmic50x0_temp = {
	.type = hwmon_temp,
	.config = pmic50x0_temp_config,
};

static const u32 pmic50x0_in_config[] = {
	HWMON_I_INPUT,
	HWMON_I_INPUT,
	HWMON_I_INPUT,
	HWMON_I_INPUT,
	0
};

static const struct hwmon_channel_info pmic50x0_in = {
	.type = hwmon_in,
	.config = pmic50x0_in_config,
};

static const u32 pmic50x0_curr_config[] = {
	HWMON_C_INPUT,
	HWMON_C_INPUT,
	HWMON_C_INPUT,
	HWMON_C_INPUT,
	0
};

static const struct hwmon_channel_info pmic50x0_curr = {
	.type = hwmon_curr,
	.config = pmic50x0_curr_config,
};

static const u32 pmic50x0_power_config[] = {
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	HWMON_P_INPUT,
	0
};

static const struct hwmon_channel_info pmic50x0_power = {
	.type = hwmon_power,
	.config = pmic50x0_power_config,
};

static const struct hwmon_channel_info *pmic50x0_info[] = {
	&pmic50x0_temp,
	&pmic50x0_in,
	&pmic50x0_curr,
	&pmic50x0_power,
	NULL
};

static const struct hwmon_ops pmic50x0_ops = {
	.is_visible = pmic50x0_is_visible,
	.read = pmic50x0_read,
};

static const struct hwmon_chip_info pmic50x0_chip_info = {
	.ops = &pmic50x0_ops,
	.info = pmic50x0_info
};

static const struct regmap_config pmic50x0_regmap_config = {
	.reg_bits = PMIC50X0_REG_BITS,
	.val_bits = PMIC50X0_VAL_BITS,
	.max_register = PMIC50X0_MAX_REG_ADDR
};

static void pmic50x0_update_last_volt(struct pmic50x0 *pmic50x0)
{
	struct device *dev = pmic50x0->dev;
	int i, err;

	for (i = 0; i < PMIC50X0_VOLTAGE_CHANNELS; i++) {
		err = pmic50x0_in_read(dev, &pmic50x0->last_voltage[i], i);
		if (err < 0)
			dev_err(dev, "Failed to read voltage (%d)\n", err);
	}
}

static void pmic50x0_work_callback(struct work_struct *work)
{
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct pmic50x0 *pmic50x0 = container_of(delayed_work, struct pmic50x0, work);
	struct pmic50x0_err_reg *reg;
	u8 bit, sbit, ebit, i, idx;
	unsigned int err_reg;
	int err;

	pmic50x0_update_last_volt(pmic50x0);

	for (i = 0; i < ARRAY_SIZE(pmic50x0_err_regs); i++) {
		reg = &pmic50x0->err_regs[i];
		err = regmap_read(pmic50x0->regmap, reg->addr, &err_reg);
		if (err < 0) {
			dev_err(pmic50x0->dev, "Could not read Error Register at %x\n", reg->addr);
			continue;
		}

		/* Checks if error bits changed since last polling interval */
		if (err_reg == reg->err_active)
			continue;

		sbit = reg->sbit;
		ebit = reg->ebit;

		for (bit = sbit; bit <= ebit; bit++) {
			idx = bit - sbit;

			if (err_reg & BIT(bit)) {
				/* Continue if error is not new */
				if (reg->err_active & BIT(bit))
					continue;

				/* Mark the error bit as active */
				reg->err_active |= BIT(bit);
				reg->counters[idx]++;

				dev_err(pmic50x0->dev, "%s (cnt=%u)\n", reg->err_msgs[idx],
					reg->counters[idx]);
			} else if (reg->err_active & BIT(bit)) {
				/* Error cleared since last polling interval. */
				dev_info(pmic50x0->dev, "Error (%s) cleared\n", reg->err_msgs[idx]);

				reg->err_active &= ~BIT(bit);
			}
		}
	}

	schedule_delayed_work(&pmic50x0->work, msecs_to_jiffies(error_polling_ms));
}

static void pmic50x0_cancel_work(void *work)
{
	cancel_delayed_work_sync(work);
}

static int pmic50x0_work_init(struct device *dev)
{
	struct pmic50x0 *pmic50x0 = dev_get_drvdata(dev);

	INIT_DELAYED_WORK(&pmic50x0->work, pmic50x0_work_callback);
	schedule_delayed_work(&pmic50x0->work, msecs_to_jiffies(error_polling_ms));

	return devm_add_action_or_reset(dev, pmic50x0_cancel_work, &pmic50x0->work);
}

static int pmic50x0_error_init(struct device *dev)
{
	struct pmic50x0 *pmic50x0 = dev_get_drvdata(dev);
	int reg;

	pmic50x0->err_regs = devm_kcalloc(dev, ARRAY_SIZE(pmic50x0_err_regs),
					  sizeof(*pmic50x0->err_regs), GFP_KERNEL);
	if (!pmic50x0->err_regs)
		return -ENOMEM;

	for (reg = 0; reg < ARRAY_SIZE(pmic50x0_err_regs); reg++) {
		pmic50x0->err_regs[reg] = pmic50x0_err_regs[reg];
		pmic50x0->err_regs[reg].counters =
			devm_kcalloc(dev, pmic50x0->err_regs[reg].size,
				     sizeof(*pmic50x0->err_regs[reg].counters), GFP_KERNEL);
		if (!pmic50x0->err_regs[reg].counters)
			return -ENOMEM;
	}

	return 0;
}

static void pmic50x0_mutexes_destroy(void *arg)
{
	struct pmic50x0 *pmic50x0 = arg;

	mutex_destroy(&pmic50x0->curr_power_mutex);
	mutex_destroy(&pmic50x0->voltage_mutex);
}

static int pmic50x0_mutexes_init(struct pmic50x0 *pmic50x0)
{
	mutex_init(&pmic50x0->voltage_mutex);
	mutex_init(&pmic50x0->curr_power_mutex);

	return devm_add_action_or_reset(pmic50x0->dev, pmic50x0_mutexes_destroy, pmic50x0);
}

static int pmic50x0_panic_callback(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	struct pmic50x0 *pmic50x0 = container_of(nb, struct pmic50x0, panic_notifier);

	dev_emerg(pmic50x0->dev, "volt(mV): A=%ld, B=%ld, C=%ld, D=%ld\n",
		  pmic50x0->last_voltage[0], pmic50x0->last_voltage[1],
		  pmic50x0->last_voltage[2], pmic50x0->last_voltage[3]);

	return NOTIFY_DONE;
}

static void pmic50x0_panic_notifier_unregister(void *data)
{
	struct pmic50x0 *pmic50x0 = data;

	atomic_notifier_chain_unregister(&panic_notifier_list, &pmic50x0->panic_notifier);
}

static int pmic50x0_panic_notifier_register(struct pmic50x0 *pmic50x0)
{
	struct notifier_block *panic_notifier = &pmic50x0->panic_notifier;
	struct device *dev = pmic50x0->dev;
	int ret;

	panic_notifier->notifier_call = pmic50x0_panic_callback;
	panic_notifier->priority = 0;

	ret = atomic_notifier_chain_register(&panic_notifier_list, panic_notifier);
	if (ret) {
		dev_err(dev, "failed to register panic notifier (%d)\n", ret);
		return ret;
	}

	return devm_add_action_or_reset(dev, pmic50x0_panic_notifier_unregister, pmic50x0);
}

static int pmic50x0_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct pmic50x0 *pmic50x0;
	struct device *hwmon_dev;
	int err;

	pmic50x0 = devm_kzalloc(dev, sizeof(*pmic50x0), GFP_KERNEL);
	if (!pmic50x0)
		return -ENOMEM;

	dev_set_drvdata(dev, pmic50x0);

	pmic50x0->dev = dev;
	pmic50x0->regmap = devm_regmap_init_i2c(client, &pmic50x0_regmap_config);
	if (IS_ERR(pmic50x0->regmap)) {
		dev_err(dev, "init regmap failed!\n");
		return PTR_ERR(pmic50x0->regmap);
	}

	err = pmic50x0_error_init(dev);
	if (err < 0)
		return err;

	err = pmic50x0_mutexes_init(pmic50x0);
	if (err < 0)
		return err;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name, pmic50x0,
							 &pmic50x0_chip_info, pmic50x0_err_groups);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	err = pmic50x0_panic_notifier_register(pmic50x0);
	if (err < 0)
		return err;

	return pmic50x0_work_init(dev);
}

static const struct i2c_device_id pmic50x0_ids[] = {
	{ PMIC50X0_DRIVER_NAME },
	{ }
};

MODULE_DEVICE_TABLE(i2c, pmic50x0_ids);

static const struct of_device_id pmic50x0_of_match[] = {
	{ .compatible = "jedec,pmic50x0" },
	{ },
};

MODULE_DEVICE_TABLE(of, pmic50x0_of_match);

static struct i2c_driver pmic50x0_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver		= {
				.name = PMIC50X0_DRIVER_NAME,
				.of_match_table = pmic50x0_of_match,
			},
	.probe		= pmic50x0_probe,
	.id_table	= pmic50x0_ids,
};

module_i2c_driver(pmic50x0_driver);

MODULE_AUTHOR("Almog Ben Shaul <almogbs@amazon.com>");
MODULE_DESCRIPTION("JEDEC PMIC50x0 Hardware Monitoring Driver");
MODULE_LICENSE("GPL");
