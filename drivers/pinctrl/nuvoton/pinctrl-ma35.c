// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Nuvoton Technology Corp.
 *
 * Author: Shan-Chun Hung <schung@nuvoton.com>
 */

#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/regmap.h>
#include <linux/gpio/driver.h>

#include "../core.h"
#include "../pinconf.h"
#include "pinctrl-ma35.h"

#define MA35_MFP_REG_BASE		0x80
#define MA35_MFP_REG_OFFSET_PER_GROUP	8
#define MA35_MFP_BITS_PER_PORT		4

#define MA35_GPIO_GROUP_MAX		14
#define MA35_GPIO_GROUP_PORT_MAX	16

/* GPIO control registers */
#define MA35_GP_REG_MODE		0x00
#define MA35_GP_REG_DINOFF		0x04
#define MA35_GP_REG_DOUT		0x08
#define MA35_GP_REG_DATMSK		0x0c
#define MA35_GP_REG_PIN			0x10
#define MA35_GP_REG_DBEN		0x14
#define MA35_GP_REG_INTTYPE		0x18
#define MA35_GP_REG_INTEN		0x1c
#define MA35_GP_REG_INTSRC		0x20
#define MA35_GP_REG_SMTEN		0x24
#define MA35_GP_REG_SLEWCTL		0x28
#define MA35_GP_REG_SPW			0x2c
#define MA35_GP_REG_PUSEL		0x30
#define MA35_GP_REG_DSL			0x38
#define MA35_GP_REG_DSH			0x3c

/* GPIO mode control */
#define MA35_GP_MODE_INPUT		0x0
#define MA35_GP_MODE_OUTPUT		0x1
#define MA35_GP_MODE_OPEN_DRAIN		0x2
#define MA35_GP_MODE_QUASI		0x3

/* GPIO pull-up and pull-down selection control */
#define MA35_GP_PUSEL_DISABLE		0x0
#define MA35_GP_PUSEL_PULL_UP		0x1
#define MA35_GP_PUSEL_PULL_DOWN		0x2

/* Each pin data input/output is mapped by address mapping */
#define MA35_PIN_MAP_BASE		0x800

#define MA35_GP_DSH_BASE_PORT		8

#define VOLT_1_8			1800
#define VOLT_3_3			3300

char *gpio_group_name[] = {
	"gpioa", "gpiob", "gpioc", "gpiod", "gpioe", "gpiof", "gpiog",
	"gpioh", "gpioi", "gpioj", "gpiok", "gpiol", "gpiom", "gpion",
};

struct ma35_pin_func {
	const char		*name;
	const char		**groups;
	u32			ngroups;
};

struct ma35_pin_setting {
	u32			offset;
	u32			shift;
	u32			muxval;
	unsigned long		*configs;
	unsigned int		nconfigs;
};

struct ma35_pin_group {
	const char		*name;
	unsigned int		npins;
	unsigned int		*pins;
	struct ma35_pin_setting	*settings;
};

struct ma35_pin_bank {
	void __iomem		*reg_base;
	struct clk		*clk;
	int			irq;
	u8			nr_pins;
	char			*name;
	u8			bank_num;
	bool			valid;
	struct device_node	*of_node;
	struct gpio_chip	chip;
	struct irq_chip		irqc;
	u32			irqtype;
	u32			irqinten;
	struct regmap		*regmap;
	struct device		*dev;
	spinlock_t		lock;
};

struct ma35_pin_ctrl {
	struct ma35_pin_bank	*pin_banks;
	u32			nr_banks;
	u32			nr_pins;
};

struct ma35_pinctrl {
	struct device		*dev;
	struct ma35_pin_ctrl	*ctrl;
	struct pinctrl_dev	*pctl;
	const struct ma35_pinctrl_soc_info *info;
	struct regmap		*regmap;
	struct ma35_pin_group	*groups;
	unsigned int		ngroups;
	struct ma35_pin_func	*functions;
	unsigned int		nfunctions;
};

static int ma35_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	return npctl->ngroups;
}

static const char *ma35_get_group_name(struct pinctrl_dev *pctldev,
				       unsigned int selector)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	return npctl->groups[selector].name;
}

static int ma35_get_group_pins(struct pinctrl_dev *pctldev, unsigned int selector,
			       const unsigned int **pins, unsigned int *npins)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	if (selector >= npctl->ngroups)
		return -EINVAL;

	*pins = npctl->groups[selector].pins;
	*npins = npctl->groups[selector].npins;

	return 0;
}

