// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Armandas Kvietkus <armandas.kvietkus@proton.me>
 * Copyright (C) 2025 Griffin Kroah-Hartman <griffin.kroah@fairphone.com>
 * Copyright (c) 2021 AWINIC Technology CO., LTD
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/math.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define AW8697_ID_REG				0x00
#define AW8697_ID_SOFTRST			0xaa
#define AW8697_CHIPID				0x97

#define AW8697_SYSINT_REG			0x02
#define AW8697_SYSINT_BSTERRI			BIT(7)
#define AW8697_SYSINT_OVI			BIT(6)
#define AW8697_SYSINT_UVLI			BIT(5)
#define AW8697_SYSINT_FF_AEI			BIT(4)
#define AW8697_SYSINT_FF_AFI			BIT(3)
#define AW8697_SYSINT_OCDI			BIT(2)
#define AW8697_SYSINT_OTI			BIT(1)
#define AW8697_SYSINT_DONEI			BIT(0)

#define AW8697_SYSINTM_REG			0x03
#define AW8697_SYSINTM_BSTERRM			BIT(7)
#define AW8697_SYSINTM_UVLM			BIT(5)
#define AW8697_SYSINTM_FF_AEM			BIT(4)
#define AW8697_SYSINTM_FF_AFM			BIT(3)
#define AW8697_SYSINTM_DONEM			BIT(0)

#define AW8697_SYSCTRL_REG			0x04
#define AW8697_SYSCTRL_RAMINIT			BIT(5)
#define AW8697_SYSCTRL_PLAY_MODE_MASK		GENMASK(3, 2)
#define AW8697_SYSCTRL_PLAY_MODE_RAM		0
#define AW8697_SYSCTRL_PLAY_MODE_CONT		2
#define AW8697_SYSCTRL_BST_MODE_BOOST		BIT(1)
#define AW8697_SYSCTRL_WORK_MODE_STANDBY	BIT(0)

#define AW8697_GO_REG				0x05
#define AW8697_GO				BIT(0)

#define AW8697_WAVSEQ1_REG			0x07
#define AW8697_WAVSEQ2_REG			0x08
#define AW8697_WAVSEQ_WAV_MASK			GENMASK(6, 0)

#define AW8697_WAVLOOP1_REG			0x0f
#define AW8697_WAVLOOP1_SEQ1_MASK		GENMASK(7, 4)
#define AW8697_WAVLOOP_INFINITE			0x0f

#define AW8697_DBGCTRL_REG			0x20
#define AW8697_DBGCTRL_INT_MODE_EDGE		BIT(2)

#define AW8697_BASEADDRH_REG			0x21
#define AW8697_BASEADDRL_REG			0x22

#define AW8697_DATCTRL_REG			0x2b
#define AW8697_DATCTRL_FC_1000HZ		GENMASK(7, 6)
#define AW8697_DATCTRL_LPF_EN			BIT(5)

#define AW8697_PWMPRC_REG			0x2d
#define AW8697_PWMPRC_PRC_EN			BIT(7)

#define AW8697_PWMDBG_REG			0x2e
#define AW8697_PWMDBG_PWM_MODE_MASK		GENMASK(6, 5)
#define AW8697_PWMDBG_PWM_24K			2

#define AW8697_BSTDBG1_REG			0x31
#define AW8697_BSTDBG2_REG			0x32
#define AW8697_BSTDBG3_REG			0x33

#define AW8697_ANADBG_REG			0x35
#define AW8697_ANADBG_IOC_MASK			GENMASK(3, 2)
#define AW8697_ANADBG_IOC_4P65A			3

#define AW8697_ANACTRL_REG			0x36
#define AW8697_ANACTRL_LRA_SRC_REG		BIT(5)

#define AW8697_DATDBG_REG			0x39

#define AW8697_PRLVL_REG			0x3e
#define AW8697_PRLVL_PR_EN			BIT(7)

#define AW8697_RAMADDRH_REG			0x40
#define AW8697_RAMADDRL_REG			0x41
#define AW8697_RAMDATA_REG			0x42

#define AW8697_GLBSTATE_REG			0x46
#define AW8697_GLBSTATE_STATE_MASK		GENMASK(3, 0)
#define AW8697_GLBSTATE_STANDBY			0

