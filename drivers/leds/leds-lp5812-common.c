// SPDX-License-Identifier: GPL-2.0-only
/*
 * LP5812 Common Driver
 *
 * Copyright (C) 2025 Texas Instruments
 *
 * Author: Jared Zhou <jared-zhou@ti.com>
 */

#include "leds-lp5812-common.h"

/*
 * Function: lp5812_write
 * Params:
 * Return: 0 if success
 */
int lp5812_write(struct lp5812_chip *chip, u16 reg, u8 val)
{
	int ret;
	u8 extracted_bits; /* save 9th and 8th bit of reg address */
	struct i2c_msg msg;
	u8 buf[2] = {(u8)(reg & 0xFF), val};

	extracted_bits = (reg >> 8) & 0x03;
	msg.addr = (chip->i2c_cl->addr << 2) | extracted_bits;
	msg.flags = 0;
	msg.len = sizeof(buf);
	msg.buf = buf;

	ret = i2c_transfer(chip->i2c_cl->adapter, &msg, 1);
	if (ret != 1) {
		pr_err("i2c write error, ret=%d\n", ret);
		ret = ret < 0 ? ret : -EIO;
	} else {
		ret = 0;
	}

	return ret;
}

int lp5812_read(struct lp5812_chip *chip, u16 reg, u8 *val)
{
	int ret;
	u8 ret_val;  /* lp5812_chip return value */
	u8 extracted_bits; /* save 9th and 8th bit of reg address */
	u8 converted_reg;  /* extracted 8bit from reg */
	struct i2c_msg msgs[2];

	extracted_bits = (reg >> 8) & 0x03;
	converted_reg = (u8)(reg & 0xFF);

	msgs[0].addr = (chip->i2c_cl->addr << 2) | extracted_bits;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &converted_reg;

	msgs[1].addr = (chip->i2c_cl->addr << 2) | extracted_bits;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = &ret_val;

	ret = i2c_transfer(chip->i2c_cl->adapter, msgs, 2);
	if (ret != 2) {
		pr_err("Read reg value error, ret=%d\n", ret);
		*val = 0;
		ret = ret < 0 ? ret : -EIO;
	} else {
		*val = ret_val;
		ret = 0;
	}

	return ret;
}

int lp5812_update_bit(struct lp5812_chip *chip, u16 reg, u8 mask, u8 val)
{
	int ret;
	u8 tmp;

	ret = lp5812_read(chip, reg, &tmp);
	if (ret)
		return ret;

	tmp &= ~mask;
	tmp |= val & mask;

	return lp5812_write(chip, reg, tmp);
}

/*
 * Function: lp5812_read_tsd_config_status
 * Description: read tsd config status register
 * Param: chip --> struct lp5812_chip itself
 *        reg_val
 * Return: 0 if success
 */
int lp5812_read_tsd_config_status(struct lp5812_chip *chip, u8 *reg_val)
{
	int ret = 0;

	if (!reg_val)
		return -1;

	ret = lp5812_read(chip, chip->regs->tsd_config_status_reg, reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed\n",
				chip->regs->tsd_config_status_reg);
	}

	return ret;
}

/*
 * Function: lp5812_update_regs_config
 * Description: update reg config register
 * Param: chip --> struct lp5812_chip itself
 * Return: 0 if success
 */
int lp5812_update_regs_config(struct lp5812_chip *chip)
{
	int ret;
	u8 reg_val; /* save register value */

	/* Send update command to update config setting */
	ret = lp5812_write(chip, chip->regs->update_cmd_reg, UPDATE_CMD_VAL);
	if (ret) {
		pr_info("[DEBUG]: Send update command failed\n");
		return ret;
	}
	/* check if the configuration is proper */
	ret = lp5812_read_tsd_config_status(chip, &reg_val);
	if (ret == 0) {
		pr_info("[DEBUG]: tsd_config_status_reg: %d\n", (int)reg_val);
		return (int)(reg_val & 0x01);
	}

	return ret;
}

/*
 * Function: lp5812_read_lod_status
 * Description: read lod status register
 * Param: chip --> struct lp5812_chip itself
 *        led_number --> int
 *        val -> u8 *
 * Return: 0 if success
 */
