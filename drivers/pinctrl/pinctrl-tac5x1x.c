// SPDX-License-Identifier: GPL-2.0
/*
 * TAC5X1X Pinctrl and GPIO driver
 *
 * Copyright (C) 2023-2025 Texas Instruments Incorporated - https://www.ti.com
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/mfd/tac5x1x/registers.h>
#include <linux/mfd/tac5x1x/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/string_choices.h>

#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinmux.h>

#include "pinctrl-utils.h"

/* 2 pins can be gpio */
#define TAC5X1X_NUM_GPIO_PINS 5
#define TAC5X1X_MAX_PINS 5

struct tac5x1x_pin {
	struct gpio_chip gpio_chip;
	struct regmap *regmap;
	struct device *dev;
	struct tac5x1x *parent;
};

static const struct pinctrl_pin_desc tac5x1x_pin_pins[] = {
	/* naming as per data sheet */
	PINCTRL_PIN(0, "GPIO_1"),
	PINCTRL_PIN(1, "GPIO_2"),
	PINCTRL_PIN(2, "GPO_1"), /* same as GPO1A*/
	PINCTRL_PIN(3, "GPI_1"), /* same as GPI1A*/
	PINCTRL_PIN(4, "GPI_2A")
};

/* pin control registers */
static const u32 pin_to_reg[] = {
	TAC5X1X_GPIO1,
	TAC5X1X_GPIO2,
	TAC5X1X_GPO1,
	TAC5X1X_GPI1,
	TAC5X1X_GPI1,
};

static_assert(ARRAY_SIZE(tac5x1x_pin_pins) == ARRAY_SIZE(pin_to_reg));

enum tac5x1x_gpio_pins {
	PIN_GPIO1 = 0,
	PIN_GPIO2,
	PIN_GPO1,
	PIN_GPI1,
	PIN_GPI2A,
};

enum tac5x1x_pin_funcs {
	TAC5X1X_FUNC_GPIO,
	TAC5X1X_FUNC_PDM,
	TAC5X1X_FUNC_IRQ,
	TAC5X1X_FUNC_MAX
};

struct tac5x1x_config {
	u32 reg;
	u32 mask;
	u32 val;
};

static const unsigned int tac5x1x_pin_gpio1_pins[] = { PIN_GPIO1 };
static const unsigned int tac5x1x_pin_gpio2_pins[] = { PIN_GPIO2 };
static const unsigned int tac5x1x_pin_gpo1_pins[] = { PIN_GPO1 };
static const unsigned int tac5x1x_pin_gpi1_pins[] = { PIN_GPI1 };
static const unsigned int tac5x1x_pin_gpi2a_pins[] = { PIN_GPI2A };

/* pdm pins for - clk, data in */
static const unsigned int tac5x1x_pin_pdm01_pins[] = { PIN_GPIO1, PIN_GPIO2 };
static const unsigned int tac5x1x_pin_pdm03_pins[] = { PIN_GPIO1, PIN_GPI1 };
static const unsigned int tac5x1x_pin_pdm04_pins[] = { PIN_GPIO1, PIN_GPI2A };
static const unsigned int tac5x1x_pin_pdm10_pins[] = { PIN_GPIO2, PIN_GPIO1 };
static const unsigned int tac5x1x_pin_pdm13_pins[] = { PIN_GPIO2, PIN_GPI1 };
static const unsigned int tac5x1x_pin_pdm14_pins[] = { PIN_GPIO2, PIN_GPI2A };
static const unsigned int tac5x1x_pin_pdm20_pins[] = { PIN_GPO1, PIN_GPIO1 };
static const unsigned int tac5x1x_pin_pdm21_pins[] = { PIN_GPO1, PIN_GPIO2 };
static const unsigned int tac5x1x_pin_pdm23_pins[] = { PIN_GPO1, PIN_GPI1 };
static const unsigned int tac5x1x_pin_pdm24_pins[] = { PIN_GPO1, PIN_GPI2A };

#define TAC5X1X_CFG_ENTRY(_reg, _mask, _val) \
	{				\
		.reg = TAC5X1X_##_reg,	\
		.mask = TAC5X1X_##_mask,\
		.val = TAC5X1X_##_val	\
	}

