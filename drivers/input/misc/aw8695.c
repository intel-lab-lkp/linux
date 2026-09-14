// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Luca Weiss <luca.weiss@fairphone.com>
 * Copyright (C) 2026 Griffin Kroah-Hartman <griffin.kroah@fairphone.com>
 *
 * Partially based on vendor driver:
 *   Copyright (c) 2018 AWINIC Technology CO., LTD
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define AW8695_CHIPID				0x95
#define AW8695_RESET				0xaa
/* Default of BASE_ADDR* registers */
#define AW8695_RAM_BASE_ADDR			0x800

#define AW8695_HIGH_MASK			GENMASK(15, 8)
#define AW8695_LOW_MASK				GENMASK(7, 0)

/* Chip ID */
#define AW8695_ID_REG				0x00

/* System Status */
#define AW8695_SYSST_REG			0x01

/* System Interrupt */
#define AW8695_SYSINT_REG			0x02
#define AW8695_SYSINT_BSTERRI			BIT(7)
#define AW8695_SYSINT_OVI			BIT(6)
#define AW8695_SYSINT_UVLI			BIT(5)
#define AW8695_SYSINT_FF_AEI			BIT(4)
#define AW8695_SYSINT_FF_AFI			BIT(3)
#define AW8695_SYSINT_OCDI			BIT(2)
#define AW8695_SYSINT_OTI			BIT(1)
#define AW8695_SYSINT_DONEI			BIT(0)

/* System Interrupt Mask */
#define AW8695_SYSINTM_REG			0x03
#define AW8695_SYSINTM_BSTERR_OFF		BIT(7)
#define AW8695_SYSINTM_OV_OFF			BIT(6)
#define AW8695_SYSINTM_UVLO_OFF			BIT(5)
#define AW8695_SYSINTM_OCD_OFF			BIT(2)
#define AW8695_SYSINTM_OT_OFF			BIT(1)

/* System Control */
#define AW8695_SYSCTRL_REG			0x04
#define AW8695_SYSCTRL_RAMINIT_EN		BIT(5)

#define AW8695_SYSCTRL_PLAY_MODE_MASK		GENMASK(3, 2)
#define AW8695_SYSCTRL_PLAY_MODE_RAM		(0)

#define AW8695_SYSCTRL_BST_MODE_MASK		GENMASK(1, 1)
#define AW8695_SYSCTRL_BST_MODE_BYPASS		(0)

#define AW8695_SYSCTRL_WORK_MODE_MASK		GENMASK(0, 0)
#define AW8695_SYSCTRL_STANDBY			(1)
#define AW8695_SYSCTRL_ACTIVE			(0)

/* Process Control */
#define AW8695_GO_REG				0x05
#define AW8695_GO_ENABLE			BIT(0)

/* Waveform #1 */
#define AW8695_WAVSEQ1_REG			0x07

/* Waveform #2 */
#define AW8695_WAVSEQ2_REG			0x08

/* Waveform Loop #1 */
#define AW8695_WAVLOOP1_REG			0x0f
#define AW8695_WAVLOOP1_SEQ1_MASK		GENMASK(7, 4)
#define AW8695_WAVLOOP1_SEQ2_MASK		GENMASK(3, 0)

#define AW8695_WAVLOOP_INFINITE			0xf

/* Debug Control */
#define AW8695_DBGCTRL_REG				0x20
#define AW8695_DBGCTRL_INT_MODE_MASK		GENMASK(2, 2)
#define AW8695_DBGCTRL_INT_MODE_EDGE		(1)

/* PWM Output Protect Configuration */
#define AW8695_PWMPRC_REG			0x2d
#define AW8695_PWMPRC_PRC_ENABLE		BIT(7)

/* PWM Debug */
#define AW8695_PWMDBG_REG			0x2e
#define AW8695_PWMDBG_PWM_MODE_MASK		GENMASK(6, 5)
#define AW8695_PWMDBG_PWM_24K			(2)

/* Debug Status */
#define AW8695_DBGSTAT_REG			0x30

