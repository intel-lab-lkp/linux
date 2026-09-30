// SPDX-License-Identifier: GPL-2.0
/*
 * Pinctrl driver for the TI TDA54 SoC
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinctrl-utils.h"
#include "../pinmux.h"

/*
 * Each pad on TDA54 is configured by a pair of PAD Configuration registers:
 *
 *	PADCFGA<n> at PADCFG_BASE + 0x100  + 4 * n
 *	PADCFGB<n> at PADCFG_BASE + 0x1100 + 4 * n
 *
 * PADCFGA holds the mux mode and the static pin configuration. PADCFGB holds
 * the wakeup and retention configuration.
 */
#define TDA54_PADCFGA_BASE		0x100
#define TDA54_PADCFGB_BASE		0x1100
#define TDA54_PADCFG_STRIDE		4

/*
 * Each pinmux entry packs the pad number and the mux mode into a single
 * cell, as described in the binding.
 */
#define TDA54_PINMUX_PAD_SHIFT		8
#define TDA54_PINMUX_MUX_MASK		GENMASK(3, 0)
#define TDA54_PINMUX_PAD(x)		((x) >> TDA54_PINMUX_PAD_SHIFT)
#define TDA54_PINMUX_MUX(x)		((x) & TDA54_PINMUX_MUX_MASK)

/* Size of a single proxy window into the padcfg register space */
#define TDA54_PROXY_STRIDE		0x4000

/*
 * Every register is write protected by the LOCK_KICK pair of its own proxy
 * window.
 */
#define TDA54_LOCK_KICK0		0x3008
#define TDA54_LOCK_KICK0_UNLOCK_VAL	0x68ef3490
#define TDA54_LOCK_KICK1		0x300c
#define TDA54_LOCK_KICK1_UNLOCK_VAL	0xd172bc5a

/*
 * Claim registers, bit b of claim register i corresponds to
 * the register at offset i * TDA54_CLAIM_BLOCK_SIZE + b * 4.
 */
#define TDA54_CLAIM_BASE		0x3100
#define TDA54_CLAIM_REG_STRIDE		0x4
#define TDA54_CLAIM_REGS_PER_WORD	32
#define TDA54_CLAIM_BLOCK_SIZE		(TDA54_CLAIM_REGS_PER_WORD * 0x4)

/* PADCFGA */
#define TDA54_MUXMODE			GENMASK(3, 0)
#define TDA54_VIRT_GPIO		GENMASK(6, 4)
#define TDA54_DEBOUNCE_SEL		GENMASK(14, 12)
#define TDA54_PULLUDEN			BIT(16)	/* 0 = pull enabled */
#define TDA54_PULLTYPESEL		BIT(17)	/* 0 = down, 1 = up */
#define TDA54_RXACTIVE			BIT(18)	/* 1 = receiver enabled */
#define TDA54_TX_DIS			BIT(19)	/* 1 = driver disabled */
#define TDA54_SLEW_CTRL		GENMASK(21, 20)
#define TDA54_HYST_DIS			BIT(25)	/* 1 = hysteresis disabled */
#define TDA54_BIAS			(TDA54_PULLUDEN | TDA54_PULLTYPESEL)

#define TDA54_DEBOUNCE_SEL_MAX		6
#define TDA54_MUX_GPIO			7

/* PADCFGB */
#define TDA54_WKUP_LVL_EN		BIT(0)	/* wkup_lvl_en: 1 = level sensitive */
#define TDA54_WKUP_LVL_POL		BIT(1)	/* wkup_lvl_pol: 0 = LOW, 1 = HIGH */
#define TDA54_WKUP_EN			BIT(8)	/* wkup_en */
#define TDA54_RET_PULLUD_EN		BIT(16)	/* 0 = pull enabled */
#define TDA54_RET_PULLTYPE_SEL		BIT(17)	/* 0 = down, 1 = up */
#define TDA54_RET_OFFMODE_EN		BIT(18)	/* 1 = force to OFF mode value */
#define TDA54_RET_OUT_DIS		BIT(19)	/* 1 = output disabled */
/*
 * Retention Output Value. As Per TDA54 PADCFG documentation the encoding
 * is inverted with respect to the field value names: 0x0 (LOW) drives the pin
 * high, 0x1 (HIGH) drives it low. Named after the level actually driven.
 */
