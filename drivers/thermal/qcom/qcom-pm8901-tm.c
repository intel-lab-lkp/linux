// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm PM8901 PMIC Thermal-Alarm Driver
 *
 * Mainline port of the legacy 2.6.35-palm drivers/thermal/pmic8901-tm.c.
 * PM8901 exposes a stage-based over-temperature alarm (no raw ADC) with
 * four selectable thresholds and three escalating stages. This driver
 * mirrors the legacy programming exactly (threshold-set 0, software
 * override enabled, PWM gating at 8 Hz) and registers a thermal-of
 * sensor so a board DT can declare trip points and a critical action.
 *
 * Copyright (c) 2010-2011, Code Aurora Forum.
 * Copyright (c) 2026, HP TouchPad mainline port.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/thermal.h>

/* SSBI registers (offsets from the per-instance reg base) */
#define PM8901_TM_REG_CTRL		0x00	/* CTRL/STATUS  (base + 0) */
#define PM8901_TM_REG_PWM		0x01	/* PWM gating  (base + 1) */

/* CTRL register fields */
#define CTRL_ST3_SD			BIT(7)
#define CTRL_ST2_SD			BIT(6)
#define CTRL_STATUS_MASK		GENMASK(5, 4)
#define CTRL_THRESH_MASK		GENMASK(3, 2)
#define CTRL_OVRD_ST3			BIT(1)
#define CTRL_OVRD_ST2			BIT(0)
#define CTRL_OVRD_MASK			GENMASK(1, 0)

/* PWM register fields */
#define PWM_EN				BIT(7)
#define PWM_PER_PRE_MASK		GENMASK(5, 3)
#define PWM_PER_DIV_MASK		GENMASK(2, 0)

/* Temperature math (from legacy pmic8901-tm.c) */
#define PM8901_TEMP_STAGE_STEP		20000	/* 20 deg C between stages */
#define PM8901_TEMP_STAGE_HYSTERESIS	2000	/*  2 deg C transition guard */
#define PM8901_TEMP_THRESH_MIN		105000	/* threshold 0 base = 105 C */
#define PM8901_TEMP_THRESH_STEP		5000	/*  5 deg C per threshold step */

/*
 * PM8901 has no real die ADC; when stage == 0 ("below threshold") we
 * report a plausible idle estimate matching the legacy DEFAULT_NO_ADC_TEMP.
 */
#define PM8901_TEMP_NO_ALARM		37000

struct pm8901_tm_chip {
	struct device			*dev;
	struct regmap			*map;
	struct thermal_zone_device	*tz_dev;
	struct mutex			lock;
	unsigned int			base;	/* SSBI offset, from DT reg */
	unsigned int			stage;
	unsigned int			thresh;
	int				temp;
	bool				initialised;
};

static int pm8901_tm_read_ctrl(struct pm8901_tm_chip *chip, u8 *val)
{
	unsigned int v;
	int ret;

	ret = regmap_read(chip->map, chip->base + PM8901_TM_REG_CTRL, &v);
	if (!ret)
		*val = v;
	return ret;
}

static int pm8901_tm_write_ctrl(struct pm8901_tm_chip *chip, u8 val)
{
	return regmap_write(chip->map, chip->base + PM8901_TM_REG_CTRL, val);
}

static int pm8901_tm_write_pwm(struct pm8901_tm_chip *chip, u8 val)
{
	return regmap_write(chip->map, chip->base + PM8901_TM_REG_PWM, val);
}

/*
 * Decode the (stage, threshold) pair into a single millicelsius value.
 * Logic matches the legacy pmic8901-tm.c hysteresis selection:
 *  - on a rising stage transition, use the lower bound of the new stage
 *    plus +HYSTERESIS so we don't bounce
 *  - on a falling stage transition, use the upper bound of the new stage
 *    minus -HYSTERESIS
 *  - on the first read after probe (initialised == false), pick a
 *    representative point: midpoint of the stage range, or
 *    PM8901_TEMP_NO_ALARM when stage == 0.
 */
