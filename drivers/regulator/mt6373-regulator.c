// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2024 MediaTek Inc.
// Copyright (c) 2025 Collabora Ltd
//                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/mt6373-regulator.h>
#include <linux/regulator/of_regulator.h>

#define MT6373_REGULATOR_MODE_NORMAL	0
#define MT6373_REGULATOR_MODE_FCCM	1
#define MT6373_REGULATOR_MODE_LP	2
#define MT6373_REGULATOR_MODE_ULP	3

#define EN_SET_OFFSET			0x1
#define EN_CLR_OFFSET			0x2

#define MT6373_RG_RSV_SWREG_H		0xa09
#define MT6373_PLG_CFG_ELR1		0x3ab
#define MT6373_ELR_MASK			0xc

#define OC_IRQ_ENABLE_DELAY_MS		10

/* Unlock key for mode setting */
#define MT6373_BUCK_TOP_UNLOCK_VALUE	0x5543

enum {
	MT6373_ID_VBUCK0,
	MT6373_ID_VBUCK1,
	MT6373_ID_VBUCK2,
	MT6373_ID_VBUCK3,
	MT6373_ID_VBUCK4,
	MT6373_ID_VBUCK4_UFS,
	MT6373_ID_VBUCK5,
	MT6373_ID_VBUCK6,
	MT6373_ID_VBUCK7,
	MT6373_ID_VBUCK8,
	MT6373_ID_VBUCK9,
	MT6373_ID_VUSB,
	MT6373_ID_VAUX18,
	MT6373_ID_VRF13_AIF,
	MT6373_ID_VRF18_AIF,
	MT6373_ID_VRFIO18_AIF,
	MT6373_ID_VRF09_AIF,
	MT6373_ID_VRF12_AIF,
	MT6373_ID_VANT18,
	MT6373_ID_VSRAM_DIGRF_AIF,
	MT6373_ID_VIBR,
	MT6373_ID_VIO28,
	MT6373_ID_VFP,
	MT6373_ID_VTP,
	MT6373_ID_VMCH,
	MT6373_ID_VMC,
	MT6373_ID_VAUD18,
	MT6373_ID_VCN33_1,
	MT6373_ID_VCN33_2,
	MT6373_ID_VCN33_3,
	MT6373_ID_VCN18IO,
	MT6373_ID_VEFUSE,
	MT6373_ID_VMCH_EINT_HIGH,
	MT6373_ID_VMCH_EINT_LOW
};

/**
 * struct mt6373_regulator_info - MT6373 regulators information
 * @desc: Regulator description structure
 * @lp_mode_reg: Low Power mode register (normal/idle)
 * @lp_mode_mask: Low Power mode regulator mask
 * @modeset_reg: AUTO/PWM mode register
 * @modeset_mask: AUTO/PWM regulator mask
 * @vocal_reg: Voltage Output Calibration register
 * @vocal_mask: Voltage Output Calibration regulator mask
 * @oc_work: Delayed work for enabling overcurrent IRQ
 * @irq: Interrupt for overcurrent event
 */
struct mt6373_regulator_info {
	struct regulator_desc desc;
	u16 lp_mode_reg;
	u16 lp_mode_mask;
	u16 modeset_reg;
	u16 modeset_mask;
	u16 vocal_reg;
	u16 vocal_mask;
	struct delayed_work oc_work;
	int irq;
};

#define MT6373_BUCK(match, vreg, min, max, step, en_reg, en_bit, vs_reg,\
		    vs_mask, lp_reg, lp_bit, mset_reg, mset_bit)	\
[MT6373_ID_##vreg] = {							\
	.desc = {							\
		.name = match,						\
		.of_match = of_match_ptr(match),			\
		.ops = &mt6373_vreg_setclr_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6373_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = (max - min) / step + 1,			\
		.min_uV = min,						\
		.uV_step = step,					\
		.enable_reg = en_reg,					\
		.enable_mask = BIT(en_bit),				\
		.vsel_reg = vs_reg,					\
		.vsel_mask = vs_mask,					\
		.of_map_mode = mt6373_map_mode,				\
	},								\
	.lp_mode_reg = lp_reg,						\
	.lp_mode_mask = BIT(lp_bit),					\
	.modeset_reg = mset_reg,					\
	.modeset_mask = BIT(mset_bit),					\
}