static const struct tac5x1x_config pdm_cfgs[] = {
	/* pdm01 */
	TAC5X1X_CFG_ENTRY(GPIO1, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPIO2, GPIOX_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI02),

	/* pdm03 */
	TAC5X1X_CFG_ENTRY(GPIO1, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPI1, GPI1_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI1),

	/* pdm04 */
	TAC5X1X_CFG_ENTRY(GPIO1, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPI1, GPI2A_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI2A),

	/* pdm10 */
	TAC5X1X_CFG_ENTRY(GPIO2, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPIO1, GPIOX_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI01),

	/* pdm13 */
	TAC5X1X_CFG_ENTRY(GPIO2, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPI1, GPI1_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI1),

	/* pdm14 */
	TAC5X1X_CFG_ENTRY(GPIO2, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPI1, GPI2A_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI2A),

	/* pdm20 */
	TAC5X1X_CFG_ENTRY(GPO1, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPIO1, GPIOX_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI01),

	/* pdm21 */
	TAC5X1X_CFG_ENTRY(GPO1, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPIO2, GPIOX_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI02),

	/* pdm23 */
	TAC5X1X_CFG_ENTRY(GPO1, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPI1, GPI1_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI1),

	/* pdm24 */
	TAC5X1X_CFG_ENTRY(GPO1, GPIOX_CFG_MASK, GPIO_PDMCLK),
	TAC5X1X_CFG_ENTRY(GPI1, GPI2A_CFG_MASK, GPIO_GPI),
	TAC5X1X_CFG_ENTRY(INTF4, PDM_DIN12_SEL_MASK, PDM_DIN_GPI2A),
};

static const struct tac5x1x_config irq_cfgs[] = {
	TAC5X1X_CFG_ENTRY(GPIO1, GPIOX_CFG_MASK, GPIO_IRQ),
	TAC5X1X_CFG_ENTRY(GPIO2, GPIOX_CFG_MASK, GPIO_IRQ),
	TAC5X1X_CFG_ENTRY(GPO1, GPIOX_CFG_MASK, GPIO_IRQ),
};

struct tac5x1x_pingroup {
	const char *name;
	const unsigned int *pins;
	const struct tac5x1x_config *regdata;
	size_t npins; /* number of pins */
	size_t nregdata; /* number of regdata */
};

#define TAC5X1X_PINCTRL_PINGROUP(_name, _pins, _npins, _regdata, _nregdata) \
((struct tac5x1x_pingroup) {		\
	.name = (_name),		\
	.pins = (_pins),		\
	.npins = (_npins),		\
	.regdata = (_regdata),		\
	.nregdata = (_nregdata),	\
})

static const struct tac5x1x_pingroup tac5x1x_pin_groups[] = {
	TAC5X1X_PINCTRL_PINGROUP("gpio1", tac5x1x_pin_gpio1_pins,
				 ARRAY_SIZE(tac5x1x_pin_gpio1_pins), 0, 0),
	TAC5X1X_PINCTRL_PINGROUP("gpio2", tac5x1x_pin_gpio2_pins,
				 ARRAY_SIZE(tac5x1x_pin_gpio2_pins), 0, 0),
	TAC5X1X_PINCTRL_PINGROUP("gpo1", tac5x1x_pin_gpo1_pins,
				 ARRAY_SIZE(tac5x1x_pin_gpo1_pins), 0, 0),
	TAC5X1X_PINCTRL_PINGROUP("gpi1", tac5x1x_pin_gpi1_pins,
				 ARRAY_SIZE(tac5x1x_pin_gpi1_pins), 0, 0),
	TAC5X1X_PINCTRL_PINGROUP("gpi2a", tac5x1x_pin_gpi2a_pins,
				 ARRAY_SIZE(tac5x1x_pin_gpi2a_pins), 0, 0),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpio1_gpio2", tac5x1x_pin_pdm01_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm01_pins),
				 &pdm_cfgs[0], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpio1_gpi1", tac5x1x_pin_pdm03_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm03_pins),
				 &pdm_cfgs[3], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpio1_gpi2a", tac5x1x_pin_pdm04_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm04_pins),
				 &pdm_cfgs[6], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpio2_gpio1", tac5x1x_pin_pdm10_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm10_pins),
				 &pdm_cfgs[9], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpio2_gpi1", tac5x1x_pin_pdm13_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm13_pins),
				 &pdm_cfgs[12], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpio2_gpi2a", tac5x1x_pin_pdm14_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm14_pins),
				 &pdm_cfgs[15], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpo1_gpio1", tac5x1x_pin_pdm20_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm20_pins),
				 &pdm_cfgs[18], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpo1_gpio2", tac5x1x_pin_pdm21_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm21_pins),
				 &pdm_cfgs[21], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpo1_gpi1", tac5x1x_pin_pdm23_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm23_pins),
				 &pdm_cfgs[24], 3),
	TAC5X1X_PINCTRL_PINGROUP("pdm_gpo1_gpi2a", tac5x1x_pin_pdm24_pins,
				 ARRAY_SIZE(tac5x1x_pin_pdm24_pins),
				 &pdm_cfgs[27], 3),
};