#define AW8697_BSTAUTO_REG			0x47
#define AW8697_BSTAUTO_AUTOSW			BIT(2)

#define AW8697_CONTCTRL_REG			0x48
#define AW8697_CONTCTRL_CLOSE_PLAYBACK		BIT(3)
#define AW8697_CONTCTRL_F0_DETECT		BIT(2)

#define AW8697_FPREH_REG			0x49
#define AW8697_FPREL_REG			0x4a

#define AW8697_TRIMLRA_REG			0x5b
#define AW8697_TRIMLRA_MASK			GENMASK(5, 0)

#define AW8697_RSPARE_REG			0x5d

#define AW8697_ADCTEST_REG			0x66
#define AW8697_ADCTEST_VBAT_HW			BIT(6)

#define AW8697_FLRAF0H_REG			0x68
#define AW8697_FLRAF0L_REG			0x69

#define AW8697_DRVLVL_REG			0x7b
#define AW8697_DRVLVL_F0_DETECT			0x49

#define AW8697_NUMF01_REG			0x7d
#define AW8697_NUMF01_PRE_MASK			GENMASK(7, 4)
#define AW8697_NUMF01_WAIT_MASK			GENMASK(3, 0)
#define AW8697_NUMF02_REG			0x7e
#define AW8697_NUMF03_REG			0x7f

#define AW8697_F0_PRE_NUM			9
#define AW8697_F0_WAIT_NUM			3
#define AW8697_F0_REPEAT_NUM			1
#define AW8697_F0_TRACE_NUM			31
#define AW8697_F0_CLK_DHZ			3846154
#define AW8697_F0_REF_DHZ			1700
#define AW8697_F0_TOLERANCE_PCT			7
#define AW8697_TRIM_STEPS_PER_UNIT		400

#define AW8697_RAM_BASE_ADDR			0x800
#define AW8697_GAIN_MAX				0x80

struct aw8697_data {
	struct work_struct play_work;
	struct device *dev;
	struct input_dev *input_dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	u16 level;
};

static const struct regmap_config aw8697_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
	.max_register = 0x7f,
};

static const u8 aw8697_waveform[] = {
	0x00, 0x02, 0x03, 0x05, 0x07, 0x08, 0x0a, 0x0c, 0x0d, 0x0f, 0x10, 0x12,
	0x13, 0x15, 0x16, 0x17, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
	0x21, 0x22, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x26, 0x26,
	0x26, 0x26, 0x26, 0x26, 0x25, 0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x21,
	0x20, 0x1f, 0x1e, 0x1d, 0x1c, 0x1b, 0x1a, 0x19, 0x17, 0x16, 0x15, 0x13,
	0x12, 0x10, 0x0f, 0x0d, 0x0c, 0x0a, 0x08, 0x07, 0x05, 0x03, 0x02, 0x00,
	0xfe, 0xfd, 0xfb, 0xf9, 0xf8, 0xf6, 0xf4, 0xf3, 0xf1, 0xf0, 0xee, 0xed,
	0xeb, 0xea, 0xe9, 0xe7, 0xe6, 0xe5, 0xe4, 0xe3, 0xe2, 0xe1, 0xe0, 0xdf,
	0xde, 0xdd, 0xdd, 0xdc, 0xdc, 0xdb, 0xdb, 0xda, 0xda, 0xda, 0xda, 0xda,
	0xda, 0xda, 0xda, 0xdb, 0xdb, 0xdc, 0xdc, 0xdd, 0xdd, 0xde, 0xdf, 0xe0,
	0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xed, 0xee,
	0xf0, 0xf1, 0xf3, 0xf4, 0xf6, 0xf8, 0xf9, 0xfb, 0xfd, 0xfe,
};

struct aw8697_sram_header {
	u8 version;
	__be16 start_address;
	__be16 end_address;
} __packed;

static const struct aw8697_sram_header aw8697_sram_header = {
	.version = 0x01,
	.start_address = cpu_to_be16(AW8697_RAM_BASE_ADDR +
				     sizeof(struct aw8697_sram_header)),
	.end_address = cpu_to_be16(AW8697_RAM_BASE_ADDR +
				   sizeof(struct aw8697_sram_header) +
				   ARRAY_SIZE(aw8697_waveform) - 1),
};