static struct ma35_pin_group *ma35_pinctrl_find_group_by_name(
			      const struct ma35_pinctrl *npctl, const char *name)
{
	int i;

	for (i = 0; i < npctl->ngroups; i++) {
		if (!strcmp(npctl->groups[i].name, name))
			return &npctl->groups[i];
	}

	return NULL;
}

static int ma35_pinctrl_dt_node_to_map_func(struct pinctrl_dev *pctldev,
					    struct device_node *np,
					    struct pinctrl_map **map,
					    unsigned int *num_maps)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);
	struct ma35_pin_group *grp;
	struct pinctrl_map *new_map;
	struct device_node *parent;
	int map_num = 1;
	int i;

	/*
	 * first find the group of this node and check if we need create
	 * config maps for pins
	 */
	grp = ma35_pinctrl_find_group_by_name(npctl, np->name);
	if (!grp) {
		dev_err(npctl->dev, "unable to find group for node %s\n", np->name);
		return -EINVAL;
	}

	map_num += grp->npins;
	new_map = devm_kzalloc(pctldev->dev, sizeof(*new_map) * map_num, GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	*map = new_map;
	*num_maps = map_num;
	/* create mux map */
	parent = of_get_parent(np);
	if (!parent) {
		devm_kfree(pctldev->dev, new_map);
		return -EINVAL;
	}

	new_map[0].type = PIN_MAP_TYPE_MUX_GROUP;
	new_map[0].data.mux.function = parent->name;
	new_map[0].data.mux.group = np->name;
	of_node_put(parent);

	new_map++;
	for (i = 0; i < grp->npins; i++) {
		new_map[i].type = PIN_MAP_TYPE_CONFIGS_PIN;
		new_map[i].data.configs.group_or_pin = pin_get_name(pctldev, grp->pins[i]);
		new_map[i].data.configs.configs = grp->settings[i].configs;
		new_map[i].data.configs.num_configs = grp->settings[i].nconfigs;
	}
	dev_dbg(pctldev->dev, "maps: function %s group %s num %d\n",
		(*map)->data.mux.function, (*map)->data.mux.group, map_num);

	return 0;
}

static void ma35_dt_free_map(struct pinctrl_dev *pctldev, struct pinctrl_map *map,
			     unsigned int num_maps)
{
	devm_kfree(pctldev->dev, map);
}

static const struct pinctrl_ops ma35_pctrl_ops = {
	.get_groups_count = ma35_get_groups_count,
	.get_group_name = ma35_get_group_name,
	.get_group_pins = ma35_get_group_pins,
	.dt_node_to_map = ma35_pinctrl_dt_node_to_map_func,
	.dt_free_map = ma35_dt_free_map,
};

static int ma35_pinmux_get_func_count(struct pinctrl_dev *pctldev)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	return npctl->nfunctions;
}

static const char *ma35_pinmux_get_func_name(struct pinctrl_dev *pctldev,
					     unsigned int selector)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	return npctl->functions[selector].name;
}

static int ma35_pinmux_get_func_groups(struct pinctrl_dev *pctldev,
				       unsigned int function,
				       const char *const **groups,
				       unsigned int *const num_groups)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);

	*groups = npctl->functions[function].groups;
	*num_groups = npctl->functions[function].ngroups;

	return 0;
}

static int ma35_pinmux_set_mux(struct pinctrl_dev *pctldev, unsigned int selector,
			       unsigned int group)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);
	struct ma35_pin_group *grp = &npctl->groups[group];
	struct ma35_pin_setting *setting = grp->settings;
	u32 i, regval;

	dev_dbg(npctl->dev, "enable function %s group %s\n",
		npctl->functions[selector].name, npctl->groups[group].name);

	for (i = 0; i < grp->npins; i++) {
		regmap_read(npctl->regmap, setting->offset, &regval);
		regval &= ~GENMASK(setting->shift + 3, setting->shift);
		regval |= setting->muxval << setting->shift;

		regmap_write(npctl->regmap, setting->offset, regval);
		setting++;
	}
	return 0;
}

const struct pinmux_ops ma35_pmx_ops = {
	.get_functions_count = ma35_pinmux_get_func_count,
	.get_function_name = ma35_pinmux_get_func_name,
	.get_function_groups = ma35_pinmux_get_func_groups,
	.set_mux = ma35_pinmux_set_mux,
	.strict = true,
};

static int ma35_gpio_core_direction_in(struct gpio_chip *gc, unsigned int gpio)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(gc);
	void __iomem *reg_mode = bank->reg_base + MA35_GP_REG_MODE;
	unsigned long flags;
	unsigned int regval;

	spin_lock_irqsave(&bank->lock, flags);

	regval = readl(reg_mode);

	regval &= ~GENMASK(gpio * 2 + 1, gpio * 2);
	regval |= MA35_GP_MODE_INPUT << gpio * 2;

	writel(regval, reg_mode);

	spin_unlock_irqrestore(&bank->lock, flags);

	return 0;
}