static int tac5x1x_pin_get_groups_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(tac5x1x_pin_groups);
}

static const char *tac5x1x_pin_get_group_name(struct pinctrl_dev *pctldev,
					      unsigned int group)
{
	return tac5x1x_pin_groups[group].name;
}

static int tac5x1x_pin_get_group_pins(struct pinctrl_dev *pctldev,
				      unsigned int group,
				      const unsigned int **pins,
				      unsigned int *num_pins)
{
	*pins = tac5x1x_pin_groups[group].pins;
	*num_pins = tac5x1x_pin_groups[group].npins;
	return 0;
}

static const struct pinctrl_ops tac5x1x_pin_group_ops = {
	.get_groups_count = tac5x1x_pin_get_groups_count,
	.get_group_name = tac5x1x_pin_get_group_name,
	.get_group_pins = tac5x1x_pin_get_group_pins,
#if IS_ENABLED(CONFIG_OF)
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinconf_generic_dt_free_map,
#endif
};

static const char *const tac5x1x_pin_funcs[] = {
	"gpio", "pdm", "irq"
};

static const char * const tac5x1x_pin_gpio_groups[] = {
	"gpio1", "gpio2", "gpo1", "gpi1", "gpi2a"
};

static const char * const tac5x1x_pin_pdm_groups[] = {
	"pdm_gpio1_gpio2",
	"pdm_gpio1_gpi1",
	"pdm_gpio1_gpi2a",
	"pdm_gpio2_gpio1",
	"pdm_gpio2_gpi1",
	"pdm_gpio2_gpi2a",
	"pdm_gpo1_gpio1",
	"pdm_gpo1_gpio2",
	"pdm_gpo1_gpi1",
	"pdm_gpo1_gpi2a"
};

static const char * const tac5x1x_pin_irq_groups[] = {
	"gpio1", "gpio2", "gpo1"
};

static const struct pinfunction tac5x1x_pin_func_groups[] = {
	PINCTRL_PINFUNCTION("gpio", tac5x1x_pin_gpio_groups,
			    ARRAY_SIZE(tac5x1x_pin_gpio_groups)),
	PINCTRL_PINFUNCTION("pdm",  tac5x1x_pin_pdm_groups,
			    ARRAY_SIZE(tac5x1x_pin_pdm_groups)),
	PINCTRL_PINFUNCTION("irq",  tac5x1x_pin_irq_groups,
			    ARRAY_SIZE(tac5x1x_pin_irq_groups)),
};

static_assert(ARRAY_SIZE(tac5x1x_pin_funcs) == TAC5X1X_FUNC_MAX);
static_assert(ARRAY_SIZE(tac5x1x_pin_func_groups) == TAC5X1X_FUNC_MAX);

static int tac5x1x_pin_get_func_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(tac5x1x_pin_funcs);
}

static const char *tac5x1x_pin_get_func_name(struct pinctrl_dev *pctldev,
					     unsigned int func_idx)
{
	return tac5x1x_pin_funcs[func_idx];
}