/* Boost Debug #1 */
#define AW8695_BSTDBG1_REG			0x31
#define AW8695_BSTDBG1_DEFAULT			0x30

/* Boost Debug #2 */
#define AW8695_BSTDBG2_REG			0x32
#define AW8695_BSTDBG2_DEFAULT			0xeb

/* Boost Debug #3 */
#define AW8695_BSTDBG3_REG			0x33
#define AW8695_BSTDBG3_DEFAULT			0xd4

/* Boost Config */
#define AW8695_BSTCFG_REG			0x34
#define AW8695_BSTCFG_PEAKCUR_MASK		GENMASK(2, 0)
#define AW8695_BSTCFG_PEAKCUR_2A		(1)

#define AW8695_ANADBG_REG			0x35
#define AW8695_ANADBG_IOC_MASK			GENMASK(3, 2)
#define AW8695_ANADBG_IOC_4P65A			(3)

/* Waveform Protect Level */
#define AW8695_PRLVL_REG			0x3e
#define AW8695_PRLVL_PR_ENABLE			BIT(7)

/* SRAM Address 0xhigh */
#define AW8695_RAMADDRH_REG			0x40

/* SRAM Address 0xlow */
#define AW8695_RAMADDRL_REG			0x41

/* SRAM Data */
#define AW8695_RAMDATA_REG			0x42

#define AW8695_GLB_STATE_REG			0x46

#define AW8695_BST_AUTO_REG			0x47
#define AW8695_BST_AUTO_BST_AUTOSW_MASK		GENMASK(2, 2)
#define AW8695_BST_AUTO_BST_MANUAL_BOOST	(0)

#define AW8695_TSET_REG				0x4d
#define AW8695_TSET_DEFAULT			0x12

#define AW8695_R_SPARE_REG			0x5d
#define Aw8695_R_SPARE_DEFAULT			0x68

/* Detection Control */
#define AW8695_DETCTRL_REG			0x5f
#define AW8695_DETCTRL_PROTECT_MASK		GENMASK(5, 5)
#define AW8695_DETCTRL_PROTECT_NO_ACTION	(1)
#define AW8695_DETCTRL_DIAG_GO_ENABLE		BIT(0)

/* ADC Test */
#define AW8695_ADCTEST_REG			0x66
#define AW8695_ADCTEST_VBAT_MODE_MASK		GENMASK(6, 6)
#define AW8695_ADCTEST_VBAT_HW_COMP		(1)

#define AW8695_BEMF_VTHH_H_REG			0x74

#define AW8695_BEMF_VTHH_L_REG			0x75

#define AW8695_BEMF_VTHL_H_REG			0x76

#define AW8695_BEMF_VTHL_L_REG			0x77

#define AW8695_MAX_REG				0x7f

#define AW8695_BEMF_UPPER_THRESHOLD		0x1008
#define AW8695_BEMF_LOWER_THRESHOLD		0x3f8

enum aw8695_work_mode {
	AW8695_STANDBY_MODE,
	AW8695_RAM_MODE,
};

struct aw8695_data {
	struct input_dev *input_dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	u16 level;
	struct work_struct play_work;
};

/*
 * Sine wave representing the magnitude of the drive to be used.
 * Data is encoded in two's complement.
 *   round(84 * sin(x / 16.25))
 */
static const u8 aw8695_sine_waveform[] = {
	0x00, 0x05, 0x0a, 0x0f, 0x14, 0x19, 0x1e, 0x23, 0x28, 0x2c, 0x30, 0x35,
	0x39, 0x3c, 0x40, 0x43, 0x46, 0x49, 0x4b, 0x4d, 0x4f, 0x51, 0x52, 0x53,
	0x54, 0x54, 0x54, 0x54, 0x53, 0x52, 0x51, 0x4f, 0x4d, 0x4b, 0x49, 0x46,
	0x43, 0x40, 0x3c, 0x39, 0x35, 0x31, 0x2c, 0x28, 0x23, 0x1f, 0x1a, 0x15,
	0x10, 0x0b, 0x05, 0x00, 0xfb, 0xf6, 0xf1, 0xec, 0xe7, 0xe2, 0xdd, 0xd9,
	0xd4, 0xd0, 0xcc, 0xc8, 0xc4, 0xc0, 0xbd, 0xba, 0xb7, 0xb5, 0xb3, 0xb1,
	0xaf, 0xae, 0xad, 0xac, 0xac, 0xac, 0xac, 0xad, 0xae, 0xaf, 0xb1, 0xb2,
	0xb5, 0xb7, 0xba, 0xbd, 0xc0, 0xc3, 0xc7, 0xcb, 0xcf, 0xd3, 0xd8, 0xdc,
	0xe1, 0xe6, 0xeb, 0xf0, 0xf5, 0xfa
};