#define MT6373_LDO_L(match, vreg, min, max, step, en_reg, vs_reg,	\
		     vs_mask)						\
[MT6373_ID_##vreg] = {							\
	.desc = {							\
		.name = match,						\
		.of_match = of_match_ptr(match),			\
		.ops = &mt6373_ldo_linear_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6373_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = (max - min) / step + 1,			\
		.min_uV = min,						\
		.uV_step = step,					\
		.enable_reg = en_reg,					\
		.enable_mask = BIT(0),					\
		.vsel_reg = vs_reg,					\
		.vsel_mask = vs_mask,					\
		.of_map_mode = mt6373_map_mode,				\
	},								\
	.lp_mode_reg = en_reg,						\
	.lp_mode_mask = BIT(1),						\
}

#define MT6373_LDO_VT_OPS(match, vreg, vops, vtable, en_reg, vs_reg,	\
			  cal_reg)					\
[MT6373_ID_##vreg] = {							\
	.desc = {							\
		.name = match,						\
		.of_match = of_match_ptr(match),			\
		.ops = &vops,						\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6373_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.volt_table = vtable,					\
		.n_voltages = ARRAY_SIZE(vtable),			\
		.enable_reg = en_reg,					\
		.enable_mask = BIT(0),					\
		.vsel_reg = vs_reg,					\
		.vsel_mask = MT6373_PMIC_RG_LDO_VT_VOCALSEL_MASK,	\
		.of_map_mode = mt6373_map_mode,				\
	},								\
	.vocal_reg = cal_reg,						\
	.vocal_mask = MT6373_PMIC_RG_LDO_VT_VOCALSEL_MASK,		\
	.lp_mode_reg = en_reg,						\
	.lp_mode_mask = BIT(1),						\
}

#define MT6373_LDO_VT(match, vreg, vtable, en_reg, vsel_reg, cal_reg)	\
	MT6373_LDO_VT_OPS(match, vreg, mt6373_ldo_vtable_ops, vtable,	\
			  en_reg, vsel_reg, cal_reg)

static const unsigned int ldo_volt_table1[] = {
	1200000, 1300000, 1500000, 1700000, 1800000, 2000000, 2100000, 2200000,
	2700000, 2800000, 2900000, 3000000, 3100000, 3300000, 3400000, 3500000,
};

static const unsigned int ldo_volt_table2[] = {
	1800000, 1900000, 2000000, 2100000, 2200000, 2300000, 2400000, 2500000,
	2600000, 2700000, 2800000, 2900000, 3000000, 3100000, 3200000, 3300000,
};

static const unsigned int ldo_volt_table3[] = {
	600000,  700000,  800000,  900000,  1000000, 1100000, 1200000, 1300000,
	1400000, 1500000, 1600000, 1700000, 1800000, 1900000, 2000000, 2100000,
};

static const unsigned int ldo_volt_table4[] = {
	1200000, 1300000, 1500000, 1700000, 1800000, 2000000, 2500000, 2600000,
	2700000, 2800000, 2900000, 3000000, 3100000, 3300000, 3400000, 3500000,
};

static const unsigned int ldo_volt_table5[] = {
	900000, 1000000, 1100000, 1200000, 1300000, 1700000, 1800000, 1810000,
};

static int mt6373_vreg_enable_setclr(struct regulator_dev *rdev)
{
	return regmap_write(rdev->regmap, rdev->desc->enable_reg + EN_SET_OFFSET,
			    rdev->desc->enable_mask);
}

static int mt6373_vreg_disable_setclr(struct regulator_dev *rdev)
{
	return regmap_write(rdev->regmap, rdev->desc->enable_reg + EN_CLR_OFFSET,
			    rdev->desc->enable_mask);
}