#define TDA54_RET_OUT_LOW		BIT(20)
#define TDA54_FORCE_RET_EN		BIT(24)	/* 1 = override Device Manager */
#define TDA54_ISO_BYP			BIT(28)	/* 1 = I/O isolation bypassed */

#define TDA54_RET_BIAS	(TDA54_RET_PULLUD_EN | TDA54_RET_PULLTYPE_SEL)

/* Custom pinconf parameters */
#define TDA54_PIN_CONFIG_DEBOUNCE_SELECT	(PIN_CONFIG_END + 1)
#define TDA54_PIN_CONFIG_VIRT_GPIO_INSTANCE	(PIN_CONFIG_END + 2)
#define TDA54_PIN_CONFIG_WKUP			(PIN_CONFIG_END + 3)
#define TDA54_PIN_CONFIG_RET_BIAS		(PIN_CONFIG_END + 4)
#define TDA54_PIN_CONFIG_RET_OFFMODE		(PIN_CONFIG_END + 5)
#define TDA54_PIN_CONFIG_RET_OUT		(PIN_CONFIG_END + 6)
#define TDA54_PIN_CONFIG_FORCE_RET		(PIN_CONFIG_END + 7)
#define TDA54_PIN_CONFIG_ISO_BYPASS		(PIN_CONFIG_END + 8)

static const struct pinconf_generic_params tda54_custom_bindings[] = {
	{ "ti,debounce-select", TDA54_PIN_CONFIG_DEBOUNCE_SELECT, 0 },
	{ "ti,virt-gpio-instance", TDA54_PIN_CONFIG_VIRT_GPIO_INSTANCE, 0 },
	{ "ti,wakeup", TDA54_PIN_CONFIG_WKUP, 0 },
	{ "ti,retention-bias", TDA54_PIN_CONFIG_RET_BIAS, 0 },
	{ "ti,retention-offmode", TDA54_PIN_CONFIG_RET_OFFMODE, 0 },
	{ "ti,retention-output", TDA54_PIN_CONFIG_RET_OUT, 0 },
	{ "ti,retention-force", TDA54_PIN_CONFIG_FORCE_RET, 0 },
	{ "ti,isolation-bypass", TDA54_PIN_CONFIG_ISO_BYPASS, 0 },
};

#ifdef CONFIG_DEBUG_FS
static const struct pin_config_item
tda54_conf_items[ARRAY_SIZE(tda54_custom_bindings)] = {
	PCONFDUMP(TDA54_PIN_CONFIG_DEBOUNCE_SELECT, "debounce select", NULL, true),
	PCONFDUMP(TDA54_PIN_CONFIG_VIRT_GPIO_INSTANCE, "virtual gpio instance", NULL, true),
	PCONFDUMP(TDA54_PIN_CONFIG_WKUP, "wakeup", NULL, true),
	PCONFDUMP(TDA54_PIN_CONFIG_RET_BIAS, "retention bias", NULL, true),
	PCONFDUMP(TDA54_PIN_CONFIG_RET_OFFMODE, "retention offmode", NULL, true),
	PCONFDUMP(TDA54_PIN_CONFIG_RET_OUT, "retention output", NULL, true),
	PCONFDUMP(TDA54_PIN_CONFIG_FORCE_RET, "retention force", NULL, true),
	PCONFDUMP(TDA54_PIN_CONFIG_ISO_BYPASS, "isolation bypass", NULL, true),
};
#endif

/**
 * struct tda54_group_data - per group driver data
 * @mux: mux mode to program for each pin of the group
 */
struct tda54_group_data {
	unsigned int *mux;
};

/**
 * struct tda54_pinctrl - TDA54 pin controller driver data
 * @desc: pin controller descriptor registered with the pinctrl core
 * @pctl: pin controller device returned by the pinctrl core
 * @dev: device backing this pin controller
 * @base: base address of the proxy region used to access the registers
 * @proxy: proxy region this instance uses
 * @npins: number of pins exposed on this instance
 * @mutex: protects adding groups and functions
 * @lock: serialises read-modify-write of PADCFG registers
 */