/*
 * Header that gets written to AW8695 SRAM that describes the available
 * waveforms being transferred afterwards.
 *
 * @version: waveform library version
 * @start_address: start address of waveform in SRAM
 * @end_address: end address of waveform in SRAM
 */
struct aw8695_sram_waveform_header {
	u8 version;
	__be16 start_address;
	__be16 end_address;
} __packed;

static const struct aw8695_sram_waveform_header sram_waveform_header = {
	.version = 0x01,
	.start_address = cpu_to_be16(AW8695_RAM_BASE_ADDR +
		sizeof(struct aw8695_sram_waveform_header)),
	.end_address = cpu_to_be16(AW8695_RAM_BASE_ADDR +
		sizeof(struct aw8695_sram_waveform_header) +
		ARRAY_SIZE(aw8695_sine_waveform) - 1),
};

static int aw8695_interrupt_clear(struct aw8695_data *haptics)
{
	unsigned int read_buf;

	/* Clear UVLI bit by reading register */
	return regmap_read(haptics->regmap, AW8695_SYSINT_REG, &read_buf);
}

static int aw8695_haptic_set_active(struct aw8695_data *haptics)
{
	int err;

	err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL_REG,
				 AW8695_SYSCTRL_WORK_MODE_MASK,
				 FIELD_PREP(AW8695_SYSCTRL_WORK_MODE_MASK,
					    AW8695_SYSCTRL_ACTIVE));
	if (err)
		return err;

	err = aw8695_interrupt_clear(haptics);
	if (err)
		return err;

	return regmap_update_bits(haptics->regmap, AW8695_SYSINTM_REG,
				  AW8695_SYSINTM_UVLO_OFF, 0);
}

static int aw8695_play_mode(struct aw8695_data *haptics,
				 enum aw8695_work_mode mode)
{
	struct device *dev = &haptics->client->dev;
	int err;

	switch (mode) {
	case AW8695_STANDBY_MODE:
		err = regmap_update_bits(haptics->regmap, AW8695_SYSINTM_REG,
					 AW8695_SYSINTM_UVLO_OFF, AW8695_SYSINTM_UVLO_OFF);
		if (err)
			return err;

		return regmap_update_bits(haptics->regmap, AW8695_SYSCTRL_REG,
					  AW8695_SYSCTRL_WORK_MODE_MASK,
					  FIELD_PREP(AW8695_SYSCTRL_WORK_MODE_MASK,
						     AW8695_SYSCTRL_STANDBY));
	case AW8695_RAM_MODE:
		err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL_REG,
					 AW8695_SYSCTRL_PLAY_MODE_MASK,
					 FIELD_PREP(AW8695_SYSCTRL_PLAY_MODE_MASK,
						    AW8695_SYSCTRL_PLAY_MODE_RAM));
		if (err)
			return err;

		err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL_REG,
					 AW8695_SYSCTRL_BST_MODE_MASK,
					 FIELD_PREP(AW8695_SYSCTRL_BST_MODE_MASK,
						    AW8695_SYSCTRL_BST_MODE_BYPASS));
		if (err)
			return err;

		return aw8695_haptic_set_active(haptics);
	default:
		dev_err(dev, "Unhandled mode: %d\n", mode);
		return -EINVAL;
	}
}