static int ma35_gpio_core_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(gc);

	return readl(bank->reg_base + MA35_PIN_MAP_BASE + gpio * 4);
}

static void ma35_gpio_core_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(gc);

	writel(val, bank->reg_base + MA35_PIN_MAP_BASE + gpio * 4);
}

static int ma35_gpio_core_direction_out(struct gpio_chip *gc, unsigned int gpio,
					int val)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(gc);
	void __iomem *reg_dout = bank->reg_base + MA35_GP_REG_DOUT;
	void __iomem *reg_mode = bank->reg_base + MA35_GP_REG_MODE;
	unsigned long flags;
	unsigned int regval;

	spin_lock_irqsave(&bank->lock, flags);

	regval = readl(reg_dout);
	if (val)
		writel(regval | BIT(gpio), reg_dout);
	else
		writel(regval & ~BIT(gpio), reg_dout);

	regval = readl(reg_mode);

	regval &= ~GENMASK(gpio * 2 + 1, gpio * 2);
	regval |= MA35_GP_MODE_OUTPUT << gpio * 2;

	writel(regval, reg_mode);

	spin_unlock_irqrestore(&bank->lock, flags);

	return 0;
}

static int ma35_gpio_core_to_request(struct gpio_chip *gc, unsigned int gpio)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(gc);
	u32 reg_offs, bit_offs, regval;

	if (gpio < 8) {
		/* The MFP low register controls port 0 ~ 7 */
		reg_offs = bank->bank_num * MA35_MFP_REG_OFFSET_PER_GROUP;
		bit_offs = gpio * MA35_MFP_BITS_PER_PORT;
	} else {
		/* The MFP high register controls port 8 ~ 15 */
		reg_offs = bank->bank_num * MA35_MFP_REG_OFFSET_PER_GROUP + 4;
		bit_offs = (gpio - 8) * MA35_MFP_BITS_PER_PORT;
	}

	regmap_read(bank->regmap, MA35_MFP_REG_BASE + reg_offs, &regval);

	regval &= ~GENMASK(bit_offs + MA35_MFP_BITS_PER_PORT - 1, bit_offs);

	regmap_write(bank->regmap, MA35_MFP_REG_BASE + reg_offs, regval);

	return 0;
}

static void ma35_irq_gpio_mask(struct irq_data *d)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	void __iomem *reg_ien = bank->reg_base + MA35_GP_REG_INTEN;
	unsigned int num = (d->hwirq);
	u32 regval;

	regval = readl(reg_ien);

	/*
	 * The MA35_GP_REG_INTEN bits 0 ~ 15 control low-level or falling edge trigger,
	 * while bits 16 ~ 31 control high-level or rising edge trigger.
	 * We disable both type of interrupt.
	 */
	regval &= ~(BIT(num + 16) | BIT(num));

	writel(regval, reg_ien);
}

static void ma35_irq_gpio_unmask(struct irq_data *d)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	void __iomem *reg_itype = bank->reg_base + MA35_GP_REG_INTTYPE;
	void __iomem *reg_ien = bank->reg_base + MA35_GP_REG_INTEN;
	unsigned int num = (d->hwirq);
	u32 bval, regval;

	bval = bank->irqtype & BIT(num);

	regval = readl(reg_itype);
	regval &= ~BIT(num);
	writel(regval | bval, reg_itype);

	bval = bank->irqinten & (BIT(num + 16) | BIT(num));

	regval = readl(reg_ien);
	regval &= ~(BIT(num + 16) | BIT(num));
	writel(regval | bval, reg_ien);
}