struct tda54_pinctrl {
	struct pinctrl_desc desc;
	struct pinctrl_dev *pctl;
	struct device *dev;
	void __iomem *base;
	u32 proxy;
	u32 npins;
	struct mutex mutex;	/* protects adding groups and functions */
	raw_spinlock_t lock;	/* serialises read-modify-write of PADCFG registers */
};

static unsigned int tda54_padcfga(unsigned int pin)
{
	return TDA54_PADCFGA_BASE + pin * TDA54_PADCFG_STRIDE;
}

static unsigned int tda54_padcfgb(unsigned int pin)
{
	return TDA54_PADCFGB_BASE + pin * TDA54_PADCFG_STRIDE;
}

/*
 * Claim @reg for this proxy before writing it. A register claimed by a
 * proxy can no longer be written through any other proxy's address until
 * that proxy releases the claim.
 */
static void tda54_claim(struct tda54_pinctrl *tp, unsigned int reg)
{
	unsigned int block = reg / TDA54_CLAIM_BLOCK_SIZE;
	unsigned int bit = (reg % TDA54_CLAIM_BLOCK_SIZE) / TDA54_CLAIM_REG_STRIDE;
	unsigned int claim_reg = TDA54_CLAIM_BASE + block * TDA54_CLAIM_REG_STRIDE;
	u32 val;

	val = readl_relaxed(tp->base + claim_reg);
	val |= BIT(bit);
	writel_relaxed(val, tp->base + claim_reg);
}

static void tda54_rmw(struct tda54_pinctrl *tp, unsigned int reg,
		      u32 mask, u32 val)
{
	unsigned long flags;
	u32 tmp;

	raw_spin_lock_irqsave(&tp->lock, flags);

	/* Proxy0 does not need to claim; every other proxy must claim first */
	if (tp->proxy)
		tda54_claim(tp, reg);

	tmp = readl_relaxed(tp->base + reg);
	tmp &= ~mask;
	tmp |= val;
	writel_relaxed(tmp, tp->base + reg);

	raw_spin_unlock_irqrestore(&tp->lock, flags);
}

static void tda54_unlock(struct tda54_pinctrl *tp)
{
	writel_relaxed(TDA54_LOCK_KICK0_UNLOCK_VAL, tp->base + TDA54_LOCK_KICK0);
	writel_relaxed(TDA54_LOCK_KICK1_UNLOCK_VAL, tp->base + TDA54_LOCK_KICK1);
}

static void tda54_pin_dbg_show(struct pinctrl_dev *pctldev, struct seq_file *s,
			       unsigned int pin)
{
	struct tda54_pinctrl *tp = pinctrl_dev_get_drvdata(pctldev);

	seq_printf(s, "PADCFGA %08x PADCFGB %08x",
		   readl_relaxed(tp->base + tda54_padcfga(pin)),
		   readl_relaxed(tp->base + tda54_padcfgb(pin)));
}