static inline unsigned int mt6373_map_mode(unsigned int mode)
{
	switch (mode) {
	case MT6373_REGULATOR_MODE_NORMAL:
		return REGULATOR_MODE_NORMAL;
	case MT6373_REGULATOR_MODE_FCCM:
		return REGULATOR_MODE_FAST;
	case MT6373_REGULATOR_MODE_LP:
		return REGULATOR_MODE_IDLE;
	case MT6373_REGULATOR_MODE_ULP:
		return REGULATOR_MODE_STANDBY;
	default:
		return REGULATOR_MODE_INVALID;
	}
}

static int mt6373_vmch_eint_enable(struct regulator_dev *rdev)
{
	const struct regulator_desc *rdesc = rdev->desc;
	unsigned int val;
	int ret;

	if (rdesc->id == MT6373_ID_VMCH_EINT_HIGH)
		val = MT6373_PMIC_RG_LDO_VMCH_EINT_POL_BIT;
	else
		val = 0;

	ret = regmap_update_bits(rdev->regmap, MT6373_LDO_VMCH_EINT,
				 MT6373_PMIC_RG_LDO_VMCH_EINT_POL_BIT, val);
	if (ret)
		return ret;

	ret = regmap_set_bits(rdev->regmap, MT6373_PMIC_RG_LDO_VMCH_ADDR,
			      rdesc->enable_mask);
	if (ret)
		return ret;

	return regmap_set_bits(rdev->regmap, rdesc->enable_reg, rdesc->enable_mask);
}

static int mt6373_vmch_eint_disable(struct regulator_dev *rdev)
{
	const struct regulator_desc *rdesc = rdev->desc;
	int ret;

	ret = regmap_clear_bits(rdev->regmap, MT6373_PMIC_RG_LDO_VMCH_ADDR,
				rdesc->enable_mask);
	if (ret)
		return ret;

	/* Wait for VMCH discharging */
	usleep_range(1500, 1600);

	return regmap_clear_bits(rdev->regmap, rdesc->enable_reg, rdesc->enable_mask);
}

static unsigned int mt6373_regulator_get_mode(struct regulator_dev *rdev)
{
	struct mt6373_regulator_info *info = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;

	ret = regmap_read(rdev->regmap, info->modeset_reg, &val);
	if (ret) {
		dev_err(&rdev->dev, "Failed to get mt6373 mode: %d\n", ret);
		return ret;
	}

	if (val & info->modeset_mask)
		return REGULATOR_MODE_FAST;

	ret = regmap_read(rdev->regmap, info->lp_mode_reg, &val);
	val &= info->lp_mode_mask;
	if (ret) {
		dev_err(&rdev->dev, "Failed to get lp mode: %d\n", ret);
		return ret;
	}

	if (val)
		return REGULATOR_MODE_IDLE;
	else
		return REGULATOR_MODE_NORMAL;
}

static int mt6373_buck_unlock(struct regmap *map, bool unlock)
{
	u16 buf = unlock ? MT6373_BUCK_TOP_UNLOCK_VALUE : 0;

	return regmap_bulk_write(map, MT6373_BUCK_TOP_KEY_PROT_LO, &buf, sizeof(buf));
}

static int mt6373_regulator_set_mode(struct regulator_dev *rdev,
				     unsigned int mode)
{
	struct mt6373_regulator_info *info = rdev_get_drvdata(rdev);
	struct regmap *regmap = rdev->regmap;
	int cur_mode, ret;

	switch (mode) {
	case REGULATOR_MODE_FAST:
		ret = mt6373_buck_unlock(regmap, true);
		if (ret)
			break;

		ret = regmap_set_bits(regmap, info->modeset_reg, info->modeset_mask);

		mt6373_buck_unlock(regmap, false);
		break;
	case REGULATOR_MODE_NORMAL:
		cur_mode = mt6373_regulator_get_mode(rdev);
		if (cur_mode < 0) {
			ret = cur_mode;
			break;
		}

		if (cur_mode == REGULATOR_MODE_FAST) {
			ret = mt6373_buck_unlock(regmap, true);
			if (ret)
				break;

			ret = regmap_clear_bits(regmap, info->modeset_reg, info->modeset_mask);

			mt6373_buck_unlock(regmap, false);
			break;
		} else if (cur_mode == REGULATOR_MODE_IDLE) {
			ret = regmap_clear_bits(regmap, info->lp_mode_reg, info->lp_mode_mask);
			if (ret == 0)
				usleep_range(100, 200);
		} else {
			ret = 0;
		}
		break;
	case REGULATOR_MODE_IDLE:
		ret = regmap_set_bits(regmap, info->lp_mode_reg, info->lp_mode_mask);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret) {
		dev_err(&rdev->dev, "Failed to set mode %u: %d\n", mode, ret);
		return ret;
	}

	return 0;
}