int lp5812_read_lod_status(struct lp5812_chip *chip, int led_number, u8 *val)
{
	int ret = 0;
	u16 reg = 0;
	u8 reg_val = 0;

	if (!val)
		return -1;

	if (led_number < 0x8)
		reg = LOD_STAT_1_REG;
	else
		reg = LOD_STAT_2_REG;

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed: %d\n", reg, ret);
		return ret;
	}

	*val = (reg_val & (1 << (led_number % 8))) ? 1 : 0;

	return ret;
}

/*
 * Function: lp5812_read_lsd_status
 * Description: read lsd status register
 * Param: chip --> struct lp5812_chip itself
 *        led_number --> int
 *        val -> u8 *
 * Return: 0 if success
 */
int lp5812_read_lsd_status(struct lp5812_chip *chip, int led_number, u8 *val)
{
	int ret = 0;
	u16 reg = 0;
	u8 reg_val = 0;

	if (!val)
		return -1;

	if (led_number < 0x8)
		reg = LSD_STAT_1_REG;
	else
		reg = LSD_STAT_2_REG;

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed: %d\n", reg, ret);
		return ret;
	}

	*val = (reg_val & (1 << (led_number % 8))) ? 1 : 0;

	return ret;
}

/*
 * Function: lp5812_read_auto_pwm_value
 * Description: read pwm value in autonomous mode
 * Param: chip --> struct lp5812_chip itself
 *        led_number --> int
 *        val -> u8 *
 * Return: 0 if success
 */
int lp5812_read_auto_pwm_value(struct lp5812_chip *chip, int led_number,
		u8 *val)
{
	int ret = 0;
	u16 reg = 0;
	u8 reg_val = 0;

	reg = AUTO_PWM_BASE_ADDR + led_number;

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed: %d\n", reg, ret);
		return ret;
	}

	*val = reg_val;

	return ret;
}

/*
 * Function: lp5812_read_aep_status
 * Description: read autonomous engine pattern status
 * Param: chip --> struct lp5812_chip itself
 *        led_number --> int
 *        val -> u8 *
 * Return: 0 if success
 */
int lp5812_read_aep_status(struct lp5812_chip *chip, int led_number, u8 *val)
{
	int ret = 0;
	u16 reg;
	u8 reg_val;

	switch (led_number / 2) {
	case 0:
		reg = AEP_STATUS_0_REG; // LED_0 and LED_1
		break;
	case 1:
		reg = AEP_STATUS_1_REG; // LED_2 and LED_3
		break;
	case 2:
		reg = AEP_STATUS_2_REG; // LED_A0 and LED_A1
		break;
	case 3:
		reg = AEP_STATUS_3_REG; // LED_A2 and LED_B0
		break;
	case 4:
		reg = AEP_STATUS_4_REG; // LED_B1 and LED_B2
		break;
	case 5:
		reg = AEP_STATUS_5_REG; // LED_C0 and LED_C1
		break;
	case 6:
		reg = AEP_STATUS_6_REG; // LED_C2 and LED_D0
		break;
	case 7:
		reg = AEP_STATUS_7_REG; // LED_D1 and LED_D2
		break;
	default:
		return -EINVAL;
	}

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed: %d\n", reg, ret);
		return ret;
	}

	*val = (led_number % 2) ? ((reg_val >> 3) & 0x07) : (reg_val & 0x07);

	return ret;
}

int lp5812_enable_disable(struct lp5812_chip *chip, int enable)
{
	return lp5812_write(chip, chip->regs->enable_reg, (u8)enable);
}

int lp5812_reset(struct lp5812_chip *chip)
{
	return lp5812_write(chip, chip->regs->reset_reg, RESET_REG_VAL);
}

int lp5812_fault_clear(struct lp5812_chip *chip, u8 value)
{
	u8 reg_val;

	if (value == 0)
		reg_val = LOD_CLEAR_VAL;
	else if (value == 1)
		reg_val = LSD_CLEAR_VAL;
	else if (value == 2)
		reg_val = TSD_CLEAR_VAL;
	else if (value == 3)
		reg_val = FAULT_CLEAR_ALL;
	else
		return -EINVAL;

	return lp5812_write(chip, chip->regs->fault_clear_reg, reg_val);
}