static int pm8901_tm_update_temp_locked(struct pm8901_tm_chip *chip)
{
	unsigned int new_stage;
	u8 reg;
	int ret;

	ret = pm8901_tm_read_ctrl(chip, &reg);
	if (ret)
		return ret;

	new_stage = FIELD_GET(CTRL_STATUS_MASK, reg);
	chip->thresh = FIELD_GET(CTRL_THRESH_MASK, reg);

	if (!chip->initialised) {
		if (new_stage)
			chip->temp = PM8901_TEMP_THRESH_MIN +
				     chip->thresh * PM8901_TEMP_THRESH_STEP +
				     (new_stage - 1) * PM8901_TEMP_STAGE_STEP;
		else
			chip->temp = PM8901_TEMP_NO_ALARM;
		chip->initialised = true;
	} else if (new_stage > chip->stage) {
		chip->temp = PM8901_TEMP_THRESH_MIN +
			     chip->thresh * PM8901_TEMP_THRESH_STEP +
			     (new_stage - 1) * PM8901_TEMP_STAGE_STEP +
			     PM8901_TEMP_STAGE_HYSTERESIS;
	} else if (new_stage < chip->stage) {
		chip->temp = PM8901_TEMP_THRESH_MIN +
			     chip->thresh * PM8901_TEMP_THRESH_STEP +
			     new_stage * PM8901_TEMP_STAGE_STEP -
			     PM8901_TEMP_STAGE_HYSTERESIS;
	}

	chip->stage = new_stage;
	return 0;
}

static int pm8901_tm_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct pm8901_tm_chip *chip = thermal_zone_device_priv(tz);
	int ret;

	if (!temp)
		return -EINVAL;

	mutex_lock(&chip->lock);
	ret = pm8901_tm_update_temp_locked(chip);
	if (!ret)
		*temp = chip->temp;
	mutex_unlock(&chip->lock);

	return ret;
}

static const struct thermal_zone_device_ops pm8901_tm_zone_ops = {
	.get_temp = pm8901_tm_get_temp,
};

/*
 * Shared ISR for both TEMP_ALARM (stage-transition) and TEMP_HI_ALARM
 * (hi-temp) interrupts. Updates the cached temperature, clears any
 * latched ST2_SD / ST3_SD shutdown bits so the next stage transition
 * can be observed, and pokes the thermal core which then re-reads
 * temp and walks trips (a critical-trip cross triggers orderly_poweroff
 * via the kernel's standard machinery).
 */
static irqreturn_t pm8901_tm_isr(int irq, void *data)
{
	struct pm8901_tm_chip *chip = data;
	u8 reg;
	int ret;

	mutex_lock(&chip->lock);

	ret = pm8901_tm_update_temp_locked(chip);
	if (ret) {
		mutex_unlock(&chip->lock);
		return IRQ_HANDLED;
	}

	ret = pm8901_tm_read_ctrl(chip, &reg);
	if (!ret && (reg & (CTRL_ST2_SD | CTRL_ST3_SD))) {
		reg &= ~(CTRL_ST2_SD | CTRL_ST3_SD | CTRL_STATUS_MASK);
		pm8901_tm_write_ctrl(chip, reg);
	}

	dev_dbg(chip->dev, "alarm irq=%d stage=%u thresh=%u temp=%d\n",
		irq, chip->stage, chip->thresh, chip->temp);

	mutex_unlock(&chip->lock);

	thermal_zone_device_update(chip->tz_dev, THERMAL_EVENT_UNSPECIFIED);
	return IRQ_HANDLED;
}

/*
 * Program PM8901 to the legacy default: threshold-set 0 (105 / 125 / 145 C),
 * software override enabled (kernel handles shutdown, PMIC does not auto-cut),
 * PWM at 8 Hz (legacy "cut down on unnecessary interrupts" rate).
 */