static void mt6373_oc_irq_enable_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct mt6373_regulator_info *info =
		container_of(dwork, struct mt6373_regulator_info, oc_work);

	enable_irq(info->irq);
}

static irqreturn_t mt6373_oc_isr(int irq, void *data)
{
	struct regulator_dev *rdev = (struct regulator_dev *)data;
	struct mt6373_regulator_info *info = rdev_get_drvdata(rdev);

	disable_irq_nosync(info->irq);

	if (regulator_is_enabled_regmap(rdev))
		regulator_notifier_call_chain(rdev, REGULATOR_EVENT_OVER_CURRENT, NULL);

	schedule_delayed_work(&info->oc_work, msecs_to_jiffies(OC_IRQ_ENABLE_DELAY_MS));

	return IRQ_HANDLED;
}

static int mt6373_set_ocp(struct regulator_dev *rdev, int lim, int severity, bool enable)
{
	struct mt6373_regulator_info *info = rdev_get_drvdata(rdev);

	/* MT6373 supports only enabling protection and does not support limits */
	if (lim || severity != REGULATOR_SEVERITY_PROT || !enable)
		return -EINVAL;

	/* If there is no OCP interrupt, there's nothing to set */
	if (info->irq <= 0)
		return -EINVAL;

	return devm_request_threaded_irq(&rdev->dev, info->irq, NULL,
					 mt6373_oc_isr, IRQF_ONESHOT,
					 info->desc.name, rdev);
}


static const struct regulator_ops mt6373_vreg_setclr_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = mt6373_vreg_enable_setclr,
	.disable = mt6373_vreg_disable_setclr,
	.is_enabled = regulator_is_enabled_regmap,
	.set_mode = mt6373_regulator_set_mode,
	.get_mode = mt6373_regulator_get_mode,
	.set_over_current_protection = mt6373_set_ocp,
};

static const struct regulator_ops mt6373_ldo_linear_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_mode = mt6373_regulator_set_mode,
	.get_mode = mt6373_regulator_get_mode,
	.set_over_current_protection = mt6373_set_ocp,
};

static const struct regulator_ops mt6373_ldo_vtable_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_mode = mt6373_regulator_set_mode,
	.get_mode = mt6373_regulator_get_mode,
	.set_over_current_protection = mt6373_set_ocp,
};

static const struct regulator_ops mt6373_vmch_eint_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = mt6373_vmch_eint_enable,
	.disable = mt6373_vmch_eint_disable,
	.is_enabled = regulator_is_enabled_regmap,
	.set_mode = mt6373_regulator_set_mode,
	.get_mode = mt6373_regulator_get_mode,
	.set_over_current_protection = mt6373_set_ocp,
};