void lp5812_dump_regs(struct lp5812_chip *chip, u16 from_reg, u16 to_reg)
{
	u16 reg_addr;
	u8 reg_val;

	for (reg_addr = from_reg; reg_addr <= to_reg; reg_addr++) {
		lp5812_read(chip, reg_addr, &reg_val);
		pr_info("reg addr: 0x%02X, value: 0x%02X\n", reg_addr,
				reg_val);
	}
}

int lp5812_device_command(struct lp5812_chip *chip, enum device_command command)
{
	switch (command) {
	case UPDATE:
		return lp5812_write(chip, chip->regs->update_cmd_reg,
				UPDATE_CMD_VAL);
	case START:
		return lp5812_write(chip, chip->regs->start_cmd_reg,
				START_CMD_VAL);
	case STOP:
		return lp5812_write(chip, chip->regs->stop_cmd_reg,
				STOP_CMD_VAL);
	case PAUSE:
		return lp5812_write(chip, chip->regs->pause_cmd_reg,
				PAUSE_CMD_VAL);
	case CONTINUE:
		return lp5812_write(chip, chip->regs->continue_cmd_reg,
				CONTINUE_CMD_VAL);
	default:
		return -EINVAL;
	}
}

/*
 * Function: lp5812_set_pwm_dimming_scale
 * Description: set led as pwm Linear or exponential dimming scale
 * Param: chip --> struct lp5812_chip itself
 *        led_number
 *        scale: enum type (LINEAR or EXPONENTIAL)
 * Return: 0 if success
 */
int lp5812_set_pwm_dimming_scale(struct lp5812_chip *chip, int led_number,
		enum pwm_dimming_scale scale)
{
	int ret = 0;
	u16 reg;
	u8 reg_val;

	if (led_number <= 7)
		reg = (u16)DEV_CONFIG5;
	else
		reg = (u16)DEV_CONFIG6;

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret != 0) {
		pr_info("[DEBUG]: Read register 0x%02X failed\n", reg);
		return ret;
	}
	if (scale == LINEAR) {
		reg_val &= ~(1 << (led_number % 8));
		ret = lp5812_write(chip, reg, reg_val);
		if (ret != 0) {
			pr_info("[DEBUG]: set %d to reg 0x%X failed\n",
					reg_val, reg);
			return ret;
		}
	} else {
		reg_val |= (1 << (led_number % 8));
		ret = lp5812_write(chip, reg, reg_val);
		if (ret != 0) {
			pr_info("[DEBUG]: set %d to reg 0x%X failed\n",
					reg_val, reg);
			return ret;
		}
	}

	ret = lp5812_update_regs_config(chip);

	return ret;
}

/*
 * Function: lp5812_get_pwm_dimming_scale
 * Description: get lp5812 led pwm dimming scale
 * Param: chip --> struct lp5812_chip itself
 *        led_number
 *        scale: enum type (LINEAR or EXPONENTIAL)
 * Return: 0 if success
 */
int lp5812_get_pwm_dimming_scale(struct lp5812_chip *chip,
		int led_number, enum pwm_dimming_scale *scale)
{
	int ret = 0;
	u16 reg;
	u8 reg_val;

	if (led_number < 0x8)
		reg = DEV_CONFIG5;
	else
		reg = DEV_CONFIG6;

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed: %d\n", reg, ret);
		return ret;
	}

	*scale = (reg_val & (1 << (led_number % 8))) ? EXPONENTIAL : LINEAR;

	return 0;
}

/*
 * Function: lp5812_set_phase_align
 * Description: set phase align for led (forward, middle, backward)
 * Param: chip --> struct lp5812_chip itself
 *        led_number
 *        phase_align_val: 0,1,2,3
 * Return: 0 if success
 */
int lp5812_set_phase_align(struct lp5812_chip *chip, int led_number,
		int phase_align_val)
{
	int ret;
	int bit_pos;
	u16 reg;
	u8 reg_val;

	reg = DEV_CONFIG7 + (led_number / 4);
	bit_pos = (led_number % 4) * 2;

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed: %d\n", reg, ret);
		return ret;
	}
	reg_val |= (phase_align_val << bit_pos);
	ret = lp5812_write(chip, reg, reg_val);
	if (ret != 0) {
		pr_info("[DEBUG]: set %d to reg 0x%X failed\n",
				reg_val, reg);
		return ret;
	}
	ret = lp5812_update_regs_config(chip);

	return ret;
}