static int aw8697_stop(struct aw8697_data *haptics)
{
	unsigned int val;
	int err;

	err = regmap_clear_bits(haptics->regmap, AW8697_GO_REG, AW8697_GO);
	if (err)
		return err;

	err = regmap_read_poll_timeout(haptics->regmap, AW8697_GLBSTATE_REG, val,
				       FIELD_GET(AW8697_GLBSTATE_STATE_MASK, val) ==
						AW8697_GLBSTATE_STANDBY,
				       2500, 2500 * 100);
	if (err)
		dev_warn(haptics->dev, "Failed to enter standby, forcing it\n");

	return regmap_set_bits(haptics->regmap, AW8697_SYSCTRL_REG,
			       AW8697_SYSCTRL_WORK_MODE_STANDBY);
}

static int aw8697_play(struct aw8697_data *haptics)
{
	int err;

	err = aw8697_stop(haptics);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_WAVSEQ1_REG,
				 AW8697_WAVSEQ_WAV_MASK,
				 FIELD_PREP(AW8697_WAVSEQ_WAV_MASK, 1));
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_WAVSEQ2_REG,
				 AW8697_WAVSEQ_WAV_MASK, 0);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_WAVLOOP1_REG,
				 AW8697_WAVLOOP1_SEQ1_MASK,
				 FIELD_PREP(AW8697_WAVLOOP1_SEQ1_MASK,
					    AW8697_WAVLOOP_INFINITE));
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_DATDBG_REG,
			   haptics->level * AW8697_GAIN_MAX / U16_MAX);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_SYSCTRL_REG,
				 AW8697_SYSCTRL_PLAY_MODE_MASK |
					AW8697_SYSCTRL_BST_MODE_BOOST |
					AW8697_SYSCTRL_WORK_MODE_STANDBY,
				 FIELD_PREP(AW8697_SYSCTRL_PLAY_MODE_MASK,
					    AW8697_SYSCTRL_PLAY_MODE_RAM) |
					AW8697_SYSCTRL_BST_MODE_BOOST);
	if (err)
		return err;

	return regmap_set_bits(haptics->regmap, AW8697_GO_REG, AW8697_GO);
}

static void aw8697_play_work(struct work_struct *work)
{
	struct aw8697_data *haptics = container_of(work, struct aw8697_data, play_work);
	int err;

	if (haptics->level)
		err = aw8697_play(haptics);
	else
		err = aw8697_stop(haptics);

	if (err)
		dev_err(haptics->dev, "Failed to set playback state: %d\n", err);
}

static int aw8697_play_effect(struct input_dev *dev, void *data,
			      struct ff_effect *effect)
{
	struct aw8697_data *haptics = input_get_drvdata(dev);
	u16 level;

	level = effect->u.rumble.strong_magnitude;
	if (!level)
		level = effect->u.rumble.weak_magnitude;

	if (haptics->level == level)
		return 0;

	haptics->level = level;
	schedule_work(&haptics->play_work);

	return 0;
}

static void aw8697_halt(struct aw8697_data *haptics)
{
	int err;

	cancel_work_sync(&haptics->play_work);
	haptics->level = 0;

	err = aw8697_stop(haptics);
	if (err)
		dev_err(haptics->dev, "Failed to stop playback: %d\n", err);
}

static void aw8697_close(struct input_dev *input)
{
	struct aw8697_data *haptics = input_get_drvdata(input);

	aw8697_halt(haptics);
}