/* The array is indexed by id(MT6373_ID_XXX) */
static struct mt6373_regulator_info mt6373_regulators[] = {
	MT6373_BUCK("vbuck0", VBUCK0, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK0_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK0_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK0_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK0_FCCM_BIT),
	MT6373_BUCK("vbuck1", VBUCK1, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK1_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK1_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK1_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK1_FCCM_BIT),
	MT6373_BUCK("vbuck2", VBUCK2, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK2_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK2_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK2_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK2_FCCM_BIT),
	MT6373_BUCK("vbuck3", VBUCK3, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK3_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK3_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK3_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK3_FCCM_BIT),
	MT6373_BUCK("vbuck4", VBUCK4, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK4_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK4_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK4_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK4_FCCM_BIT),
	MT6373_BUCK("vbuck4-ufs", VBUCK4_UFS, 0, 2650125, 13875,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK4_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK4_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK4_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_1_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK4_FCCM_BIT),
	MT6373_BUCK("vbuck5", VBUCK5, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK5_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK5_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK5_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_1_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK5_FCCM_BIT),
	MT6373_BUCK("vbuck6", VBUCK6, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK6_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK6_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK6_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_1_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK6_FCCM_BIT),
	MT6373_BUCK("vbuck7", VBUCK7, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK0_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK7_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK7_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK0_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK7_LP_BIT,
		    MT6373_PMIC_RG_BUCK0_1_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK7_FCCM_BIT),
	MT6373_BUCK("vbuck8", VBUCK8, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK1_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK8_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK8_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK1_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK8_LP_BIT,
		    MT6373_PMIC_RG_BUCK1_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK8_FCCM_BIT),
	MT6373_BUCK("vbuck9", VBUCK9, 0, 1193750, 6250,
		    MT6373_PMIC_RG_BUCK1_EN_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK9_EN_BIT,
		    MT6373_PMIC_RG_BUCK_VBUCK9_VOSEL_ADDR,
		    MT6373_PMIC_RG_BUCK_VOSEL_MASK,
		    MT6373_PMIC_RG_BUCK1_LP_ADDR,
		    MT6373_PMIC_RG_BUCK_VBUCK9_LP_BIT,
		    MT6373_PMIC_RG_BUCK1_FCCM_ADDR,
		    MT6373_PMIC_RG_VBUCK9_FCCM_BIT),
	MT6373_LDO_L("vsram-digrf-aif", VSRAM_DIGRF_AIF, 400000, 1193750, 6250,
		     MT6373_PMIC_RG_LDO_VSRAM_DIGRF_AIF_ADDR,
		     MT6373_PMIC_RG_LDO_VSRAM_DIGRF_AIF_VOSEL_ADDR,
		     MT6373_PMIC_RG_LDO_VSRAM_DIGRF_AIF_VOSEL_MASK),
	MT6373_LDO_VT("vusb", VUSB, ldo_volt_table1,
		      MT6373_PMIC_RG_LDO_VUSB_ADDR,
		      MT6373_PMIC_RG_VUSB_VOSEL_ADDR,
		      MT6373_PMIC_RG_VUSB_VOCAL_ADDR),
	MT6373_LDO_VT("vaux18", VAUX18, ldo_volt_table2,
		      MT6373_PMIC_RG_LDO_VAUX18_ADDR,
		      MT6373_PMIC_RG_VAUX18_VOSEL_ADDR,
		      MT6373_PMIC_RG_VAUX18_VOCAL_ADDR),
	MT6373_LDO_VT("vrf13-aif", VRF13_AIF, ldo_volt_table3,
		      MT6373_PMIC_RG_LDO_VRF13_AIF_ADDR,
		      MT6373_PMIC_RG_VRF13_AIF_VOSEL_ADDR,
		      MT6373_PMIC_RG_VRF13_AIF_VOCAL_ADDR),
	MT6373_LDO_VT("vrf18-aif", VRF18_AIF, ldo_volt_table3,
		      MT6373_PMIC_RG_LDO_VRF18_AIF_ADDR,
		      MT6373_PMIC_RG_VRF18_AIF_VOSEL_ADDR,
		      MT6373_PMIC_RG_VRF18_AIF_VOCAL_ADDR),
	MT6373_LDO_VT("vrfio18-aif", VRFIO18_AIF, ldo_volt_table3,
		      MT6373_PMIC_RG_LDO_VRFIO18_AIF_ADDR,
		      MT6373_PMIC_RG_VRFIO18_AIF_VOSEL_ADDR,
		      MT6373_PMIC_RG_VRFIO18_AIF_VOCAL_ADDR),
	MT6373_LDO_VT("vrf09-aif", VRF09_AIF, ldo_volt_table3,
		      MT6373_PMIC_RG_LDO_VRF09_AIF_ADDR,
		      MT6373_PMIC_RG_VRF09_AIF_VOSEL_ADDR,
		      MT6373_PMIC_RG_VRF09_AIF_VOCAL_ADDR),
	MT6373_LDO_VT("vrf12-aif", VRF12_AIF, ldo_volt_table5,
		      MT6373_PMIC_RG_LDO_VRF12_AIF_ADDR,
		      MT6373_PMIC_RG_VRF12_AIF_VOSEL_ADDR,
		      MT6373_PMIC_RG_VRF12_AIF_VOCAL_ADDR),
	MT6373_LDO_VT("vant18", VANT18, ldo_volt_table3,
		      MT6373_PMIC_RG_LDO_VANT18_ADDR,
		      MT6373_PMIC_RG_VANT18_VOSEL_ADDR,
		      MT6373_PMIC_RG_VANT18_VOCAL_ADDR),
	MT6373_LDO_VT("vibr", VIBR, ldo_volt_table1,
		      MT6373_PMIC_RG_LDO_VIBR_ADDR,
		      MT6373_PMIC_RG_VIBR_VOSEL_ADDR,
		      MT6373_PMIC_RG_VIBR_VOCAL_ADDR),
	MT6373_LDO_VT("vio28", VIO28, ldo_volt_table1,
		      MT6373_PMIC_RG_LDO_VIO28_ADDR,
		      MT6373_PMIC_RG_VIO28_VOSEL_ADDR,
		      MT6373_PMIC_RG_VIO28_VOCAL_ADDR),
	MT6373_LDO_VT("vfp", VFP, ldo_volt_table1,
		      MT6373_PMIC_RG_LDO_VFP_ADDR,
		      MT6373_PMIC_RG_VFP_VOSEL_ADDR,
		      MT6373_PMIC_RG_VFP_VOCAL_ADDR),
	MT6373_LDO_VT("vtp", VTP, ldo_volt_table1,
		      MT6373_PMIC_RG_LDO_VTP_ADDR,
		      MT6373_PMIC_RG_VTP_VOSEL_ADDR,
		      MT6373_PMIC_RG_VTP_VOCAL_ADDR),
	MT6373_LDO_VT("vmch", VMCH, ldo_volt_table4,
		      MT6373_PMIC_RG_LDO_VMCH_ADDR,
		      MT6373_PMIC_RG_VMCH_VOSEL_ADDR,
		      MT6373_PMIC_RG_VMCH_VOCAL_ADDR),
	MT6373_LDO_VT("vmc", VMC, ldo_volt_table1,
		      MT6373_PMIC_RG_LDO_VMC_ADDR,
		      MT6373_PMIC_RG_VMC_VOSEL_ADDR,
		      MT6373_PMIC_RG_VMC_VOCAL_ADDR),
	MT6373_LDO_VT("vaud18", VAUD18, ldo_volt_table3,
		      MT6373_PMIC_RG_LDO_VAUD18_ADDR,
		      MT6373_PMIC_RG_VAUD18_VOSEL_ADDR,
		      MT6373_PMIC_RG_VAUD18_VOCAL_ADDR),
	MT6373_LDO_VT("vcn33-1", VCN33_1, ldo_volt_table4,
		      MT6373_PMIC_RG_LDO_VCN33_1_ADDR,
		      MT6373_PMIC_RG_VCN33_1_VOSEL_ADDR,
		      MT6373_PMIC_RG_VCN33_1_VOCAL_ADDR),
	MT6373_LDO_VT("vcn33-2", VCN33_2, ldo_volt_table4,
		      MT6373_PMIC_RG_LDO_VCN33_2_ADDR,
		      MT6373_PMIC_RG_VCN33_2_VOSEL_ADDR,
		      MT6373_PMIC_RG_VCN33_2_VOCAL_ADDR),
	MT6373_LDO_VT("vcn33-3", VCN33_3, ldo_volt_table4,
		      MT6373_PMIC_RG_LDO_VCN33_3_ADDR,
		      MT6373_PMIC_RG_VCN33_3_VOSEL_ADDR,
		      MT6373_PMIC_RG_VCN33_3_VOCAL_ADDR),
	MT6373_LDO_VT("vcn18io", VCN18IO, ldo_volt_table3,
		      MT6373_PMIC_RG_LDO_VCN18IO_ADDR,
		      MT6373_PMIC_RG_VCN18IO_VOSEL_ADDR,
		      MT6373_PMIC_RG_VCN18IO_VOCAL_ADDR),
	MT6373_LDO_VT("vefuse", VEFUSE, ldo_volt_table1,
		      MT6373_PMIC_RG_LDO_VEFUSE_ADDR,
		      MT6373_PMIC_RG_VEFUSE_VOSEL_ADDR,
		      MT6373_PMIC_RG_VEFUSE_VOCAL_ADDR),
	MT6373_LDO_VT_OPS("vmch-eint-high", VMCH_EINT_HIGH,
			  mt6373_vmch_eint_ops, ldo_volt_table4,
			  MT6373_LDO_VMCH_EINT,
			  MT6373_PMIC_RG_VMCH_VOSEL_ADDR,
			  MT6373_PMIC_RG_VMC_VOCAL_ADDR),
	MT6373_LDO_VT_OPS("vmch-eint-low", VMCH_EINT_LOW,
			  mt6373_vmch_eint_ops, ldo_volt_table4,
			  MT6373_LDO_VMCH_EINT,
			  MT6373_PMIC_RG_VMCH_VOSEL_ADDR,
			  MT6373_PMIC_RG_VMC_VOCAL_ADDR),
};