/*
 * Function: lp5812_get_phase_align
 * Description: get phase align of led
 * Param: chip --> struct lp5812_chip itself
 *        led_number
 *        *phase_align_val: Return 0,1,2,3
 * Return: 0 if success
 */
int lp5812_get_phase_align(struct lp5812_chip *chip, int led_number,
		int *phase_align_val)
{
	int ret;
	int bit_pos;
	u16 reg;
	u8 reg_val;

	reg = DEV_CONFIG7 + (led_number / 4);
	bit_pos = (led_number % 4) * 2;

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed: %d\n", reg, ret);
		return ret;
	}

	*phase_align_val = (reg_val >> bit_pos) & 0x03;

	return ret;
}

/*
 * Function: lp5812_get_led_mode
 * Description: get lp5812 led mode
 * Param: chip --> struct lp5812_chip itself
 *        led_number
 *        mode: enum type (MANUAL or AUTONOMOUS)
 * Return: 0 if success
 */
int lp5812_get_led_mode(struct lp5812_chip *chip,
		int led_number, enum control_mode *mode)
{
	int ret = 0;
	u16 reg;
	u8 reg_val;

	if (led_number < 0x8)
		reg = DEV_CONFIG3;
	else
		reg = DEV_CONFIG4;

	ret = lp5812_read(chip, reg, &reg_val);
	if (ret) {
		pr_err("Read register 0x%02X failed: %d\n", reg, ret);
		return ret;
	}

	*mode = (reg_val & (1 << (led_number % 8))) ? AUTONOMOUS : MANUAL;

	return 0;
}

/*
 * Function: lp5812_set_led_mode
 * Description: set lp5812 as manual or autonomous mode
 * Param: chip --> struct lp5812_chip itself
 *        led_number
 *        mode: enum type (MANUAL or AUTONOMOUS)
 * Return: 0 if success
 */
int lp5812_set_led_mode(struct lp5812_chip *chip, int led_number,
		enum control_mode mode)
{
	int ret = 0;
	u16 reg;
	u8 reg_val;

	if (led_number <= 7)
		reg = (u16)DEV_CONFIG3;
	else
		reg = (u16)DEV_CONFIG4;
	ret = lp5812_read(chip, reg, &reg_val);
	if (ret != 0) {
		pr_info("[DEBUG]: Read register 0x%02X failed\n", reg);
		return ret;
	}
	if (mode == MANUAL) {
		reg_val &= ~(1 << (led_number % 8));
		ret = lp5812_write(chip, reg, reg_val);
		if (ret != 0) {
			pr_info("[DEBUG]: set %d to reg 0x%X failed\n",
					reg_val, reg);
			return ret;
		}
	} else {
		reg_val |= (1 << (led_number % 8));
		ret = lp5812_write(chip, reg, reg_val);
		if (ret != 0) {
			pr_info("[DEBUG]: set %d to reg 0x%X failed\n",
					reg_val, reg);
			return ret;
		}
	}

	ret = lp5812_update_regs_config(chip);

	return ret;
}

/*
 * Function: lp5812_manual_dc_pwm_control
 * Description: manual control for analog or pwm dimming type
 * Param: chip --> struct lp5812_chip itself
 *        led_number --> led_number need to control (0,1,2,3)
 *        val  --> 0 -> 255
 *        dimming_type --> enum(ANALOG, PWM)
 * Return: 0 if success
 */
int lp5812_manual_dc_pwm_control(struct lp5812_chip *chip,
		int led_number, u8 val, enum dimming_type dimming_type)
{
	int ret;
	u16 led_base_reg;

	if (dimming_type == ANALOG)
		led_base_reg = (u16)MANUAL_DC_LED_0_REG;
	else
		led_base_reg = (u16)MANUAL_PWM_LED_0_REG;
	ret = lp5812_write(chip, led_base_reg + led_number, val);
	if (ret != 0) {
		pr_info("[DEBUG]: write %d to register: %X failed\n", val,
				led_base_reg + led_number);
	}

	return ret;
}