static int tda54_dt_node_to_map(struct pinctrl_dev *pctldev,
				struct device_node *np,
				struct pinctrl_map **maps,
				unsigned int *num_maps)
{
	struct tda54_pinctrl *tp = pinctrl_dev_get_drvdata(pctldev);
	struct tda54_group_data *gdata;
	unsigned int *pins, *pinmux;
	unsigned int nmaps = 0;
	unsigned long *configs;
	unsigned int nconfigs;
	const char **grpnames;
	struct pinctrl_map *map;
	const char *grpname;
	int ret, gsel, i, npins;

	npins = of_property_count_u32_elems(np, "pinmux");
	if (npins <= 0) {
		dev_err(tp->dev, "%pOF: missing or empty pinmux property\n", np);
		return -EINVAL;
	}

	/* One map for the mux, one for the configs. */
	map = kcalloc(2, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	pinmux = kcalloc(npins, sizeof(*pinmux), GFP_KERNEL);
	if (!pinmux) {
		kfree(map);
		return -ENOMEM;
	}

	ret = of_property_read_u32_array(np, "pinmux", pinmux, npins);
	if (ret)
		goto out;

	grpname = devm_kasprintf(tp->dev, GFP_KERNEL, "%pOFn", np);
	pins = devm_kcalloc(tp->dev, npins, sizeof(*pins), GFP_KERNEL);
	gdata = devm_kzalloc(tp->dev, sizeof(*gdata), GFP_KERNEL);
	grpnames = devm_kcalloc(tp->dev, 1, sizeof(*grpnames), GFP_KERNEL);
	if (!grpname || !pins || !gdata || !grpnames) {
		ret = -ENOMEM;
		goto out;
	}

	gdata->mux = devm_kcalloc(tp->dev, npins, sizeof(*gdata->mux),
				  GFP_KERNEL);
	if (!gdata->mux) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < npins; i++) {
		unsigned int pad = TDA54_PINMUX_PAD(pinmux[i]);

		if (pad >= tp->npins) {
			dev_err(tp->dev, "%pOF: pad %u out of range\n", np, pad);
			ret = -EINVAL;
			goto out;
		}
		pins[i] = pad;
		gdata->mux[i] = TDA54_PINMUX_MUX(pinmux[i]);
	}

	ret = pinconf_generic_parse_dt_config(np, pctldev, &configs, &nconfigs);
	if (ret)
		goto out;

	mutex_lock(&tp->mutex);

	gsel = pinctrl_generic_add_group(pctldev, grpname, pins, npins, gdata);
	if (gsel < 0) {
		ret = gsel;
		goto unlock;
	}

	grpnames[0] = grpname;
	ret = pinmux_generic_add_function(pctldev, grpname, grpnames, 1, NULL);
	if (ret < 0)
		goto remove_group;

	mutex_unlock(&tp->mutex);

	map[nmaps].type = PIN_MAP_TYPE_MUX_GROUP;
	map[nmaps].data.mux.function = grpname;
	map[nmaps].data.mux.group = grpname;
	nmaps++;

	if (nconfigs) {
		map[nmaps].type = PIN_MAP_TYPE_CONFIGS_GROUP;
		map[nmaps].data.configs.group_or_pin = grpname;
		map[nmaps].data.configs.configs = configs;
		map[nmaps].data.configs.num_configs = nconfigs;
		nmaps++;
	} else {
		kfree(configs);
	}

	*maps = map;
	*num_maps = nmaps;
	kfree(pinmux);

	return 0;

remove_group:
	pinctrl_generic_remove_group(pctldev, gsel);
unlock:
	mutex_unlock(&tp->mutex);
	kfree(configs);
out:
	kfree(pinmux);
	kfree(map);
	return ret;
}

static const struct pinctrl_ops tda54_pinctrl_ops = {
	.get_groups_count	= pinctrl_generic_get_group_count,
	.get_group_name		= pinctrl_generic_get_group_name,
	.get_group_pins		= pinctrl_generic_get_group_pins,
	.pin_dbg_show		= tda54_pin_dbg_show,
	.dt_node_to_map		= tda54_dt_node_to_map,
	.dt_free_map		= pinctrl_utils_free_map,
};

static int tda54_set_mux(struct pinctrl_dev *pctldev, unsigned int fsel,
			 unsigned int gsel)
{
	struct tda54_pinctrl *tp = pinctrl_dev_get_drvdata(pctldev);
	const struct tda54_group_data *gdata;
	const struct group_desc *grp;
	unsigned int i;

	grp = pinctrl_generic_get_group(pctldev, gsel);
	if (!grp)
		return -EINVAL;

	gdata = grp->data;
	if (!gdata)
		return -EINVAL;

	for (i = 0; i < grp->grp.npins; i++)
		tda54_rmw(tp, tda54_padcfga(grp->grp.pins[i]),
			  TDA54_MUXMODE, gdata->mux[i]);

	return 0;
}

static int tda54_gpio_request_enable(struct pinctrl_dev *pctldev,
				     struct pinctrl_gpio_range *range,
				     unsigned int offset)
{
	struct tda54_pinctrl *tp = pinctrl_dev_get_drvdata(pctldev);

	if (offset >= tp->npins)
		return -EINVAL;

	tda54_rmw(tp, tda54_padcfga(offset), TDA54_MUXMODE, TDA54_MUX_GPIO);

	return 0;
}

static const struct pinmux_ops tda54_pinmux_ops = {
	.get_functions_count	= pinmux_generic_get_function_count,
	.get_function_name	= pinmux_generic_get_function_name,
	.get_function_groups	= pinmux_generic_get_function_groups,
	.set_mux		= tda54_set_mux,
	.gpio_request_enable	= tda54_gpio_request_enable,
};