static int aw8695_haptics_play(struct input_dev *dev, void *data,
			       struct ff_effect *effect)
{
	struct aw8695_data *haptics = input_get_drvdata(dev);
	int level;

	level = effect->u.rumble.strong_magnitude;
	if (!level)
		level = effect->u.rumble.weak_magnitude;

	if (haptics->level == level)
		return 0;

	haptics->level = level;
	schedule_work(&haptics->play_work);

	return 0;
}

static void aw8695_hw_reset(struct aw8695_data *haptics)
{
	gpiod_set_value_cansleep(haptics->reset_gpio, 1);

	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(haptics->reset_gpio, 0);

	usleep_range(3500, 4000);
}

static int aw8695_stop(struct aw8695_data *haptics)
{
	int err;
	unsigned int read_buf;
	struct device *dev = &haptics->client->dev;

	err = regmap_update_bits(haptics->regmap, AW8695_GO_REG,
				 AW8695_GO_ENABLE, 0);
	if (err)
		return err;

	err = regmap_read_poll_timeout(haptics->regmap, AW8695_GLB_STATE_REG, read_buf,
			(read_buf & 0x0f) == 0, 2000, 2000 * 100);
	if (err) {
		dev_err(dev, "Did not enter standby: %d\n	Trying to force it...\n", err);
		err = aw8695_play_mode(haptics, AW8695_STANDBY_MODE);
		return err;
	}

	return aw8695_play_mode(haptics, AW8695_STANDBY_MODE);
}

static int aw8695_play_sine(struct aw8695_data *haptics)
{
	int err;

	err = aw8695_stop(haptics);
	if (err)
		return err;

	/*
	 * Configure for waveform #1 to be played infinitely,
	 * and waveform #2 to not be played.
	 */
	err = regmap_write(haptics->regmap, AW8695_WAVSEQ1_REG, 0x1);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8695_WAVSEQ2_REG, 0x0);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8695_WAVLOOP1_REG,
			   FIELD_PREP(AW8695_WAVLOOP1_SEQ1_MASK,
				      AW8695_WAVLOOP_INFINITE) |
			   FIELD_PREP(AW8695_WAVLOOP1_SEQ2_MASK, 0));
	if (err)
		return err;

	/* Configure for RAM mode */
	err = aw8695_play_mode(haptics, AW8695_RAM_MODE);
	if (err)
		return err;

	/* Start vibration */
	return regmap_update_bits(haptics->regmap, AW8695_GO_REG,
				  AW8695_GO_ENABLE, AW8695_GO_ENABLE);
}

static void aw8695_close(struct input_dev *input)
{
	struct aw8695_data *haptics = input_get_drvdata(input);
	struct device *dev = &haptics->client->dev;
	int err;

	cancel_work_sync(&haptics->play_work);
	err = aw8695_stop(haptics);
	if (err)
		dev_err(dev, "Failed to stop haptics: %d\n", err);
}

static void aw8695_haptics_play_work(struct work_struct *work)
{
	struct aw8695_data *haptics =
		container_of(work, struct aw8695_data, play_work);
	struct device *dev = &haptics->client->dev;
	int err;

	if (haptics->level)
		err = aw8695_play_sine(haptics);
	else
		err = aw8695_stop(haptics);

	if (err)
		dev_err(dev, "Failed to execute work command: %d\n", err);
}

static int aw8695_haptic_offset_calibration(struct aw8695_data *haptics)
{
	unsigned int read_buf;
	int err;

	err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL_REG,
				 AW8695_SYSCTRL_RAMINIT_EN,
				 AW8695_SYSCTRL_RAMINIT_EN);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8695_DETCTRL_REG,
				 AW8695_DETCTRL_DIAG_GO_ENABLE,
				 AW8695_DETCTRL_DIAG_GO_ENABLE);
	if (err)
		return err;

	err = regmap_read_poll_timeout(haptics->regmap, AW8695_DETCTRL_REG, read_buf,
			(read_buf & AW8695_DETCTRL_DIAG_GO_ENABLE) == 0, 10000, 10000 * 50);
	if (err)
		return err;

	return regmap_update_bits(haptics->regmap, AW8695_SYSCTRL_REG,
				  AW8695_SYSCTRL_RAMINIT_EN, 0);
}