static int mt6373_regulator_probe(struct platform_device *pdev)
{
	struct regulator_config config = {};
	struct mt6373_regulator_info *info;
	struct regulator_dev *rdev;
	bool is_vbuck4_hw_ctrl;
	bool is_cw_variant;
	int i, ret;
	u32 val;

	config.regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!config.regmap)
		return dev_err_probe(&pdev->dev, -ENODEV, "Cannot get regmap\n");

	config.dev = &pdev->dev;

	/* Read PMIC variant information */
	ret = regmap_read(config.regmap, MT6373_PLG_CFG_ELR1, &val);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Cannot read ID register\n");

	val = FIELD_GET(MT6373_ELR_VARIANT_MASK, val);
	is_cw_variant = (val == MT6373_ELR_VARIANT_MT6373CW);

	/* Read Reserved-SW information */
	ret = regmap_read(config.regmap, MT6373_RG_RSV_SWREG_H, &val);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Cannot read RSV_SW register\n");

	is_vbuck4_hw_ctrl = val & MT6373_RG_RSV_SWREG_VBUCK4_HW_CTRL;

	for (i = 0; i < ARRAY_SIZE(mt6373_regulators); i++) {
		info = &mt6373_regulators[i];

		/* MT6373CW does not have a VBUCK4_UFS regulator */
		if (is_cw_variant && info->desc.id == MT6373_ID_VBUCK4_UFS)
			continue;

		/* VBUCK4 vreg software control is not allowed if in hw_ctrl mode */
		if (is_vbuck4_hw_ctrl && info->desc.id == MT6373_ID_VBUCK4)
			continue;

		info->irq = platform_get_irq_byname_optional(pdev, info->desc.name);

		config.driver_data = info;
		rdev = devm_regulator_register(&pdev->dev, &info->desc, &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "failed to register %s\n", info->desc.name);

		if (info->irq > 0)
			INIT_DELAYED_WORK(&info->oc_work, mt6373_oc_irq_enable_work);
	}

	return 0;
}

static void mt6373_regulator_shutdown(struct platform_device *pdev)
{
	struct regmap *regmap;

	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!regmap) {
		dev_err(&pdev->dev, "Cannot get regmap for shutdown!\n");
		return;
	}

	regmap_write(regmap, MT6373_TOP_CFG_ELR5, MT6373_TOP_CFG_ELR5_SHUTDOWN);
}

static const struct of_device_id mt6373_regulator_match[] = {
	{ .compatible = "mediatek,mt6373-regulator" },
	{ /* sentinel */ }
};

static struct platform_driver mt6373_regulator_driver = {
	.driver = {
		.name = "mt6373-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = mt6373_regulator_match,
	},
	.probe = mt6373_regulator_probe,
	.shutdown = mt6373_regulator_shutdown
};
module_platform_driver(mt6373_regulator_driver);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6373 PMIC Regulator Driver");
MODULE_LICENSE("GPL");