static int tac5x1x_pin_get_func_groups(struct pinctrl_dev *pctldev,
				       unsigned int func_idx,
				       const char * const **groups,
				       unsigned int * const num_groups)
{
	*groups = tac5x1x_pin_func_groups[func_idx].groups;
	*num_groups = tac5x1x_pin_func_groups[func_idx].ngroups;

	return 0;
}

static int tac5x1x_setup_pdm(struct pinctrl_dev *pinctrl, u32 group)
{
	int ret = 0, i;
	u32 val;
	size_t reg_count;
	const struct tac5x1x_config *regdata;
	struct tac5x1x_pin *priv;
	const struct tac5x1x_pingroup *grp = &tac5x1x_pin_groups[group];

	regdata = grp->regdata;
	reg_count = grp->nregdata;
	priv = pinctrl_dev_get_drvdata(pinctrl);

	/*
	 * PDM pin groups use naming convention: pdm_<clk_pin>_<data_pin>
	 * First pin in the group is PDM Clock (output)
	 * Second pin in the group is PDM Data (input)
	 */
	if (grp->npins >= 2) {
		dev_dbg(priv->dev, "PDM config: %s CLK=%s DATA=%s\n", grp->name,
			tac5x1x_pin_pins[grp->pins[0]].name,
			tac5x1x_pin_pins[grp->pins[1]].name);
	}

	for (i = 0; i < reg_count; i++) {
		val = (regdata[i].val << (ffs(regdata[i].mask) - 1)) & regdata[i].mask;
		ret = regmap_update_bits(priv->regmap, regdata[i].reg,
					 regdata[i].mask, val);
		if (ret) {
			dev_err(priv->dev, "pdm setup failed, reg=%x err=%d",
				regdata[i].reg, ret);
			break;
		}
	}

	/* Mark PDM as enabled in parent structure for codec driver */
	if (!ret)
		priv->parent->pdm_enabled = true;

	return ret;
}

/* configure a gpio pin to generate HW interrupt */
static int tac5x1x_setup_irq(struct pinctrl_dev *pinctrl, u32 group)
{
	struct tac5x1x_pin *priv = pinctrl_dev_get_drvdata(pinctrl);
	const struct tac5x1x_config *const cfg = &irq_cfgs[group];
	u32 val;

	dev_dbg(priv->dev, "Configuring %s as IRQ output\n",
		tac5x1x_pin_groups[group].name);
	val = (cfg->val << (ffs(cfg->mask) - 1)) & cfg->mask;
	return regmap_update_bits(priv->regmap, cfg->reg, cfg->mask, val);
}