int lp5812_manual_dc_pwm_read(struct lp5812_chip *chip,
		int led_number, u8 *val, enum dimming_type dimming_type)
{
	int ret;
	u16 led_base_reg;

	if (dimming_type == ANALOG)
		led_base_reg = (u16)MANUAL_DC_LED_0_REG;
	else
		led_base_reg = (u16)MANUAL_PWM_LED_0_REG;
	ret = lp5812_read(chip, led_base_reg + led_number, val);
	if (ret != 0) {
		pr_info("[DEBUG]: read from register: %X failed\n",
				led_base_reg + led_number);
	}

	return ret;
}

int lp5812_autonomous_dc_pwm_control(struct lp5812_chip *chip,
		int led_number, u8 val, enum dimming_type dimming_type)
{
	int ret;
	u16 led_base_reg;

	led_base_reg = (u16)AUTO_DC_LED_0_REG;
	ret = lp5812_write(chip, led_base_reg + led_number, val);
	if (ret != 0) {
		pr_info("[DEBUG]: write %d to register: %X failed\n", val,
				led_base_reg + led_number);
	}

	return ret;
}

int lp5812_autonomous_dc_pwm_read(struct lp5812_chip *chip,
		int led_number, u8 *val, enum dimming_type dimming_type)
{
	int ret;
	u16 led_base_reg;

	led_base_reg = (u16)AUTO_DC_LED_0_REG;
	ret = lp5812_read(chip, led_base_reg + led_number, val);
	if (ret != 0) {
		pr_info("[DEBUG]: read from register: %X failed\n",
				led_base_reg + led_number);
	}

	return ret;
}

int lp5812_disable_all_leds(struct lp5812_chip *chip)
{
	int ret = 0;

	ret = lp5812_write(chip, (u16)LED_ENABLE_1_REG, 0x00);
	if (ret != 0) {
		pr_info("[DEBUG]: write 0x00 to LED_ENABLE_1_REG failed\n");
		return ret;
	}
	ret = lp5812_write(chip, (u16)LED_ENABLE_2_REG, 0x00);
	if (ret != 0) {
		pr_info("[DEBUG]: write 0x00 to LED_ENABLE_2_REG failed\n");
		return ret;
	}

	return ret;
}

int lp5812_get_drive_mode_scan_order(struct lp5812_chip *chip)
{
	u8 val;
	int ret = 0;

	/* get led mode */
	ret = lp5812_read(chip, (u16)DEV_CONFIG1, &val);
	if (ret != 0) {
		pr_info("[DEBUG]: get value from reg DEV_CONFIG1 failed\n");
		return ret;
	}
	chip->u_drive_mode.drive_mode_val = val;
	pr_info("[DEBUG]: get drive_mode_val %d\n",
		chip->u_drive_mode.drive_mode_val);

	/* get scan order */
	ret = lp5812_read(chip, (u16)DEV_CONFIG2, &val);
	if (ret != 0) {
		pr_info("[DEBUG]: get value from reg DEV_CONFIG2 failed\n");
		return ret;
	}
	chip->u_scan_order.scan_order_val = val;
	pr_info("[DEBUG]: get scan_order_val: %d\n",
		chip->u_scan_order.scan_order_val);

	return ret;
}

int lp5812_set_drive_mode_scan_order(struct lp5812_chip *chip)
{
	u8 val;
	int ret = 0;

	/* Set led mode */
	val = chip->u_drive_mode.drive_mode_val;
	pr_info("[DEBUG]: set drive_mode_val: 0x%02X\n", val);
	ret = lp5812_write(chip, (u16)DEV_CONFIG1, val);
	if (ret != 0) {
		pr_info("[DEBUG]: write %d to DEV_CONFIG1 failed\n", val);
		return ret;
	}

	/* Setup scan order */
	val = chip->u_scan_order.scan_order_val;
	pr_info("[DEBUG]: set scan_order_val: 0x%02X\n", val);
	ret = lp5812_write(chip, (u16)DEV_CONFIG2, val);
	if (ret != 0) {
		pr_info("[DEBUG]: write %d to DEV_CONFIG2 failed\n", val);
		return ret;
	}

	return ret;
}