static int tda54_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
			     unsigned long *config)
{
	struct tda54_pinctrl *tp = pinctrl_dev_get_drvdata(pctldev);
	unsigned int param = pinconf_to_config_param(*config);
	u32 val_a = readl_relaxed(tp->base + tda54_padcfga(pin));
	u32 val_b = readl_relaxed(tp->base + tda54_padcfgb(pin));
	bool enabled = true;
	u32 arg = 0;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		enabled = val_a & TDA54_PULLUDEN;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		enabled = !(val_a & TDA54_PULLUDEN) && !(val_a & TDA54_PULLTYPESEL);
		arg = enabled;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		enabled = !(val_a & TDA54_PULLUDEN) && (val_a & TDA54_PULLTYPESEL);
		arg = enabled;
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		enabled = val_a & TDA54_RXACTIVE;
		arg = enabled;
		break;
	case PIN_CONFIG_OUTPUT_ENABLE:
		enabled = !(val_a & TDA54_TX_DIS);
		arg = enabled;
		break;
	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		enabled = !(val_a & TDA54_HYST_DIS);
		arg = enabled;
		break;
	case PIN_CONFIG_SLEW_RATE:
		arg = FIELD_GET(TDA54_SLEW_CTRL, val_a);
		break;
	case TDA54_PIN_CONFIG_DEBOUNCE_SELECT:
		arg = FIELD_GET(TDA54_DEBOUNCE_SEL, val_a);
		break;
	case TDA54_PIN_CONFIG_VIRT_GPIO_INSTANCE:
		arg = FIELD_GET(TDA54_VIRT_GPIO, val_a);
		break;
	case TDA54_PIN_CONFIG_WKUP:
		if (!(val_b & TDA54_WKUP_EN))
			arg = 0;
		else if (!(val_b & TDA54_WKUP_LVL_EN))
			arg = 1;
		else if (!(val_b & TDA54_WKUP_LVL_POL))
			arg = 2;
		else
			arg = 3;
		break;
	case TDA54_PIN_CONFIG_RET_BIAS:
		if (val_b & TDA54_RET_PULLUD_EN)
			arg = 0;
		else if (val_b & TDA54_RET_PULLTYPE_SEL)
			arg = 2;
		else
			arg = 1;
		break;
	case TDA54_PIN_CONFIG_RET_OFFMODE:
		arg = (val_b & TDA54_RET_OFFMODE_EN) ? 1 : 0;
		break;
	case TDA54_PIN_CONFIG_RET_OUT:
		if (val_b & TDA54_RET_OUT_DIS)
			arg = 0;
		else if (val_b & TDA54_RET_OUT_LOW)
			arg = 1;
		else
			arg = 2;
		break;
	case TDA54_PIN_CONFIG_FORCE_RET:
		arg = (val_b & TDA54_FORCE_RET_EN) ? 1 : 0;
		break;
	case TDA54_PIN_CONFIG_ISO_BYPASS:
		arg = (val_b & TDA54_ISO_BYP) ? 1 : 0;
		break;
	default:
		return -EINVAL;
	}

	*config = pinconf_to_config_packed(param, arg);

	return enabled ? 0 : -EINVAL;
}