static int aw8697_hw_init(struct aw8697_data *haptics)
{
	int err;

	err = regmap_update_bits(haptics->regmap, AW8697_PWMDBG_REG,
				 AW8697_PWMDBG_PWM_MODE_MASK,
				 FIELD_PREP(AW8697_PWMDBG_PWM_MODE_MASK,
					    AW8697_PWMDBG_PWM_24K));
	if (err)
		return err;

	err = regmap_clear_bits(haptics->regmap, AW8697_PWMPRC_REG,
				AW8697_PWMPRC_PRC_EN);
	if (err)
		return err;

	err = regmap_clear_bits(haptics->regmap, AW8697_PRLVL_REG,
				AW8697_PRLVL_PR_EN);
	if (err)
		return err;

	err = regmap_set_bits(haptics->regmap, AW8697_ADCTEST_REG,
			      AW8697_ADCTEST_VBAT_HW);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_BSTDBG1_REG, 0x30);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_BSTDBG2_REG, 0xeb);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_BSTDBG3_REG, 0xd4);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_ANADBG_REG,
				 AW8697_ANADBG_IOC_MASK,
				 FIELD_PREP(AW8697_ANADBG_IOC_MASK,
					    AW8697_ANADBG_IOC_4P65A));
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_RSPARE_REG, 0x68);
	if (err)
		return err;

	return regmap_clear_bits(haptics->regmap, AW8697_BSTAUTO_REG,
				 AW8697_BSTAUTO_AUTOSW);
}

static int aw8697_ram_init(struct aw8697_data *haptics)
{
	int err;

	err = regmap_set_bits(haptics->regmap, AW8697_SYSCTRL_REG,
			      AW8697_SYSCTRL_RAMINIT);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_BASEADDRH_REG,
			   AW8697_RAM_BASE_ADDR >> 8);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_BASEADDRL_REG,
			   AW8697_RAM_BASE_ADDR & 0xff);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_RAMADDRH_REG,
			   AW8697_RAM_BASE_ADDR >> 8);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_RAMADDRL_REG,
			   AW8697_RAM_BASE_ADDR & 0xff);
	if (err)
		return err;

	err = regmap_noinc_write(haptics->regmap, AW8697_RAMDATA_REG,
				 &aw8697_sram_header, sizeof(aw8697_sram_header));
	if (err)
		return err;

	err = regmap_noinc_write(haptics->regmap, AW8697_RAMDATA_REG,
				 aw8697_waveform, ARRAY_SIZE(aw8697_waveform));
	if (err)
		return err;

	return regmap_clear_bits(haptics->regmap, AW8697_SYSCTRL_REG,
				 AW8697_SYSCTRL_RAMINIT);
}

static int aw8697_f0_measure(struct aw8697_data *haptics, unsigned int *period)
{
	unsigned int fpre = AW8697_F0_CLK_DHZ / AW8697_F0_REF_DHZ;
	unsigned int val;
	u8 buf[2];
	int err;

	err = regmap_update_bits(haptics->regmap, AW8697_SYSCTRL_REG,
				 AW8697_SYSCTRL_PLAY_MODE_MASK |
					AW8697_SYSCTRL_BST_MODE_BOOST |
					AW8697_SYSCTRL_WORK_MODE_STANDBY,
				 FIELD_PREP(AW8697_SYSCTRL_PLAY_MODE_MASK,
					    AW8697_SYSCTRL_PLAY_MODE_CONT) |
					AW8697_SYSCTRL_BST_MODE_BOOST);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_CONTCTRL_REG,
				 AW8697_CONTCTRL_CLOSE_PLAYBACK |
					AW8697_CONTCTRL_F0_DETECT,
				 AW8697_CONTCTRL_F0_DETECT);
	if (err)
		return err;

	err = regmap_set_bits(haptics->regmap, AW8697_DATCTRL_REG,
			      AW8697_DATCTRL_FC_1000HZ | AW8697_DATCTRL_LPF_EN);
	if (err)
		return err;

	err = regmap_set_bits(haptics->regmap, AW8697_ANACTRL_REG,
			      AW8697_ANACTRL_LRA_SRC_REG);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_FPREH_REG, fpre >> 8);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_FPREL_REG, fpre & 0xff);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_DRVLVL_REG,
			   AW8697_DRVLVL_F0_DETECT);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_NUMF01_REG,
			   FIELD_PREP(AW8697_NUMF01_PRE_MASK, AW8697_F0_PRE_NUM) |
			   FIELD_PREP(AW8697_NUMF01_WAIT_MASK, AW8697_F0_WAIT_NUM));
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_NUMF02_REG,
			   AW8697_F0_REPEAT_NUM);
	if (err)
		return err;

	err = regmap_write(haptics->regmap, AW8697_NUMF03_REG,
			   AW8697_F0_TRACE_NUM);
	if (err)
		return err;

	err = regmap_read(haptics->regmap, AW8697_SYSINT_REG, &val);
	if (err)
		return err;

	err = regmap_set_bits(haptics->regmap, AW8697_GO_REG, AW8697_GO);
	if (err)
		return err;

	err = regmap_read_poll_timeout(haptics->regmap, AW8697_SYSINT_REG, val,
				       val & AW8697_SYSINT_DONEI,
				       10000, 500000);
	if (err)
		return err;

	err = regmap_bulk_read(haptics->regmap, AW8697_FLRAF0H_REG, buf,
			       sizeof(buf));
	if (err)
		return err;

	*period = (buf[0] << 8) | buf[1];

	return 0;
}