static int aw8695_init(struct aw8695_data *haptics)
{
	int err;
	unsigned int read_buf;
	struct device *dev = &haptics->client->dev;

	aw8695_hw_reset(haptics);

	err = regmap_read(haptics->regmap, AW8695_ID_REG, &read_buf);
	if (err) {
		dev_err(dev, "Failed to read ID register: %d\n", err);
		return err;
	}

	if (read_buf != AW8695_CHIPID) {
		dev_err(dev, "Chip ID mismatch: expected %x, got %x\n",
			AW8695_CHIPID, read_buf);
		return -ENODEV;
	}

	err = regmap_write(haptics->regmap, AW8695_ID_REG, AW8695_RESET);
	if (err) {
		dev_err(dev, "Failed to reset: %d\n", err);
		return err;
	}

	/* Wait ~1ms after reset */
	usleep_range(1000, 1500);

	/* Clear UVLI bit by reading register */
	err = aw8695_interrupt_clear(haptics);
	if (err) {
		dev_err(dev, "Failed to clear interrupt: %d\n", err);
		return err;
	}

	/* Set interrupt mode to edge */
	err = regmap_update_bits(haptics->regmap, AW8695_DBGCTRL_REG,
				 AW8695_DBGCTRL_INT_MODE_MASK,
				 FIELD_PREP(AW8695_DBGCTRL_INT_MODE_MASK,
					    AW8695_DBGCTRL_INT_MODE_EDGE));
	if (err) {
		dev_err(dev, "Failed to set interrupt mode: %d\n", err);
		return err;
	}

	/* Configure interrupts */
	err = regmap_update_bits(haptics->regmap, AW8695_SYSINTM_REG,
				 AW8695_SYSINTM_BSTERR_OFF | AW8695_SYSINTM_OV_OFF |
				 AW8695_SYSINTM_UVLO_OFF | AW8695_SYSINTM_OCD_OFF |
				 AW8695_SYSINTM_OT_OFF,
				 AW8695_SYSINTM_BSTERR_OFF);
	if (err)
		return err;

	err = aw8695_play_mode(haptics, AW8695_STANDBY_MODE);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8695_PWMDBG_REG,
				 AW8695_PWMDBG_PWM_MODE_MASK,
				 FIELD_PREP(AW8695_PWMDBG_PWM_MODE_MASK,
					    AW8695_PWMDBG_PWM_24K));
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8695_BSTDBG1_REG, AW8695_BSTDBG1_DEFAULT);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_BSTDBG2_REG, AW8695_BSTDBG2_DEFAULT);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_BSTDBG3_REG, AW8695_BSTDBG3_DEFAULT);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_TSET_REG, AW8695_TSET_DEFAULT);
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_R_SPARE_REG, Aw8695_R_SPARE_DEFAULT);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8695_ANADBG_REG,
				 AW8695_ANADBG_IOC_MASK,
				 FIELD_PREP(AW8695_ANADBG_IOC_MASK,
					    AW8695_ANADBG_IOC_4P65A));
	if (err)
		return err;

	/* Set boost peak current */
	err = regmap_update_bits(haptics->regmap, AW8695_BSTCFG_REG,
				 AW8695_BSTCFG_PEAKCUR_MASK,
				 FIELD_PREP(AW8695_BSTCFG_PEAKCUR_MASK,
					    AW8695_BSTCFG_PEAKCUR_2A));
	if (err)
		return err;

	/* Adjust motorprotect config */
	err = regmap_update_bits(haptics->regmap, AW8695_DETCTRL_REG,
				 AW8695_DETCTRL_PROTECT_MASK,
				 FIELD_PREP(AW8695_DETCTRL_PROTECT_MASK,
					    AW8695_DETCTRL_PROTECT_NO_ACTION));
	if (err)
		return err;
	err = regmap_update_bits(haptics->regmap, AW8695_PWMPRC_REG,
				 AW8695_PWMPRC_PRC_ENABLE, 0);
	if (err)
		return err;
	err = regmap_update_bits(haptics->regmap, AW8695_PRLVL_REG,
				 AW8695_PRLVL_PR_ENABLE, 0);
	if (err)
		return err;

	/* Adjust auto boost config */
	err = regmap_update_bits(haptics->regmap, AW8695_BST_AUTO_REG,
				 AW8695_BST_AUTO_BST_AUTOSW_MASK,
				 FIELD_PREP(AW8695_BST_AUTO_BST_AUTOSW_MASK,
					    AW8695_BST_AUTO_BST_MANUAL_BOOST));
	if (err)
		return err;

	err = aw8695_haptic_offset_calibration(haptics);
	if (err)
		return err;

	/* Set vbat compensation mode */
	err = regmap_update_bits(haptics->regmap, AW8695_ADCTEST_REG,
				 AW8695_ADCTEST_VBAT_MODE_MASK,
				 FIELD_PREP(AW8695_ADCTEST_VBAT_MODE_MASK,
					    AW8695_ADCTEST_VBAT_HW_COMP));
	if (err)
		return err;

	/* bemf config */
	err = regmap_write(haptics->regmap, AW8695_BEMF_VTHH_H_REG,
			   FIELD_GET(AW8695_HIGH_MASK, AW8695_BEMF_UPPER_THRESHOLD));
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_BEMF_VTHH_L_REG,
			   FIELD_GET(AW8695_LOW_MASK, AW8695_BEMF_UPPER_THRESHOLD));
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_BEMF_VTHL_H_REG,
			   FIELD_GET(AW8695_HIGH_MASK, AW8695_BEMF_LOWER_THRESHOLD));
	if (err)
		return err;
	return regmap_write(haptics->regmap, AW8695_BEMF_VTHL_L_REG,
			    FIELD_GET(AW8695_LOW_MASK, AW8695_BEMF_LOWER_THRESHOLD));
}