static int ma35_irq_irqtype(struct irq_data *d, unsigned int type)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	void __iomem *reg_itype = bank->reg_base + MA35_GP_REG_INTTYPE;
	void __iomem *reg_ien = bank->reg_base + MA35_GP_REG_INTEN;
	unsigned int num = (d->hwirq);

	if (type == IRQ_TYPE_PROBE) {
		writel(readl(reg_itype) & ~BIT(num), reg_itype);
		writel(readl(reg_ien) | BIT(num) | BIT(num + 16), reg_ien);
		bank->irqtype &= ~BIT(num);
		bank->irqinten |= BIT(num) | BIT(num + 16);
		return 0;
	}

	if (type & IRQ_TYPE_LEVEL_MASK) {
		writel(readl(reg_itype) | BIT(num), reg_itype);
		writel(readl(reg_ien) & ~(BIT(num) | BIT(num + 16)), reg_ien);
		bank->irqtype |= BIT(num);
		bank->irqinten &= ~(BIT(num) | BIT(num + 16));
		if (type == IRQ_TYPE_LEVEL_HIGH) {
			writel(readl(reg_ien) | BIT(num + 16), reg_ien);
			bank->irqinten |= BIT(num + 16);
			return 0;
		}

		if (type == IRQ_TYPE_LEVEL_LOW) {
			writel(readl(reg_ien) | BIT(num), reg_ien);
			bank->irqinten |= BIT(num);
			return 0;
		}

	} else {
		writel(readl(reg_itype) & ~BIT(num), reg_itype);
		bank->irqtype &= ~BIT(num);

		if (type & IRQ_TYPE_EDGE_RISING) {
			writel(readl(reg_ien) | BIT(num + 16), reg_ien);
			bank->irqinten |= BIT(num + 16);

		} else {
			writel(readl(reg_ien) & ~BIT(num + 16), reg_ien);
			bank->irqinten &= ~BIT(num + 16);
		}

		if (type & IRQ_TYPE_EDGE_FALLING) {
			writel(readl(reg_ien) | BIT(num), reg_ien);
			bank->irqinten |= BIT(num);

		} else {
			writel(readl(reg_ien) & ~BIT(num), reg_ien);
			bank->irqinten &= ~BIT(num);
		}
	}
	return 0;
}

static void ma35_irq_demux_intgroup(struct irq_desc *desc)
{
	struct ma35_pin_bank *bank = gpiochip_get_data(irq_desc_get_handler_data(desc));
	struct irq_domain *irqdomain = bank->chip.irq.domain;
	struct irq_chip *irqchip = irq_desc_get_chip(desc);
	unsigned int j, isr;

	chained_irq_enter(irqchip, desc);

	isr = readl(bank->reg_base + MA35_GP_REG_INTSRC);

	if (isr != 0) {
		writel(isr, bank->reg_base + MA35_GP_REG_INTSRC);

		for (j = 0; j < 16; j++) {
			if (isr & 0x1)
				generic_handle_irq(irq_find_mapping(irqdomain, j));
			isr = isr >> 1;
		}
	}

	chained_irq_exit(irqchip, desc);
}

static int ma35_gpiolib_register(struct platform_device *pdev,
				 struct ma35_pinctrl *npctl)
{
	struct ma35_pin_ctrl *ctrl = npctl->ctrl;
	struct ma35_pin_bank *bank = ctrl->pin_banks;
	int ret;
	int i;

	for (i = 0; i < ctrl->nr_banks; ++i, ++bank) {
		if (!bank->valid) {
			dev_warn(&pdev->dev, "bank %s is not valid\n",
				 bank->of_node->name);
			continue;
		}
		bank->irqtype = 0;
		bank->irqinten = 0;
		bank->chip.label = bank->name;
		bank->chip.of_gpio_n_cells = 2;
		bank->chip.parent = &pdev->dev;
		bank->chip.request = ma35_gpio_core_to_request;
		bank->chip.direction_input = ma35_gpio_core_direction_in;
		bank->chip.direction_output = ma35_gpio_core_direction_out;
		bank->chip.get = ma35_gpio_core_get;
		bank->chip.set = ma35_gpio_core_set;
		bank->chip.base = -1;
		bank->chip.ngpio = bank->nr_pins;
		bank->chip.can_sleep = false;
		spin_lock_init(&bank->lock);

		if (bank->irq > 0) {
			struct gpio_irq_chip *girq;

			girq = &bank->chip.irq;
			girq->chip = &bank->irqc;
			girq->chip->name = bank->name;
			girq->chip->irq_disable = ma35_irq_gpio_mask;
			girq->chip->irq_enable = ma35_irq_gpio_unmask;
			girq->chip->irq_set_type = ma35_irq_irqtype;
			girq->chip->irq_mask = ma35_irq_gpio_mask;
			girq->chip->irq_unmask = ma35_irq_gpio_unmask;
			girq->chip->flags = IRQCHIP_MASK_ON_SUSPEND |
			IRQCHIP_SKIP_SET_WAKE | IRQCHIP_IMMUTABLE;
			girq->parent_handler = ma35_irq_demux_intgroup;
			girq->num_parents = 1;

			girq->parents = devm_kcalloc(&pdev->dev, 1,
						     sizeof(*girq->parents),
						     GFP_KERNEL);
			if (!girq->parents)
				return -ENOMEM;

			girq->parents[0] = bank->irq;
			girq->default_type = IRQ_TYPE_NONE;
			girq->handler = handle_level_irq;
		}

		ret = gpiochip_add_data(&bank->chip, bank);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to register gpio_chip %s, error code: %d\n",
				bank->chip.label, ret);
			goto fail;
		}
	}
	return 0;