int lp5812_initialize(struct lp5812_chip *chip)
{
	int ret = 0;

	/* wait for 1 ms */
	usleep_range(1000, 1100);

	/* enable the lp5812 */
	ret = lp5812_enable_disable(chip, 1);
	if (ret != 0) {
		pr_info("[DEBUG]: lp5812_enable_disable failed\n");
		return ret;
	}
	ret = lp5812_set_drive_mode_scan_order(chip);
	if (ret != 0) {
		pr_info("[DEBUG]: %s:\n", __func__);
		pr_info("[DEBUG]:lp5812_set_drive_mode_scan_order failed\n");
		return ret;
	}

	/* Set lsd_threshold = 3h to avoid incorrect LSD detection */
	ret = lp5812_write(chip, (u16)DEV_CONFIG12, 0x0B);
	if (ret != 0) {
		pr_info("[DEBUG]: write 0x0B to DEV_CONFIG12 failed\n");
		return ret;
	}

	/* Send update command to complete configuration settings */
	ret = lp5812_update_regs_config(chip);
	if (ret != 0) {
		pr_info("[DEBUG]: lp5812_update_regs_config failed\n");
		return ret;
	}

	/* Enable LED_A0 for testing */
	ret = lp5812_write(chip, (u16)LED_ENABLE_1_REG, 0x20);
	if (ret != 0) {
		pr_info("[DEBUG]: write 0x10 to LED_ENABLE_1_REG failed\n");
		return ret;
	}
	/* set max DC current for LED_A0 */
	ret = lp5812_write(chip, (u16)0x35, 0x80);
	if (ret != 0)
		pr_info("[DEBUG]: write 0xFF to reg: 0x34 failed\n");

	/* set 100% pwm cycle for LED_A0 */
	ret = lp5812_write(chip, (u16)0x45, 0x80);
	if (ret != 0)
		pr_info("[DEBUG]: write 0xFF to reg: 0x44 failed\n");

	return ret;
}

int led_set_autonomous_animation_config(struct lp5812_led *led)
{
	int ret;
	u16 reg;
	struct lp5812_chip *chip = led->priv;

	/* Set start/end pause */
	reg = led->anim_base_addr + AUTO_PAUSE;
	ret = lp5812_write(chip, reg, led->start_stop_pause_time.time_val);
	if (ret) {
		pr_info("[DEBUG]: [%d] %s: lp5812_write failed\n", __LINE__,
				__func__);
		return ret;
	}

	/* Set led playback and AEU selection */
	reg = led->anim_base_addr + AUTO_PLAYBACK;
	ret = lp5812_write(chip, reg, led->led_playback.led_playback_val);
	if (ret) {
		pr_info("[DEBUG]: [%d] %s: lp5812_write failed\n", __LINE__,
				__func__);
		return ret;
	}

	return 0;
}

int led_get_autonomous_animation_config(struct lp5812_led *led)
{
	int ret;
	u16 reg;
	struct lp5812_chip *chip = led->priv;

	/* Get start/end pause value */
	reg = led->anim_base_addr + AUTO_PAUSE;
	ret = lp5812_read(chip, reg, &led->start_stop_pause_time.time_val);
	if (ret) {
		pr_info("[DEBUG]: [%d] %s: lp5812_read failed\n", __LINE__,
				__func__);
		return ret;
	}

	/* Get led playback and AEU selection values */
	reg = led->anim_base_addr + AUTO_PLAYBACK;
	ret = lp5812_read(chip, reg, &led->led_playback.led_playback_val);
	if (ret) {
		pr_info("[DEBUG]: [%d] %s: lp5812_read failed\n", __LINE__,
				__func__);
		return ret;
	}

	return 0;
}

static u16 get_aeu_pwm_register(struct anim_engine_unit *aeu,
		enum pwm_slope_time_num pwm_num)
{
	u16 reg;
	struct lp5812_led *led = aeu->led;