static int aw8695_ram_init(struct aw8695_data *haptics)
{
	int err;

	/* Enable SRAM init */
	err = regmap_update_bits(haptics->regmap, AW8695_SYSCTRL_REG,
				 AW8695_SYSCTRL_RAMINIT_EN, AW8695_SYSCTRL_RAMINIT_EN);
	if (err)
		return err;

	/* Set RAMDATA write address */
	err = regmap_write(haptics->regmap, AW8695_RAMADDRH_REG,
			   FIELD_GET(AW8695_HIGH_MASK, AW8695_RAM_BASE_ADDR));
	if (err)
		return err;
	err = regmap_write(haptics->regmap, AW8695_RAMADDRL_REG,
			   FIELD_GET(AW8695_LOW_MASK, AW8695_RAM_BASE_ADDR));
	if (err)
		return err;

	err = regmap_noinc_write(haptics->regmap, AW8695_RAMDATA_REG,
				 &sram_waveform_header, sizeof(sram_waveform_header));
	if (err)
		return err;

	err = regmap_noinc_write(haptics->regmap, AW8695_RAMDATA_REG,
				  aw8695_sine_waveform, ARRAY_SIZE(aw8695_sine_waveform));
	if (err)
		return err;

	/* Disable SRAM init */
	return regmap_update_bits(haptics->regmap, AW8695_SYSCTRL_REG,
				  AW8695_SYSCTRL_RAMINIT_EN, 0);
}