fail:
	for (--i, --bank; i >= 0; --i, --bank) {
		if (!bank->valid)
			continue;
		gpiochip_remove(&bank->chip);
	}
	return ret;
}

static int ma35_get_bank_data(struct ma35_pin_bank *bank, struct ma35_pinctrl *npctl)
{
	struct resource res;

	if (of_address_to_resource(bank->of_node, 0, &res)) {
		dev_err(npctl->dev, "cannot find IO resource for bank\n");
		return -ENOENT;
	}

	bank->reg_base = devm_ioremap_resource(npctl->dev, &res);
	if (IS_ERR(bank->reg_base)) {
		dev_err(npctl->dev, "cannot ioremap resource for bank\n");
		return PTR_ERR(bank->reg_base);
	}

	bank->irq = irq_of_parse_and_map(bank->of_node, 0);
	bank->nr_pins = MA35_GPIO_GROUP_PORT_MAX;

	bank->clk = of_clk_get(bank->of_node, 0);
	if (IS_ERR(bank->clk))
		return PTR_ERR(bank->clk);

	return clk_prepare_enable(bank->clk);
}

static int ma35_pinctrl_get_soc_data(struct ma35_pinctrl *pctl,
				     struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct device_node *np;
	struct ma35_pin_ctrl *ctrl;
	struct ma35_pin_bank *bank;
	int i;

	ctrl = pctl->ctrl;
	ctrl->nr_banks = MA35_GPIO_GROUP_MAX;
	ctrl->pin_banks = devm_kcalloc(&pdev->dev, ctrl->nr_banks,
				       sizeof(*ctrl->pin_banks), GFP_KERNEL);
	if (!ctrl->pin_banks)
		return -ENOMEM;

	for (i = 0; i < ctrl->nr_banks; i++) {
		ctrl->pin_banks[i].bank_num = i;
		ctrl->pin_banks[i].name = gpio_group_name[i];
	}

	for_each_child_of_node(node, np) {
		if (!of_find_property(np, "gpio-controller", NULL))
			continue;

		bank = ctrl->pin_banks;
		for (i = 0; i < ctrl->nr_banks; ++i, ++bank) {
			if (!strcmp(bank->name, np->name)) {
				bank->of_node = np;
				bank->regmap = pctl->regmap;
				bank->dev = &pdev->dev;
				if (!ma35_get_bank_data(bank, pctl))
					bank->valid = true;
				break;
			}
		}
	}
	return 0;
}

static void ma35_gpio_cla_port(unsigned int gpio_num, unsigned int *group,
			       unsigned int *num)
{
	*group = gpio_num / MA35_GPIO_GROUP_PORT_MAX;
	*num = gpio_num % MA35_GPIO_GROUP_PORT_MAX;
}

static int ma35_pinconf_set_pull(struct ma35_pinctrl *npctl, unsigned int pin,
				 int pull_up)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_PUSEL);

	regval &= ~GENMASK(port * 2 + 1, port * 2);

	switch (pull_up) {
	case PIN_CONFIG_BIAS_PULL_UP:
		regval |= MA35_GP_PUSEL_PULL_UP << port * 2;
		break;

	case PIN_CONFIG_BIAS_PULL_DOWN:
		regval |= MA35_GP_PUSEL_PULL_DOWN << port * 2;
		break;

	case MA35_GP_PUSEL_DISABLE:
		regval |= MA35_GP_PUSEL_DISABLE << port * 2;
		break;

	default:
		regval |= MA35_GP_PUSEL_DISABLE << port * 2;
		break;
	}

	writel(regval, base + MA35_GP_REG_PUSEL);

	return 0;
}

static int ma35_pinconf_get_output(struct ma35_pinctrl *npctl, unsigned int pin)
{
	int port, group_num;
	void __iomem *base;
	u32 regval, mode;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_MODE);
	mode = (regval & GENMASK(port * 2 + 1, port * 2)) >> port * 2;

	if (mode == MA35_GP_MODE_OUTPUT)
		return 1;

	return 0;
}

static int ma35_pinconf_get_pull(struct ma35_pinctrl *npctl, unsigned int pin)
{
	int port, group_num;
	void __iomem *base;
	u32 regval, mode;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_PUSEL);
	mode = (regval & GENMASK(port * 2 + 1, port * 2)) >> port * 2;

	switch (mode) {
	case MA35_GP_PUSEL_PULL_UP:
		return PIN_CONFIG_BIAS_PULL_UP;

	case MA35_GP_PUSEL_PULL_DOWN:
		return PIN_CONFIG_BIAS_PULL_DOWN;

	case MA35_GP_PUSEL_DISABLE:
		return PIN_CONFIG_BIAS_DISABLE;
	}

	return PIN_CONFIG_BIAS_DISABLE;
}