	switch (pwm_num) {
	case PWM1:
		if (aeu->aeu_number == 1)
			reg = led->anim_base_addr + AEU1_PWM_1;
		else if (aeu->aeu_number == 2)
			reg = led->anim_base_addr + AEU2_PWM_1;
		else
			reg = led->anim_base_addr + AEU3_PWM_1;
		break;
	case PWM2:
		if (aeu->aeu_number == 1)
			reg = led->anim_base_addr + AEU1_PWM_2;
		else if (aeu->aeu_number == 2)
			reg = led->anim_base_addr + AEU2_PWM_2;
		else
			reg = led->anim_base_addr + AEU3_PWM_2;
		break;
	case PWM3:
		if (aeu->aeu_number == 1)
			reg = led->anim_base_addr + AEU1_PWM_3;
		else if (aeu->aeu_number == 2)
			reg = led->anim_base_addr + AEU2_PWM_3;
		else
			reg = led->anim_base_addr + AEU3_PWM_3;
		break;
	case PWM4:
		if (aeu->aeu_number == 1)
			reg = led->anim_base_addr + AEU1_PWM_4;
		else if (aeu->aeu_number == 2)
			reg = led->anim_base_addr + AEU2_PWM_4;
		else
			reg = led->anim_base_addr + AEU3_PWM_4;
		break;
	case PWM5:
		if (aeu->aeu_number == 1)
			reg = led->anim_base_addr + AEU1_PWM_5;
		else if (aeu->aeu_number == 2)
			reg = led->anim_base_addr + AEU2_PWM_5;
		else
			reg = led->anim_base_addr + AEU3_PWM_5;
		break;
	default:
		reg = led->anim_base_addr;
		break;
	}

	return reg;
}

static u16 get_aeu_slope_time_register(struct anim_engine_unit *aeu,
		enum pwm_slope_time_num slope_time_num)
{
	u16 reg;
	struct lp5812_led *led = aeu->led;

	switch (slope_time_num) {
	case SLOPE_T1:
	case SLOPE_T2:
		if (aeu->aeu_number == 1)
			reg = led->anim_base_addr + AEU1_T12;
		else if (aeu->aeu_number == 2)
			reg = led->anim_base_addr + AEU2_T12;
		else
			reg = led->anim_base_addr + AEU3_T12;
		break;
	case SLOPE_T3:
	case SLOPE_T4:
		if (aeu->aeu_number == 1)
			reg = led->anim_base_addr + AEU1_T34;
		else if (aeu->aeu_number == 2)
			reg = led->anim_base_addr + AEU2_T34;
		else
			reg = led->anim_base_addr + AEU3_T34;
		break;
	default:
		reg = led->anim_base_addr;
		break;
	}

	return reg;
}

static u16 get_aeu_playback_time_register(struct anim_engine_unit *aeu)
{
	u16 reg;
	struct lp5812_led *led = aeu->led;

	if (aeu->aeu_number == 1)
		reg = led->anim_base_addr + AEU1_PLAYBACK;
	else if (aeu->aeu_number == 2)
		reg = led->anim_base_addr + AEU2_PLAYBACK;
	else
		reg = led->anim_base_addr + AEU3_PLAYBACK;

	return reg;
}

/* Function: led_aeu_pwm_set_val
 * Description: set led AEU pwm value
 * Params: aeu -> struct anim_engine_unit itself
 *         val: 0-255
 *         pwm_num: enum(PWM1 ... PWM5)
 * Return 0 if success
 */
int led_aeu_pwm_set_val(struct anim_engine_unit *aeu, u8 val,
		enum pwm_slope_time_num pwm_num)
{
	int ret;
	u16 reg;
	struct lp5812_led *led = aeu->led;
	struct lp5812_chip *chip = led->priv;

	reg = get_aeu_pwm_register(aeu, pwm_num);
	ret = lp5812_write(chip, reg, val);
	if (ret != 0)
		pr_info("[DEBUG]: %s: lp5812_write failed\n", __func__);

	return ret;
}

/* Function: led_aeu_pwm_get_val
 * Description: get led AEU pwm value
 * Params: aeu -> struct anim_engine_unit itself
 *         val: return back 0-255
 *         pwm_num: enum(PWM1 ... PWM5)
 * Return 0 if success
 */
int led_aeu_pwm_get_val(struct anim_engine_unit *aeu, u8 *val,
		enum pwm_slope_time_num pwm_num)
{
	int ret;
	u16 reg;
	struct lp5812_led *led = aeu->led;
	struct lp5812_chip *chip = led->priv;

	reg = get_aeu_pwm_register(aeu, pwm_num);
	ret = lp5812_read(chip, reg, val);
	if (ret != 0)
		pr_info("[DEBUG]: %s: lp5812_read failed\n", __func__);