static irqreturn_t aw8695_irq(int irq, void *data)
{
	struct aw8695_data *haptics = data;
	struct device *dev = &haptics->client->dev;
	unsigned int read_buf;
	int err;

	err = regmap_read(haptics->regmap, AW8695_SYSINT_REG, &read_buf);
	if (err) {
		dev_err(dev, "Failed to read SYSINT register: %d\n", err);
		return IRQ_NONE;
	}
	dev_dbg(dev, "Interrupt: SYSINT=0x%x\n", read_buf);

	if (read_buf & AW8695_SYSINT_BSTERRI)
		dev_err(dev, "Received boost short circuit protection or over-voltage protection interrupt!\n");
	if (read_buf & AW8695_SYSINT_OVI)
		dev_err(dev, "Received wave data overflow or DPWM DC error interrupt!\n");
	if (read_buf & AW8695_SYSINT_UVLI)
		dev_err(dev, "Received under voltage lock out interrupt!\n");
	if (read_buf & AW8695_SYSINT_OCDI)
		dev_err(dev, "Received over current interrupt!\n");
	if (read_buf & AW8695_SYSINT_OTI)
		dev_err(dev, "Received over temperature interrupt!\n");

	if (read_buf & AW8695_SYSINT_DONEI)
		dev_dbg(dev, "Received playback done interrupt\n");
	/* FIFO mode is not (yet) implemented in this driver */
	if (read_buf & AW8695_SYSINT_FF_AEI)
		dev_dbg(dev, "Received FIFO almost empty interrupt\n");
	if (read_buf & AW8695_SYSINT_FF_AFI)
		dev_dbg(dev, "Received FIFO almost full interrupt\n");

	err = regmap_read(haptics->regmap, AW8695_DBGSTAT_REG, &read_buf);
	if (err) {
		dev_err(dev, "Failed to read DBGSTAT register: %d\n", err);
		return IRQ_NONE;
	}
	dev_dbg(dev, "Interrupt: DBGSTAT=0x%x\n", read_buf);

	err = regmap_read(haptics->regmap, AW8695_SYSST_REG, &read_buf);
	if (err) {
		dev_err(dev, "Failed to read SYSST register: %d\n", err);
		return IRQ_NONE;
	}
	dev_dbg(dev, "Interrupt: SYSST=0x%x\n", read_buf);

	return IRQ_HANDLED;
}

static const struct regmap_config aw8695_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = AW8695_MAX_REG,
	.cache_type = REGCACHE_NONE,
};

static int aw8695_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw8695_data *haptics;
	int err;

	haptics = devm_kzalloc(dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	haptics->client = client;
	i2c_set_clientdata(client, haptics);

	haptics->regmap = devm_regmap_init_i2c(client, &aw8695_regmap_config);
	if (IS_ERR(haptics->regmap))
		return dev_err_probe(dev, PTR_ERR(haptics->regmap),
				     "Failed to allocate register map\n");

	haptics->input_dev = devm_input_allocate_device(dev);
	if (!haptics->input_dev)
		return -ENOMEM;

	haptics->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(haptics->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(haptics->reset_gpio),
				     "Failed to get reset gpio\n");

	err = devm_request_threaded_irq(dev, client->irq, NULL, aw8695_irq,
		IRQF_ONESHOT, NULL, haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to request interrupt\n");

	INIT_WORK(&haptics->play_work, aw8695_haptics_play_work);

	haptics->input_dev->name = "aw8695";
	haptics->input_dev->close = aw8695_close;

	input_set_drvdata(haptics->input_dev, haptics);
	input_set_capability(haptics->input_dev, EV_FF, FF_RUMBLE);

	err = input_ff_create_memless(haptics->input_dev, NULL,
				      aw8695_haptics_play);
	if (err)
		return dev_err_probe(dev, err, "Failed to create FF dev\n");

	err = aw8695_init(haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to init aw8695\n");

	err = aw8695_ram_init(haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to init aw8695 sram\n");

	err = input_register_device(haptics->input_dev);
	if (err)
		return dev_err_probe(dev, err, "Failed to register input device\n");

	return 0;
}

static const struct of_device_id aw8695_of_id[] = {
	{ .compatible = "awinic,aw8695", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, aw8695_of_id);

static struct i2c_driver aw8695_driver = {
	.driver = {
		.name = "aw8695",
		.of_match_table = aw8695_of_id,
	},
	.probe = aw8695_probe,
};

module_i2c_driver(aw8695_driver);

MODULE_AUTHOR("Luca Weiss <luca.weiss@fairphone.com>");
MODULE_DESCRIPTION("AW8695 LRA Haptic Driver");
MODULE_LICENSE("GPL");