static int pm8901_tm_init_hw(struct pm8901_tm_chip *chip)
{
	int ret;
	u8 reg;

	mutex_lock(&chip->lock);

	ret = pm8901_tm_read_ctrl(chip, &reg);
	if (ret)
		goto out;

	/*
	 * Enable software override so PMIC does NOT auto-shut-down on stage 3.
	 * Critical-trip orderly_poweroff is delivered by the kernel thermal
	 * core via the DT thermal-zone trip with type = "critical".
	 */
	reg = (reg & ~(CTRL_OVRD_MASK | CTRL_STATUS_MASK | CTRL_THRESH_MASK)) |
	      CTRL_OVRD_ST3 | CTRL_OVRD_ST2;
	ret = pm8901_tm_write_ctrl(chip, reg);
	if (ret)
		goto out;

	chip->thresh = 0;

	/* PWM @ 8 Hz: PWM_EN | PRE=3 | DIV=3 — verbatim from legacy. */
	reg = PWM_EN | FIELD_PREP(PWM_PER_PRE_MASK, 3) |
	      FIELD_PREP(PWM_PER_DIV_MASK, 3);
	ret = pm8901_tm_write_pwm(chip, reg);
	if (ret)
		goto out;

	/* Prime the cached temperature from current hardware state. */
	chip->initialised = false;
	ret = pm8901_tm_update_temp_locked(chip);

out:
	mutex_unlock(&chip->lock);
	return ret;
}

static int pm8901_tm_probe(struct platform_device *pdev)
{
	struct pm8901_tm_chip *chip;
	int ret, irq_alarm, irq_hi_alarm;
	u32 res;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	mutex_init(&chip->lock);

	chip->map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->map)
		return dev_err_probe(&pdev->dev, -ENXIO,
				     "no regmap on PM8901 parent\n");

	ret = of_property_read_u32(pdev->dev.of_node, "reg", &res);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "missing reg property\n");
	chip->base = res;

	irq_alarm = platform_get_irq_byname(pdev, "alarm");
	if (irq_alarm < 0)
		return irq_alarm;
	irq_hi_alarm = platform_get_irq_byname(pdev, "hi-alarm");
	if (irq_hi_alarm < 0)
		return irq_hi_alarm;

	ret = pm8901_tm_init_hw(chip);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "hw init failed\n");

	chip->tz_dev = devm_thermal_of_zone_register(&pdev->dev, 0, chip,
						     &pm8901_tm_zone_ops);
	if (IS_ERR(chip->tz_dev))
		return dev_err_probe(&pdev->dev, PTR_ERR(chip->tz_dev),
				     "thermal zone register failed\n");

	ret = devm_request_threaded_irq(&pdev->dev, irq_alarm, NULL,
					pm8901_tm_isr, IRQF_ONESHOT,
					"pm8901-tm-alarm", chip);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "alarm IRQ request failed\n");

	ret = devm_request_threaded_irq(&pdev->dev, irq_hi_alarm, NULL,
					pm8901_tm_isr, IRQF_ONESHOT,
					"pm8901-tm-hi-alarm", chip);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "hi-alarm IRQ request failed\n");

	platform_set_drvdata(pdev, chip);
	thermal_zone_device_update(chip->tz_dev, THERMAL_EVENT_UNSPECIFIED);

	dev_info(&pdev->dev,
		 "PM8901 thermal alarm: base=0x%x stage=%u thresh=%u temp=%d\n",
		 chip->base, chip->stage, chip->thresh, chip->temp);

	return 0;
}

static void pm8901_tm_remove(struct platform_device *pdev)
{
	struct pm8901_tm_chip *chip = platform_get_drvdata(pdev);
	u8 reg;

	/*
	 * Disable software override on the way out so the PMIC reverts to
	 * its hardware auto-cut behaviour if the kernel is no longer the
	 * shutdown agent. Best-effort: ignore errors.
	 */
	mutex_lock(&chip->lock);
	if (!pm8901_tm_read_ctrl(chip, &reg)) {
		reg &= ~CTRL_OVRD_MASK;
		pm8901_tm_write_ctrl(chip, reg);
	}
	mutex_unlock(&chip->lock);
}

static const struct of_device_id pm8901_tm_match_table[] = {
	{ .compatible = "qcom,pm8901-temp-alarm" },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8901_tm_match_table);

static struct platform_driver pm8901_tm_driver = {
	.driver = {
		.name		= "pm8901-temp-alarm",
		.of_match_table	= pm8901_tm_match_table,
	},
	.probe	= pm8901_tm_probe,
	.remove	= pm8901_tm_remove,
};
module_platform_driver(pm8901_tm_driver);

MODULE_ALIAS("platform:pm8901-temp-alarm");
MODULE_DESCRIPTION("Qualcomm PM8901 PMIC Thermal Alarm driver");
MODULE_LICENSE("GPL v2");