static int tac5x1x_pin_set_mux(struct pinctrl_dev *pinctrl,
			       unsigned int func_idx, unsigned int group)
{
	int ret;
	struct tac5x1x_pin *priv = pinctrl_dev_get_drvdata(pinctrl);

	dev_dbg(priv->dev, "%s Setting %s(%d) to %s(%d)\n", __func__,
		tac5x1x_pin_groups[group].name, group,
		tac5x1x_pin_funcs[func_idx], func_idx);

	switch (func_idx) {
	case TAC5X1X_FUNC_GPIO:
		ret = 0;
		break;
	case TAC5X1X_FUNC_PDM:
		ret = tac5x1x_setup_pdm(pinctrl, group);
		break;
	case TAC5X1X_FUNC_IRQ:
		ret = tac5x1x_setup_irq(pinctrl, group);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int tac5x1x_gpio_set_direction(struct pinctrl_dev *pctldev,
				      struct pinctrl_gpio_range *range,
				      unsigned int offset, bool input)
{
	int ret;
	u32 val;
	struct tac5x1x_pin *priv;
	struct device *parent;

	priv = pinctrl_dev_get_drvdata(pctldev);
	parent = priv->dev->parent;

	dev_dbg(priv->dev, "Setting gpio offset=%s to %s\n",
		tac5x1x_pin_pins[offset].name, input ? "input" : "output");

	ret = pm_runtime_resume_and_get(parent);
	if (ret) {
		dev_err(priv->dev, "Failed to resume for direction: %d\n", ret);
		return ret;
	}

	val = (input ? TAC5X1X_GPIO_GPI : TAC5X1X_GPIO_GPO) <<
		TAC5X1X_GPIOX_CFG_SHFT;

	switch (offset) {
	case PIN_GPIO1:
	case PIN_GPIO2:
		ret = regmap_update_bits(priv->regmap,
					 pin_to_reg[offset],
					 TAC5X1X_GPIOX_CFG_MASK,
					 val);
		break;
	case PIN_GPO1:
		if (input) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = regmap_update_bits(priv->regmap, pin_to_reg[offset],
					 TAC5X1X_GPIOX_CFG_MASK, val);
		break;
	case PIN_GPI1:
		if (!input) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = regmap_update_bits(priv->regmap, pin_to_reg[offset],
					 TAC5X1X_GPI1_EN_MASK, TAC5X1X_GPI1_EN_MASK);
		break;
	case PIN_GPI2A:
		if (!input) {
			ret = -EOPNOTSUPP;
			break;
		}
		ret = regmap_update_bits(priv->regmap, pin_to_reg[offset],
					 TAC5X1X_GPI2_EN_MASK, TAC5X1X_GPI2_EN_MASK);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	if (ret)
		dev_err(priv->dev, "Failed to set gpio%d direction: %d\n",
			offset + 1, ret);

	pm_runtime_mark_last_busy(parent);
	pm_runtime_put_autosuspend(parent);

	return ret;
}

static int tac5x1x_gpio_request_enable(struct pinctrl_dev *pctldev,
				       struct pinctrl_gpio_range *range,
				       unsigned int offset)
{
	return tac5x1x_pin_set_mux(pctldev, TAC5X1X_FUNC_GPIO, offset);
}

static void tac5x1x_gpio_disable_free(struct pinctrl_dev *pctldev,
				      struct pinctrl_gpio_range *range,
				      unsigned int offset)
{
	struct tac5x1x_pin *priv = pinctrl_dev_get_drvdata(pctldev);
	struct device *parent = priv->dev->parent;
	int ret;

	/* Ensure parent device is resumed before register access */
	ret = pm_runtime_resume_and_get(parent);
	if (ret) {
		dev_warn(priv->dev, "PM resume failed for gpio_disable_free: %d (ignoring)\n", ret);
		return;
	}

	regmap_update_bits(priv->regmap, pin_to_reg[offset],
			   TAC5X1X_GPIOX_CFG_MASK, 0);

	pm_runtime_mark_last_busy(parent);
	pm_runtime_put_autosuspend(parent);
}

static const struct pinmux_ops tac5x1x_pin_mux_ops = {
	.get_functions_count	= tac5x1x_pin_get_func_count,
	.get_function_name	= tac5x1x_pin_get_func_name,
	.get_function_groups	= tac5x1x_pin_get_func_groups,

	.set_mux		= tac5x1x_pin_set_mux,

	.gpio_request_enable	= tac5x1x_gpio_request_enable,
	.gpio_disable_free	= tac5x1x_gpio_disable_free,
	.gpio_set_direction	= tac5x1x_gpio_set_direction,

	.strict			= true,
};

static int tac5x1x_pin_config_get(struct pinctrl_dev *pctldev,
				  u32 pin, unsigned long *config)
{
	struct tac5x1x_pin *priv = pinctrl_dev_get_drvdata(pctldev);
	struct device *parent = priv->dev->parent;
	u32 param = pinconf_to_config_param(*config);
	u32 reg, value, drive_mode;
	int ret;

	ret = pm_runtime_resume_and_get(parent);
	if (ret) {
		dev_err(priv->dev, "Failed to resume for config: %d\n", ret);
		return ret;
	}

	if (pin >= TAC5X1X_MAX_PINS) {
		ret = -EINVAL;
		goto out;
	}

	dev_dbg(priv->dev, "Get config for %s, param=%d\n",
		tac5x1x_pin_pins[pin].name, param);

	reg = pin_to_reg[pin];
	ret = regmap_read(priv->regmap, reg, &value);
	if (ret) {
		dev_err(priv->dev, "Failed to get config: %d\n", ret);
		goto out;
	}

	drive_mode = (value & TAC5X1X_GPIOX_DRV_MASK) >> TAC5X1X_GPIOX_DRV_SHFT;
	switch (drive_mode) {
	case TAC5X1X_GPIO_DRV_HIZ:
		*config = pinconf_to_config_packed(PIN_CONFIG_BIAS_HIGH_IMPEDANCE, 1);
		break;
	case TAC5X1X_GPIO_DRV_ALAH:
		*config = pinconf_to_config_packed(PIN_CONFIG_DRIVE_PUSH_PULL, 1);
		break;
	case TAC5X1X_GPIO_DRV_ALWH:
		*config = pinconf_to_config_packed(PIN_CONFIG_BIAS_PULL_UP, 1);
		break;
	case TAC5X1X_GPIO_DRV_ALHIZ:
		*config = pinconf_to_config_packed(PIN_CONFIG_DRIVE_OPEN_DRAIN, 1);
		break;
	case TAC5X1X_GPIO_DRV_WLAH:
		*config = pinconf_to_config_packed(PIN_CONFIG_BIAS_PULL_DOWN, 1);
		break;
	case TAC5X1X_GPIO_DRV_HIZAH:
		*config = pinconf_to_config_packed(PIN_CONFIG_DRIVE_OPEN_SOURCE, 1);
		break;
	default:
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = 0;
out:
	pm_runtime_mark_last_busy(parent);
	pm_runtime_put_autosuspend(parent);
	return ret;
}

static int tac5x1x_pin_config_set(struct pinctrl_dev *pctldev, unsigned int pin,
				  unsigned long *configs, unsigned int num_configs)
{
	struct tac5x1x_pin *priv = pinctrl_dev_get_drvdata(pctldev);
	unsigned int val, param;
	int ret = 0;

	/*
	 * push-pull	-> active low, active high
	 * pull up	-> active low, weak high (on-chip pullup)
	 * pull down	-> weak low, active high (on-chip pulldown)
	 * open-drain	-> active low, hiz
	 * open-source	-> hiz active low
	 */
	while (num_configs) {
		val = pinconf_to_config_argument(*configs);
		param = pinconf_to_config_param(*configs);
		dev_dbg(priv->dev,
			"set config for name=%s (pin=%d), param=%u val=%u\n",
			tac5x1x_pin_pins[pin].name, pin, param, val);

		switch (param) {
		case PIN_CONFIG_DRIVE_PUSH_PULL:
			val = TAC5X1X_GPIO_DRV_ALAH << TAC5X1X_GPIOX_DRV_SHFT;
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			val = TAC5X1X_GPIO_DRV_ALWH << TAC5X1X_GPIOX_DRV_SHFT;
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			val = TAC5X1X_GPIO_DRV_WLAH << TAC5X1X_GPIOX_DRV_SHFT;
			break;
		case PIN_CONFIG_DRIVE_OPEN_DRAIN:
			val = TAC5X1X_GPIO_DRV_ALHIZ << TAC5X1X_GPIOX_DRV_SHFT;
			break;
		case PIN_CONFIG_DRIVE_OPEN_SOURCE:
			val = TAC5X1X_GPIO_DRV_HIZAH << TAC5X1X_GPIOX_DRV_SHFT;
			break;
		case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
			val = TAC5X1X_GPIO_DRV_HIZ << TAC5X1X_GPIOX_DRV_SHFT;
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}

		/* input pin can't have driver strength */
		if (pin != PIN_GPI1 && pin != PIN_GPI2A) {
			ret = regmap_update_bits(priv->regmap, pin_to_reg[pin],
						 TAC5X1X_GPIOX_DRV_MASK, val);
			if (ret)
				dev_err(priv->dev,
					"pinconfig error for %s(%x), err=%d",
					tac5x1x_pin_pins[pin].name,
					pin_to_reg[pin], ret);
		}

		if (ret)
			break;

		configs++;
		num_configs--;
	}

	return ret;
}

static int tac5x1x_pin_config_group_get(struct pinctrl_dev *pctldev,
					unsigned int selector, unsigned long *config)
{
	int i, ret = 0;

	for (i = 0; i < tac5x1x_pin_groups[selector].npins; ++i) {
		ret = tac5x1x_pin_config_get(pctldev,
					     tac5x1x_pin_groups[selector].pins[i],
					     config);
		if (ret)
			break;
	}

	return ret;
}

static int tac5x1x_pin_config_group_set(struct pinctrl_dev *pctldev,
					unsigned int selector,
					unsigned long *configs,
					unsigned int num_configs)
{
	int i, ret = 0;

	for (i = 0; i < tac5x1x_pin_groups[selector].npins; ++i) {
		ret = tac5x1x_pin_config_set(pctldev,
					     tac5x1x_pin_groups[selector].pins[i],
					     configs, num_configs);
		if (ret)
			break;
	}

	return 0;
}

static const struct pinconf_ops tac5x1x_pin_conf_ops = {
	.is_generic		= true,

	.pin_config_get		= tac5x1x_pin_config_get,
	.pin_config_set		= tac5x1x_pin_config_set,
	.pin_config_group_get	= tac5x1x_pin_config_group_get,
	.pin_config_group_set	= tac5x1x_pin_config_group_set,
};

static const struct pinctrl_desc tac5x1x_pin_desc = {
	.name		= "tac5x1x-pinctrl",
	.owner		= THIS_MODULE,

	.pins		= tac5x1x_pin_pins,
	.npins		= ARRAY_SIZE(tac5x1x_pin_pins),

	.pctlops	= &tac5x1x_pin_group_ops,
	.pmxops		= &tac5x1x_pin_mux_ops,
	.confops	= &tac5x1x_pin_conf_ops,
};

static int tac5x1x_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct tac5x1x_pin *priv = gpiochip_get_data(chip);
	struct device *parent = priv->dev->parent;
	unsigned int val;
	int ret;

	ret = pm_runtime_resume_and_get(parent);
	if (ret) {
		dev_err(priv->dev, "Failed to resume for get: %d\n", ret);
		return ret;
	}

	ret = regmap_read(priv->regmap, TAC5X1X_GPIOVAL, &val);
	if (ret) {
		dev_err(priv->dev, "Failed to get gpio%d: %d\n", offset + 1, ret);
		goto done_gpio_get;
	}

	switch (offset) {
	case PIN_GPIO1:
	case PIN_GPIO2:
		ret = !!(val & BIT(3 - offset));
		break;
	case PIN_GPI1:
		ret = !!(val & BIT(1));
		break;
	case PIN_GPI2A:
		ret = !!(val & BIT(2));
		break;
	case PIN_GPO1:
	default:
		ret = -EINVAL;
	}

done_gpio_get:
	pm_runtime_mark_last_busy(parent);
	pm_runtime_put_autosuspend(parent);

	return ret;
}

static int tac5x1x_gpio_set(struct gpio_chip *chip, unsigned int offset,
			    int value)
{
	struct tac5x1x_pin *priv = gpiochip_get_data(chip);
	struct device *parent = priv->dev->parent;
	u32 mask, val;
	int ret;

	dev_dbg(priv->dev, "setting %s to %d\n",
		tac5x1x_pin_pins[offset].name, value);

	ret = pm_runtime_resume_and_get(parent);
	if (ret)
		return ret;

	switch (offset) {
	case PIN_GPIO1:
	case PIN_GPIO2:
	case PIN_GPO1:
		mask = BIT(7 - offset);
		val = value ? mask : 0;
		ret = regmap_update_bits(priv->regmap, TAC5X1X_GPIOVAL, mask,
					 val);
		break;
	case PIN_GPI1:
	case PIN_GPI2A:
		dev_err(priv->dev, "gpi1/2a are input only, set not supported\n");
		ret = -EINVAL;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	pm_runtime_mark_last_busy(parent);
	pm_runtime_put_autosuspend(parent);

	return ret;
}

static int tac5x1x_gpio_direction_in(struct gpio_chip *chip,
				     unsigned int offset)
{
	struct tac5x1x_pin *priv = gpiochip_get_data(chip);

	dev_dbg(priv->dev, "setting %s to input",
		tac5x1x_pin_pins[offset].name);

	/* input is not supported in GPO1 pin */
	if (offset == PIN_GPO1) {
		dev_err(priv->dev, "gpo1 is output only, read is not supported\n");
		return -EOPNOTSUPP;
	}

	return pinctrl_gpio_direction_input(chip, offset);
}

static int tac5x1x_gpio_direction_out(struct gpio_chip *chip,
				      unsigned int offset, int value)
{
	int ret;

	ret = tac5x1x_gpio_set(chip, offset, value);
	if (ret)
		return ret;

	return pinctrl_gpio_direction_output(chip, offset);
}

static int tac5x1x_gpio_add_pin_ranges(struct gpio_chip *chip)
{
	struct tac5x1x_pin *priv = gpiochip_get_data(chip);
	int ret;

	dev_dbg(priv->dev, "gpio add range");
	ret = gpiochip_add_pin_range(&priv->gpio_chip, priv->gpio_chip.label,
				     0, 0, TAC5X1X_NUM_GPIO_PINS);
	if (ret)
		dev_err(priv->dev, "Failed to add GPIO pin range: %d\n", ret);

	return ret;
}

static int tac5x1x_pin_probe(struct platform_device *pdev)
{
	struct tac5x1x *tac5x1x = dev_get_drvdata(pdev->dev.parent);
	struct tac5x1x_pin *priv;
	struct pinctrl_dev *pctldev;
	struct fwnode_handle *fwnode = dev_fwnode(tac5x1x->dev);
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	priv->regmap = tac5x1x->regmap;
	priv->parent = tac5x1x;

	priv->gpio_chip.request = gpiochip_generic_request;
	priv->gpio_chip.free = gpiochip_generic_free;
	priv->gpio_chip.direction_input = tac5x1x_gpio_direction_in;
	priv->gpio_chip.direction_output = tac5x1x_gpio_direction_out;
	priv->gpio_chip.add_pin_ranges = tac5x1x_gpio_add_pin_ranges;
	priv->gpio_chip.get = tac5x1x_gpio_get;
	priv->gpio_chip.set = tac5x1x_gpio_set;
	priv->gpio_chip.label = dev_name(priv->dev);
	priv->gpio_chip.parent = priv->dev;
	priv->gpio_chip.can_sleep = true;
	priv->gpio_chip.base = -1; /* no base*/
	priv->gpio_chip.ngpio = TAC5X1X_NUM_GPIO_PINS;

	if (is_of_node(fwnode)) {
		fwnode = fwnode_get_named_child_node(fwnode, "pinctrl");

		if (fwnode && !fwnode->dev)
			fwnode->dev = priv->dev;
	}

	priv->gpio_chip.fwnode = fwnode;

	device_set_node(priv->dev, fwnode);

	pctldev = devm_pinctrl_register(priv->dev, &tac5x1x_pin_desc, priv);
	if (IS_ERR(pctldev))
		return dev_err_probe(priv->dev, PTR_ERR(pctldev),
				     "Failed to register pinctrl\n");

	ret = devm_gpiochip_add_data(priv->dev, &priv->gpio_chip, priv);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "Failed to register gpiochip\n");

	return 0;
}

static const struct platform_device_id tac5x1x_pin_id_table[] = {
	{ "tac5x1x-pinctrl", },
	{}
};
MODULE_DEVICE_TABLE(platform, tac5x1x_pin_id_table);

static struct platform_driver tac5x1x_pin_driver = {
	.driver = {
		.name	= "tac5x1x-pinctrl",
	},
	.probe		= tac5x1x_pin_probe,
	.id_table	= tac5x1x_pin_id_table,
};
module_platform_driver(tac5x1x_pin_driver);

MODULE_DESCRIPTION("TAC5X1X Pinctrl Driver");
MODULE_AUTHOR("Niranjan H Y <niranjan.hy@ti.com>");
MODULE_LICENSE("GPL");