static int ma35_pinconf_set_output(struct ma35_pinctrl *npctl, unsigned int pin,
				   bool out)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_MODE);

	regval &= ~GENMASK(port * 2 + 1, port * 2);
	regval |= MA35_GP_MODE_OUTPUT << port * 2;

	writel(regval, base + MA35_GP_REG_MODE);

	return 0;
}

static int ma35_pinconf_get_drive_strength(struct ma35_pinctrl *npctl,
					   unsigned int pin, u16 *strength)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	if (port < MA35_GP_DSH_BASE_PORT) {
		regval = readl(base + MA35_GP_REG_DSL);
		*strength = (regval & GENMASK(port * 4 + 3, port * 4)) >> port * 4;
	} else {
		port -= MA35_GP_DSH_BASE_PORT;
		regval = readl(base + MA35_GP_REG_DSH);
		*strength = (regval & GENMASK(port * 4 + 3, port * 4)) >> port * 4;
	}
	return 0;
}

static int ma35_pinconf_set_drive_strength(struct ma35_pinctrl *npctl,
					   unsigned int pin, int strength)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	if (port < MA35_GP_DSH_BASE_PORT) {
		regval = readl(base + MA35_GP_REG_DSL);
		regval &= ~GENMASK(port * 4 + 3, port * 4);
		regval |= strength << port * 4;
		writel(regval, base + MA35_GP_REG_DSL);
	} else {
		port -= MA35_GP_DSH_BASE_PORT;
		regval = readl(base + MA35_GP_REG_DSH);
		regval &= ~GENMASK(port * 4 + 3, port * 4);
		regval |= strength << port * 4;
		writel(regval, base + MA35_GP_REG_DSH);
	}

	return 0;
}

static int ma35_pinconf_get_schmitt_enable(struct ma35_pinctrl *npctl,
					   unsigned int pin)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_SMTEN);

	return !!(regval & BIT(port));
}

static int ma35_pinconf_set_schmitt(struct ma35_pinctrl *npctl, unsigned int pin,
				    int enable)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_SMTEN);

	if (enable)
		regval |= BIT(port);
	else
		regval &= ~BIT(port);

	writel(regval, base + MA35_GP_REG_SMTEN);

	return 0;
}

static int ma35_pinconf_get_slew_rate(struct ma35_pinctrl *npctl,
				      unsigned int pin)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_SLEWCTL);

	return (regval & GENMASK(port * 2 + 1, port * 2)) >> port * 2;
}

static int ma35_pinconf_set_slew_rate(struct ma35_pinctrl *npctl,
				      unsigned int pin, int rate)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_SLEWCTL);

	regval &= ~GENMASK(port * 2 + 1, port * 2);
	regval |= rate << port * 2;

	writel(regval, base + MA35_GP_REG_SLEWCTL);

	return 0;
}

static int ma35_pinconf_set_power_source(struct ma35_pinctrl *npctl,
					 unsigned int pin, int volt)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	if ((volt != VOLT_1_8) && (volt != VOLT_3_3))
		return -EINVAL;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_SPW);

	if (volt == VOLT_1_8)
		regval &= ~BIT(port);
	else
		regval |= BIT(port);

	writel(regval, base + MA35_GP_REG_SPW);

	return 0;
}

static int ma35_pinconf_get_power_source(struct ma35_pinctrl *npctl,
					 unsigned int pin)
{
	int port, group_num;
	void __iomem *base;
	u32 regval;

	ma35_gpio_cla_port(pin, &group_num, &port);
	base = npctl->ctrl->pin_banks[group_num].reg_base;

	regval = readl(base + MA35_GP_REG_SPW);

	if (regval & BIT(port))
		return VOLT_3_3;
	else
		return VOLT_1_8;
}