static int tda54_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
			     unsigned long *configs, unsigned int num_configs)
{
	struct tda54_pinctrl *tp = pinctrl_dev_get_drvdata(pctldev);
	u32 mask_a = 0, val_a = 0;
	u32 mask_b = 0, val_b = 0;
	unsigned int i;

	for (i = 0; i < num_configs; i++) {
		unsigned int param = pinconf_to_config_param(configs[i]);
		u32 arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			mask_a |= TDA54_BIAS;
			val_a &= ~TDA54_BIAS;
			val_a |= TDA54_PULLUDEN;
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			mask_a |= TDA54_BIAS;
			val_a &= ~TDA54_BIAS;
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			mask_a |= TDA54_BIAS;
			val_a &= ~TDA54_BIAS;
			val_a |= TDA54_PULLTYPESEL;
			break;
		case PIN_CONFIG_INPUT_ENABLE:
			mask_a |= TDA54_RXACTIVE;
			if (arg)
				val_a |= TDA54_RXACTIVE;
			else
				val_a &= ~TDA54_RXACTIVE;
			break;
		case PIN_CONFIG_OUTPUT_ENABLE:
			mask_a |= TDA54_TX_DIS;
			if (arg)
				val_a &= ~TDA54_TX_DIS;
			else
				val_a |= TDA54_TX_DIS;
			break;
		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			mask_a |= TDA54_HYST_DIS;
			if (arg)
				val_a &= ~TDA54_HYST_DIS;
			else
				val_a |= TDA54_HYST_DIS;
			break;
		case PIN_CONFIG_SLEW_RATE:
			if (arg > FIELD_MAX(TDA54_SLEW_CTRL))
				return -EINVAL;
			mask_a |= TDA54_SLEW_CTRL;
			val_a |= FIELD_PREP(TDA54_SLEW_CTRL, arg);
			break;
		case TDA54_PIN_CONFIG_DEBOUNCE_SELECT:
			if (arg > TDA54_DEBOUNCE_SEL_MAX)
				return -EINVAL;
			mask_a |= TDA54_DEBOUNCE_SEL;
			val_a |= FIELD_PREP(TDA54_DEBOUNCE_SEL, arg);
			break;
		case TDA54_PIN_CONFIG_VIRT_GPIO_INSTANCE:
			if (arg > FIELD_MAX(TDA54_VIRT_GPIO))
				return -EINVAL;
			mask_a |= TDA54_VIRT_GPIO;
			val_a |= FIELD_PREP(TDA54_VIRT_GPIO, arg);
			break;
		case TDA54_PIN_CONFIG_WKUP:
			mask_b |= TDA54_WKUP_EN | TDA54_WKUP_LVL_EN |
				  TDA54_WKUP_LVL_POL;
			switch (arg) {
			case 0:
				break;
			case 1:
				val_b |= TDA54_WKUP_EN;
				break;
			case 2:
				val_b |= TDA54_WKUP_EN | TDA54_WKUP_LVL_EN;
				break;
			case 3:
				val_b |= TDA54_WKUP_EN | TDA54_WKUP_LVL_EN |
					 TDA54_WKUP_LVL_POL;
				break;
			default:
				return -EINVAL;
			}
			break;
		case TDA54_PIN_CONFIG_RET_BIAS:
			mask_b |= TDA54_RET_BIAS;
			switch (arg) {
			case 0:
				val_b |= TDA54_RET_PULLUD_EN;
				break;
			case 1:
				break;
			case 2:
				val_b |= TDA54_RET_PULLTYPE_SEL;
				break;
			default:
				return -EINVAL;
			}
			break;
		case TDA54_PIN_CONFIG_RET_OFFMODE:
			if (arg > 1)
				return -EINVAL;
			mask_b |= TDA54_RET_OFFMODE_EN;
			if (arg)
				val_b |= TDA54_RET_OFFMODE_EN;
			break;
		case TDA54_PIN_CONFIG_RET_OUT:
			mask_b |= TDA54_RET_OUT_DIS | TDA54_RET_OUT_LOW;
			switch (arg) {
			case 0:
				val_b |= TDA54_RET_OUT_DIS;
				break;
			case 1:
				val_b |= TDA54_RET_OUT_LOW;
				break;
			case 2:
				break;
			default:
				return -EINVAL;
			}
			break;
		case TDA54_PIN_CONFIG_FORCE_RET:
			if (arg > 1)
				return -EINVAL;
			mask_b |= TDA54_FORCE_RET_EN;
			if (arg)
				val_b |= TDA54_FORCE_RET_EN;
			break;
		case TDA54_PIN_CONFIG_ISO_BYPASS:
			if (arg > 1)
				return -EINVAL;
			mask_b |= TDA54_ISO_BYP;
			if (arg)
				val_b |= TDA54_ISO_BYP;
			break;
		default:
			return -EINVAL;
		}
	}

	if (mask_a)
		tda54_rmw(tp, tda54_padcfga(pin), mask_a, val_a);
	if (mask_b)
		tda54_rmw(tp, tda54_padcfgb(pin), mask_b, val_b);

	return 0;
}