static int aw8697_f0_calibrate(struct aw8697_data *haptics)
{
	unsigned int period = 0;
	int f0, step, err, ret;

	err = regmap_write(haptics->regmap, AW8697_TRIMLRA_REG, 0);
	if (err)
		return err;

	ret = aw8697_f0_measure(haptics, &period);

	err = aw8697_stop(haptics);
	if (err)
		return err;

	err = regmap_update_bits(haptics->regmap, AW8697_CONTCTRL_REG,
				 AW8697_CONTCTRL_CLOSE_PLAYBACK |
					AW8697_CONTCTRL_F0_DETECT,
				 AW8697_CONTCTRL_CLOSE_PLAYBACK);
	if (err)
		return err;

	if (ret)
		return ret;

	if (!period)
		return -EINVAL;

	f0 = AW8697_F0_CLK_DHZ / period;
	if (abs(f0 - AW8697_F0_REF_DHZ) * 100 >
	    AW8697_F0_REF_DHZ * AW8697_F0_TOLERANCE_PCT)
		return -ERANGE;

	step = DIV_ROUND_CLOSEST((f0 - AW8697_F0_REF_DHZ) *
				 AW8697_TRIM_STEPS_PER_UNIT, f0);

	dev_dbg(haptics->dev, "LRA f0 %d.%d Hz, trim %d\n",
		f0 / 10, f0 % 10, step);

	return regmap_write(haptics->regmap, AW8697_TRIMLRA_REG,
			    step & AW8697_TRIMLRA_MASK);
}

static irqreturn_t aw8697_irq(int irq, void *data)
{
	struct aw8697_data *haptics = data;
	struct device *dev = haptics->dev;
	unsigned int val;
	int err;

	err = regmap_read(haptics->regmap, AW8697_SYSINT_REG, &val);
	if (err) {
		dev_err(dev, "Failed to read SYSINT register: %d\n", err);
		return IRQ_NONE;
	}

	if (val & AW8697_SYSINT_BSTERRI)
		dev_err(dev, "Boost error interrupt\n");
	if (val & AW8697_SYSINT_OVI)
		dev_err(dev, "Over voltage interrupt\n");
	if (val & AW8697_SYSINT_UVLI)
		dev_err(dev, "Under voltage lockout interrupt\n");
	if (val & AW8697_SYSINT_OCDI)
		dev_err(dev, "Over current interrupt\n");
	if (val & AW8697_SYSINT_OTI)
		dev_err(dev, "Over temperature interrupt\n");
	if (val & AW8697_SYSINT_DONEI)
		dev_dbg(dev, "Playback done\n");
	if (val & AW8697_SYSINT_FF_AFI)
		dev_dbg(dev, "FIFO almost full\n");
	if (val & AW8697_SYSINT_FF_AEI)
		dev_dbg(dev, "FIFO almost empty\n");

	return IRQ_HANDLED;
}