static int ma35_pinconf_get(struct pinctrl_dev *pctldev,
			   unsigned int pin, unsigned long *config)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	u16 arg;
	int ret;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_PULL_UP:
		if (ma35_pinconf_get_pull(npctl, pin) == param)
			arg = 1;
		else
			return -EINVAL;
		break;

	case PIN_CONFIG_DRIVE_STRENGTH:
		ret = ma35_pinconf_get_drive_strength(npctl, pin, &arg);
		if (ret)
			return ret;
		break;

	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		arg = ma35_pinconf_get_schmitt_enable(npctl, pin);
		break;

	case PIN_CONFIG_SLEW_RATE:
		arg = ma35_pinconf_get_slew_rate(npctl, pin);
		break;

	case PIN_CONFIG_OUTPUT_ENABLE:
		arg = ma35_pinconf_get_output(npctl, pin);
		break;

	case PIN_CONFIG_POWER_SOURCE:
		 arg = ma35_pinconf_get_power_source(npctl, pin);
		break;

	default:
		return -EINVAL;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int ma35_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
			    unsigned long *configs, unsigned int num_configs)
{
	struct ma35_pinctrl *npctl = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param;
	unsigned int arg = 0;
	int i, ret = 0;

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
		case PIN_CONFIG_BIAS_PULL_UP:
		case PIN_CONFIG_BIAS_PULL_DOWN:
			ret = ma35_pinconf_set_pull(npctl, pin, param);
			break;

		case PIN_CONFIG_DRIVE_STRENGTH:
			ret = ma35_pinconf_set_drive_strength(npctl, pin, arg);
			break;

		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			ret = ma35_pinconf_set_schmitt(npctl, pin, 1);
			break;

		case PIN_CONFIG_INPUT_SCHMITT:
			ret = ma35_pinconf_set_schmitt(npctl, pin, arg);
			break;

		case PIN_CONFIG_SLEW_RATE:
			ret = ma35_pinconf_set_slew_rate(npctl, pin, arg);
			break;

		case PIN_CONFIG_OUTPUT_ENABLE:
			ret = ma35_pinconf_set_output(npctl, pin, arg);
			break;

		case PIN_CONFIG_POWER_SOURCE:
			ret = ma35_pinconf_set_power_source(npctl, pin, arg);
			break;

		default:
			return -EINVAL;
		}
	}
	return ret;
}

static const struct pinconf_ops ma35_pinconf_ops = {
	.pin_config_get = ma35_pinconf_get,
	.pin_config_set = ma35_pinconf_set,
	.is_generic = true,
};

static int ma35_pinctrl_parse_groups(struct device_node *np,
				    struct ma35_pin_group *grp,
				    struct ma35_pinctrl *npctl, u32 index)
{
	struct ma35_pin_setting *pin;
	const __be32 *list;
	int i, j, size, ret;

	dev_dbg(npctl->dev, "group(%d): %s\n", index, np->name);

	grp->name = np->name;

	/*
	 * the binding format is nuvoton,pins = <bank pin-mfp pin-function>,
	 * do sanity check and calculate pins number
	 */
	list = of_get_property(np, "nuvoton,pins", &size);
	size /= sizeof(*list);
	if (!size || size % 4) {
		dev_err(npctl->dev, "wrong setting!\n");
		return -EINVAL;
	}

	grp->npins = size / 4;

	grp->pins = devm_kzalloc(npctl->dev, grp->npins * sizeof(*grp->pins),
				 GFP_KERNEL);
	if (!grp->pins)
		return -ENOMEM;

	pin = grp->settings = devm_kzalloc(npctl->dev,
					   grp->npins * sizeof(*grp->settings),
					   GFP_KERNEL);
	if (!grp->settings)
		return -ENOMEM;

	for (i = 0, j = 0; i < size; i += 4, j++) {
		struct device_node *np_config;
		const __be32 *phandle;

		pin->offset = be32_to_cpu(*list++);
		pin->shift = be32_to_cpu(*list++);
		pin->muxval = be32_to_cpu(*list++);

		phandle = list++;
		if (!phandle)
			return -EINVAL;

		np_config = of_find_node_by_phandle(be32_to_cpup(phandle));

		ret = pinconf_generic_parse_dt_config(np_config, NULL,
						      &pin->configs,
						      &pin->nconfigs);
		if (ret)
			return ret;
		grp->pins[j] = npctl->info->get_pin_num(pin->offset, pin->shift);
		pin++;
	}
	return 0;
}

static int ma35_pinctrl_parse_functions(struct device_node *np,
					struct ma35_pinctrl *npctl,
					u32 index)
{
	struct device_node *child;
	struct ma35_pin_func *func;
	struct ma35_pin_group *grp;
	static u32 grp_index;
	u32 ret, i = 0;

	dev_dbg(npctl->dev, "parse function(%d): %s\n", index, np->name);

	func = &npctl->functions[index];

	func->name = np->name;
	func->ngroups = of_get_child_count(np);

	if (func->ngroups <= 0)
		return 0;

	func->groups = devm_kzalloc(npctl->dev,
				    func->ngroups * sizeof(char *), GFP_KERNEL);
	if (!func->groups)
		return -ENOMEM;

	for_each_child_of_node(np, child) {
		func->groups[i] = child->name;
		grp = &npctl->groups[grp_index++];
		ret = ma35_pinctrl_parse_groups(child, grp, npctl, i++);
		if (ret) {
			of_node_put(child);
			return ret;
		}
	}
	return 0;
}