static int tda54_pinconf_group_get(struct pinctrl_dev *pctldev,
				   unsigned int gsel, unsigned long *config)
{
	unsigned long prev_config = 0;
	const unsigned int *pins;
	unsigned int i, npins;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, gsel, &pins, &npins);
	if (ret)
		return ret;

	for (i = 0; i < npins; i++) {
		ret = tda54_pinconf_get(pctldev, pins[i], config);
		if (ret)
			return ret;

		/* configs do not match between pins */
		if (i && prev_config != *config)
			return -EINVAL;

		prev_config = *config;
	}

	return 0;
}

static int tda54_pinconf_group_set(struct pinctrl_dev *pctldev,
				   unsigned int gsel, unsigned long *configs,
				   unsigned int num_configs)
{
	const unsigned int *pins;
	unsigned int i, npins;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, gsel, &pins, &npins);
	if (ret)
		return ret;

	for (i = 0; i < npins; i++) {
		ret = tda54_pinconf_set(pctldev, pins[i], configs, num_configs);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct pinconf_ops tda54_pinconf_ops = {
	.is_generic		= true,
	.pin_config_get		= tda54_pinconf_get,
	.pin_config_set		= tda54_pinconf_set,
	.pin_config_group_get	= tda54_pinconf_group_get,
	.pin_config_group_set	= tda54_pinconf_group_set,
};

static int tda54_build_pins(struct tda54_pinctrl *tp)
{
	struct pinctrl_pin_desc *pins;
	unsigned int i;

	pins = devm_kcalloc(tp->dev, tp->npins, sizeof(*pins), GFP_KERNEL);
	if (!pins)
		return -ENOMEM;

	for (i = 0; i < tp->npins; i++) {
		pins[i].number = i;
		pins[i].name = devm_kasprintf(tp->dev, GFP_KERNEL, "padcfg%u",
					      i);
		if (!pins[i].name)
			return -ENOMEM;
	}

	tp->desc.pins = pins;
	tp->desc.npins = tp->npins;

	return 0;
}

static int tda54_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tda54_pinctrl *tp;
	void __iomem *base;
	int ret;

	tp = devm_kzalloc(dev, sizeof(*tp), GFP_KERNEL);
	if (!tp)
		return -ENOMEM;

	tp->dev = dev;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	of_property_read_u32(dev->of_node, "ti,proxy-id", &tp->proxy);
	tp->base = base + tp->proxy * TDA54_PROXY_STRIDE;

	ret = of_property_read_u32(dev->of_node, "ti,npins", &tp->npins);
	if (ret)
		return dev_err_probe(dev, ret, "ti,npins property not found\n");

	tda54_unlock(tp);

	ret = tda54_build_pins(tp);
	if (ret)
		return ret;

	tp->desc.name = dev_name(dev);
	tp->desc.pctlops = &tda54_pinctrl_ops;
	tp->desc.pmxops = &tda54_pinmux_ops;
	tp->desc.confops = &tda54_pinconf_ops;
	tp->desc.owner = THIS_MODULE;
	tp->desc.num_custom_params = ARRAY_SIZE(tda54_custom_bindings);
	tp->desc.custom_params = tda54_custom_bindings;
#ifdef CONFIG_DEBUG_FS
	tp->desc.custom_conf_items = tda54_conf_items;
#endif

	ret = devm_mutex_init(dev, &tp->mutex);
	if (ret)
		return ret;

	raw_spin_lock_init(&tp->lock);

	ret = devm_pinctrl_register_and_init(dev, &tp->desc, tp, &tp->pctl);
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not register pinctrl driver\n");

	return pinctrl_enable(tp->pctl);
}

static const struct of_device_id tda54_pinctrl_of_match[] = {
	{ .compatible = "ti,tda54-pinctrl" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tda54_pinctrl_of_match);

static struct platform_driver tda54_pinctrl_driver = {
	.probe = tda54_pinctrl_probe,
	.driver = {
		.name = "pinctrl-tda54",
		.of_match_table = tda54_pinctrl_of_match,
	},
};
module_platform_driver(tda54_pinctrl_driver);

MODULE_AUTHOR("Yemike Abhilash Chandra <y-abhilashchandra@ti.com>");
MODULE_DESCRIPTION("Pinctrl driver for the TI TDA54 SoC");
MODULE_LICENSE("GPL");