static void aw8697_hw_reset(struct aw8697_data *haptics)
{
	gpiod_set_value_cansleep(haptics->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(haptics->reset_gpio, 0);
	usleep_range(8000, 8500);
}

static int aw8697_detect(struct aw8697_data *haptics)
{
	unsigned int chip_id;
	int err;

	err = regmap_read(haptics->regmap, AW8697_ID_REG, &chip_id);
	if (err)
		return dev_err_probe(haptics->dev, err, "Failed to read chip ID\n");

	if (chip_id != AW8697_CHIPID)
		return dev_err_probe(haptics->dev, -ENODEV,
				     "Unexpected chip ID 0x%02x\n", chip_id);

	return 0;
}

static int aw8697_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw8697_data *haptics;
	unsigned int val;
	int err;

	haptics = devm_kzalloc(dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;

	haptics->dev = dev;
	i2c_set_clientdata(client, haptics);

	haptics->regmap = devm_regmap_init_i2c(client, &aw8697_regmap_config);
	if (IS_ERR(haptics->regmap))
		return dev_err_probe(dev, PTR_ERR(haptics->regmap),
				     "Failed to allocate register map\n");

	haptics->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(haptics->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(haptics->reset_gpio),
				     "Failed to get reset gpio\n");

	aw8697_hw_reset(haptics);

	err = regmap_write(haptics->regmap, AW8697_ID_REG, AW8697_ID_SOFTRST);
	if (err)
		return dev_err_probe(dev, err, "Failed to reset chip\n");

	usleep_range(3000, 3500);

	err = aw8697_detect(haptics);
	if (err)
		return err;

	err = regmap_set_bits(haptics->regmap, AW8697_DBGCTRL_REG,
			      AW8697_DBGCTRL_INT_MODE_EDGE);
	if (err)
		return dev_err_probe(dev, err, "Failed to set interrupt mode\n");

	err = regmap_write(haptics->regmap, AW8697_SYSINTM_REG,
			   AW8697_SYSINTM_BSTERRM | AW8697_SYSINTM_UVLM |
			   AW8697_SYSINTM_FF_AEM | AW8697_SYSINTM_FF_AFM |
			   AW8697_SYSINTM_DONEM);
	if (err)
		return dev_err_probe(dev, err, "Failed to set interrupt mask\n");

	err = aw8697_stop(haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to enter standby\n");

	err = aw8697_hw_init(haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to configure chip\n");

	err = aw8697_ram_init(haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to load waveform\n");

	err = aw8697_f0_calibrate(haptics);
	if (err)
		dev_warn(dev, "LRA f0 calibration failed: %d\n", err);

	err = regmap_read(haptics->regmap, AW8697_SYSINT_REG, &val);
	if (err)
		return dev_err_probe(dev, err, "Failed to clear interrupts\n");

	err = devm_request_threaded_irq(dev, client->irq, NULL, aw8697_irq,
					IRQF_ONESHOT, NULL, haptics);
	if (err)
		return dev_err_probe(dev, err, "Failed to request irq\n");

	INIT_WORK(&haptics->play_work, aw8697_play_work);

	haptics->input_dev = devm_input_allocate_device(dev);
	if (!haptics->input_dev)
		return -ENOMEM;

	haptics->input_dev->name = "aw8697-haptics";
	haptics->input_dev->close = aw8697_close;
	input_set_drvdata(haptics->input_dev, haptics);
	input_set_capability(haptics->input_dev, EV_FF, FF_RUMBLE);

	err = input_ff_create_memless(haptics->input_dev, NULL, aw8697_play_effect);
	if (err)
		return dev_err_probe(dev, err, "Failed to create FF device\n");

	err = input_register_device(haptics->input_dev);
	if (err)
		return dev_err_probe(dev, err, "Failed to register input device\n");

	return 0;
}

static int aw8697_suspend(struct device *dev)
{
	struct aw8697_data *haptics = dev_get_drvdata(dev);

	guard(mutex)(&haptics->input_dev->mutex);

	if (input_device_enabled(haptics->input_dev))
		aw8697_halt(haptics);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aw8697_pm_ops, aw8697_suspend, NULL);

static const struct i2c_device_id aw8697_id[] = {
	{ "aw8697" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw8697_id);

static const struct of_device_id aw8697_of_match[] = {
	{ .compatible = "awinic,aw8697" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8697_of_match);

static struct i2c_driver aw8697_driver = {
	.driver = {
		.name = "aw8697-haptics",
		.of_match_table = aw8697_of_match,
		.pm = pm_sleep_ptr(&aw8697_pm_ops),
	},
	.probe = aw8697_probe,
	.id_table = aw8697_id,
};
module_i2c_driver(aw8697_driver);

MODULE_AUTHOR("Armandas Kvietkus <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("Awinic AW8697 LRA haptics driver");
MODULE_LICENSE("GPL");