	return ret;
}

/* Function: led_aeu_slope_time_set_val
 * Description: set led AEU slope time value
 * Params: aeu -> struct anim_engine_unit itself
 *         val: 0 -> 15
 *         slope_time_num: enum(SLOPE_T1 ... SLOPE_T4)
 * Return 0 if success
 */
int led_aeu_slope_time_set_val(struct anim_engine_unit *aeu, u8 val,
		enum pwm_slope_time_num slope_time_num)
{
	int ret;
	u16 reg;
	union time slope_time_val;
	struct lp5812_led *led = aeu->led;
	struct lp5812_chip *chip = led->priv;

	reg = get_aeu_slope_time_register(aeu, slope_time_num);

	/* get original value of slope time */
	ret = lp5812_read(chip, reg, &slope_time_val.time_val);
	if (ret != 0) {
		pr_info("[DEBUG]: %s: lp5812_read failed\n", __func__);
		return ret;
	}

	/* Update new value for slope time*/
	if (slope_time_num == SLOPE_T1 || slope_time_num == SLOPE_T3)
		slope_time_val.s_time.first = val;
	if (slope_time_num == SLOPE_T2 || slope_time_num == SLOPE_T4)
		slope_time_val.s_time.second = val;

	/* Save updated value to hardware */
	ret = lp5812_write(chip, reg, slope_time_val.time_val);
	if (ret != 0)
		pr_info("[DEBUG]: %s: lp5812_write failed\n", __func__);

	return ret;
}

/* Function: led_aeu_slope_time_get_val
 * Description: get led AEU slope time value
 * Params: aeu -> struct anim_engine_unit itself
 *         val: return back 0 -> 15
 *         slope_time_num: enum(SLOPE_T1 ... SLOPE_T4)
 * Return 0 if success
 */
int led_aeu_slope_time_get_val(struct anim_engine_unit *aeu, u8 *val,
		enum pwm_slope_time_num slope_time_num)
{
	int ret = 0;
	u16 reg;
	union time slope_time_val;
	struct lp5812_led *led = aeu->led;
	struct lp5812_chip *chip = led->priv;

	reg = get_aeu_slope_time_register(aeu, slope_time_num);
	/* get slope time value */
	ret = lp5812_read(chip, reg, &slope_time_val.time_val);
	if (ret != 0) {
		pr_info("[DEBUG]: %s: lp5812_read failed\n", __func__);
		return ret;
	}

	if (slope_time_num == SLOPE_T1 || slope_time_num == SLOPE_T3)
		*val = slope_time_val.s_time.first;

	if (slope_time_num == SLOPE_T2 || slope_time_num == SLOPE_T4)
		*val = slope_time_val.s_time.second;

	return ret;
}

/* Function: led_aeu_playback_time_set_val
 * Description: set aeu playback time value
 * Params: aeu -> struct anim_engine_unit itself
 *         val: 0 -> 3
 * Return 0 if success
 */
int led_aeu_playback_time_set_val(struct anim_engine_unit *aeu, u8 val)
{
	int ret;
	u16 reg;
	struct lp5812_led *led = aeu->led;
	struct lp5812_chip *chip = led->priv;

	reg = get_aeu_playback_time_register(aeu);
	ret = lp5812_write(chip, reg, val);
	if (ret != 0)
		pr_info("[DEBUG]: %s: lp5812_write failed\n", __func__);

	return ret;
}

/* Function: led_aeu_playback_time_get_val
 * Description: get aeu playback time value
 * Params: aeu -> struct anim_engine_unit itself
 *         val: return back 0 -> 3
 * Return 0 if success
 */
int led_aeu_playback_time_get_val(struct anim_engine_unit *aeu, u8 *val)
{
	int ret;
	u16 reg;
	struct lp5812_led *led = aeu->led;
	struct lp5812_chip *chip = led->priv;

	reg = get_aeu_playback_time_register(aeu);
	ret = lp5812_read(chip, reg, val);
	if (ret != 0)
		pr_info("[DEBUG]: %s: lp5812_read failed\n", __func__);

	return ret;
}

MODULE_DESCRIPTION("Texas Instruments LP5812 Common Driver");
MODULE_AUTHOR("Jared Zhou");
MODULE_LICENSE("GPL");