static int ma35_pinctrl_probe_dt(struct platform_device *pdev,
				 struct ma35_pinctrl *npctl)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	u32 i = 0;
	int ret;

	if (!np)
		return -ENODEV;

	for_each_child_of_node(np, child) {
		if (of_property_read_bool(child, "gpio-controller"))
			continue;
		npctl->nfunctions++;
		npctl->ngroups += of_get_child_count(child);
	}

	npctl->functions = devm_kzalloc(&pdev->dev,
					npctl->nfunctions * sizeof(*npctl->functions),
					GFP_KERNEL);
	if (!npctl->functions)
		return -ENOMEM;

	npctl->groups = devm_kzalloc(&pdev->dev,
				     npctl->ngroups * sizeof(*npctl->groups),
				     GFP_KERNEL);
	if (!npctl->groups)
		return -ENOMEM;

	dev_dbg(&pdev->dev, "nfunctions = %d\n", npctl->nfunctions);
	dev_dbg(&pdev->dev, "ngroups = %d\n", npctl->ngroups);

	i = 0;

	for_each_child_of_node(np, child) {
		if (of_property_read_bool(child, "gpio-controller"))
			continue;

		ret = ma35_pinctrl_parse_functions(child, npctl, i++);
		if (ret) {
			dev_err(&pdev->dev, "failed to parse function\n");
			of_node_put(child);
			return ret;
		}
	}
	return 0;
}

int ma35_pinctrl_probe(struct platform_device *pdev,
		       const struct ma35_pinctrl_soc_info *info)
{
	struct device_node *np = pdev->dev.of_node;
	struct pinctrl_desc *ma35_pinctrl_desc;
	struct ma35_pinctrl *npctl;
	int ret;

	if (!info || !info->pins || !info->npins) {
		dev_err(&pdev->dev, "wrong pinctrl info\n");
		return -EINVAL;
	}

	npctl = devm_kzalloc(&pdev->dev, sizeof(*npctl), GFP_KERNEL);
	if (!npctl)
		return -ENOMEM;

	ma35_pinctrl_desc = devm_kzalloc(&pdev->dev, sizeof(*ma35_pinctrl_desc), GFP_KERNEL);
	if (!ma35_pinctrl_desc)
		return -ENOMEM;

	npctl->ctrl = devm_kzalloc(&pdev->dev, sizeof(*npctl->ctrl), GFP_KERNEL);
	if (!npctl->ctrl)
		return -ENOMEM;

	ma35_pinctrl_desc->name = dev_name(&pdev->dev);
	ma35_pinctrl_desc->pins = info->pins;
	ma35_pinctrl_desc->npins = info->npins;
	ma35_pinctrl_desc->pctlops = &ma35_pctrl_ops;
	ma35_pinctrl_desc->pmxops = &ma35_pmx_ops;
	ma35_pinctrl_desc->confops = &ma35_pinconf_ops;
	ma35_pinctrl_desc->owner = THIS_MODULE;

	npctl->info = info;
	npctl->dev = &pdev->dev;

	npctl->regmap = syscon_regmap_lookup_by_phandle(np, "nuvoton,sys");
	if (IS_ERR(npctl->regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(npctl->regmap),
				     "No syscfg phandle specified\n");

	ma35_pinctrl_get_soc_data(npctl, pdev);

	platform_set_drvdata(pdev, npctl);

	ret = ma35_pinctrl_probe_dt(pdev, npctl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "fail to probe MA35 pinctrl dt\n");

	ret = devm_pinctrl_register_and_init(&pdev->dev, ma35_pinctrl_desc,
					     npctl, &npctl->pctl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "fail to register MA35 pinctrl\n");

	ret = pinctrl_enable(npctl->pctl);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "fail to enable MA35 pinctrl\n");

	return ma35_gpiolib_register(pdev, npctl);
}

int __maybe_unused ma35_pinctrl_suspend(struct device *dev)
{
	struct ma35_pinctrl *npctl = dev_get_drvdata(dev);

	return pinctrl_force_sleep(npctl->pctl);
}

int __maybe_unused ma35_pinctrl_resume(struct device *dev)
{
	struct ma35_pinctrl *npctl = dev_get_drvdata(dev);

	return pinctrl_force_default(npctl->pctl);
}
