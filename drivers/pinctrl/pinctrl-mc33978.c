// SPDX-License-Identifier: GPL-2.0-only
/*
 * MC33978/MC33978 Multiple Switch Detection Interface - Pinctrl/GPIO Driver
 *
 * Driver Purpose:
 * ===============
 * Provides GPIO and pinctrl interfaces for the 22 switch detection inputs.
 * Handles digital input reading, interrupt processing, and wetting current
 * configuration. (Analog AMUX functionality will be handled by separate IIO driver)
 *
 * GPIO Number to Hardware Input Mapping:
 * =======================================
 * GPIO 0-13:  SG0-SG13 (Switch-to-Ground inputs)
 * GPIO 14-21: SP0-SP7 (Programmable: Switch-to-Ground or Switch-to-Battery)
 *
 * This mapping is dictated by the READ_IN register bit layout where
 * bits [21:14] = SP[7:0] and bits [13:0] = SG[13:0].
 *
 * Register Organization:
 * ======================
 * Most configuration registers are paired:
 * - _SP register at offset N controls SP0-SP7
 * - _SG register at offset N+2 controls SG0-SG13
 * Helper macros MC33978_SPSG() and MC33978_PINSHIFT() handle this mapping.
 *
 * Wetting Current Configuration:
 * ===============================
 * - 8 selectable values: 2, 6, 8, 10, 12, 14, 16, 20 mA (3-bit encoding)
 * - Stored in WET_SP (0x08), WET_SG0 (0x0a), WET_SG1 (0x0c)
 * - Each input uses 3 bits, 8 inputs per register
 * - Exposed via pinconf PIN_CONFIG_DRIVE_STRENGTH parameter (in mA)
 *
 * Interrupt Handling:
 * ===================
 * - Device asserts INT_B when any enabled input changes state
 * - Driver must read READ_IN to get current state and clear INT_B
 * - Previous state stored in mpc->state to compute edges
 * - Per-input interrupt enable via IE_SP/IE_SG registers
 * - IRQ handler and child IRQ operations run in threaded context (can sleep)
 *
 * Regcache Management:
 * ====================
 * - READ_IN is volatile (always re-read from hardware)
 * - Configuration registers are cached by regmap
 * - irq_bus_lock/unlock pattern batches multiple register writes into single
 *   SPI transaction for efficiency when configuring multiple IRQs
 *
 * Copyright 2024 Protonic Holland
 * Written by David Jander <david@protonic.nl>
 * Based loosely on pinctrl-mcp23s08
 *
 * Datasheet:
 * https://www.nxp.com/docs/en/data-sheet/MC33978.pdf
 */

#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

#include <linux/mfd/mc33978.h>

#define MC33978_NGPIO		22

/*
 * Input numbering is dictated by bit-order of the input register:
 * Inputs 0-13 -> SG0-SG13
 * Inputs 14-21 -> SP0-SP7
 */
#define MC33978_NUM_SG		14
#define MC33978_IS_SP(pin)	((pin) >= MC33978_NUM_SG)
#define MC33978_SP_MASK		GENMASK(MC33978_NGPIO - 1, MC33978_NUM_SG)
#define MC33978_SG_MASK		GENMASK(MC33978_NUM_SG - 1, 0)
#define MC33978_SG_SHIFT	0
#define MC33978_SP_SHIFT	MC33978_NUM_SG

/* Choose register offset for _SG/_SP registers. reg is always the _SP addr. */
#define MC33978_SPSG(reg, pin)	(MC33978_IS_SP(pin) ? (reg) : (reg) + 2)

/* Get the bit index into the corresponding register */
#define MC33978_PINSHIFT(pin)	(MC33978_IS_SP(pin) ? (pin) - MC33978_NUM_SG : (pin))
#define MC33978_PINMASK(pin)	BIT(MC33978_PINSHIFT(pin))

/*
 * The same thing for the wetting current registers, but those are 3 in total
 * and each pin uses a 3-bit field, 8 pins per register, except for the last
 * one.
 */
#define MC33978_WREG(reg, pin)	((reg) + (MC33978_IS_SP(pin) ? \
			0 : 2 + 2 * ((pin) / 8)))
#define MC33978_WSHIFT(pin)	(MC33978_IS_SP(pin) ? \
		(3 * ((pin) - MC33978_NUM_SG)) : (3 * ((pin) % 8)))
#define MC33978_WMASK(pin)	(7 << MC33978_WSHIFT(pin))

#define MC33978_TRISTATE	0
#define MC33978_PU		1
#define MC33978_PD		2



struct mc33978_pinctrl {
	struct device *dev;
	struct regmap *regmap;
	int irq;

	struct irq_domain *domain;

	struct gpio_chip chip;
	struct pinctrl_dev *pctldev;
	struct pinctrl_desc pinctrl_desc;

	/* Interrupt state management */
	struct mutex lock;		/* Protects state, irq_rise/fall */
	unsigned int state;		/* Last read input state */
	unsigned int irq_rise;		/* Rising edge config mask */
	unsigned int irq_fall;		/* Falling edge config mask */
};
static const struct pinctrl_pin_desc mc33978_pins[] = {
	PINCTRL_PIN(0, "sg0"),
	PINCTRL_PIN(1, "sg1"),
	PINCTRL_PIN(2, "sg2"),
	PINCTRL_PIN(3, "sg3"),
	PINCTRL_PIN(4, "sg4"),
	PINCTRL_PIN(5, "sg5"),
	PINCTRL_PIN(6, "sg6"),
	PINCTRL_PIN(7, "sg7"),
	PINCTRL_PIN(8, "sg8"),
	PINCTRL_PIN(9, "sg9"),
	PINCTRL_PIN(10, "sg10"),
	PINCTRL_PIN(11, "sg11"),
	PINCTRL_PIN(12, "sg12"),
	PINCTRL_PIN(13, "sg13"),
	PINCTRL_PIN(14, "sp0"),
	PINCTRL_PIN(15, "sp1"),
	PINCTRL_PIN(16, "sp2"),
	PINCTRL_PIN(17, "sp3"),
	PINCTRL_PIN(18, "sp4"),
	PINCTRL_PIN(19, "sp5"),
	PINCTRL_PIN(20, "sp6"),
	PINCTRL_PIN(21, "sp7"),
};

static int mc33978_read(struct mc33978_pinctrl *mpc, u8 reg, u32 *val)
{
	int ret = regmap_read(mpc->regmap, reg, val);

	if (ret)
		dev_err_ratelimited(mpc->dev,
				"Regmap read error %d at reg: %02x.\n",
				ret, reg);
	return ret;
}

static int mc33978_update_bits(struct mc33978_pinctrl *mpc, u8 reg, u32 mask, u32 val)
{
	int ret;

	ret = regmap_update_bits(mpc->regmap, reg, mask, val);
	if (ret)
		dev_err_ratelimited(mpc->dev,
				"Regmap update bits error %d at reg: %02x.\n",
				ret, reg);
	return ret;
}

static int mc33978_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	return 0;
}

static const char *mc33978_pinctrl_get_group_name(struct pinctrl_dev *pctldev,
							unsigned int group)
{
	return NULL;
}

static int mc33978_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
						unsigned int group,
						const unsigned int **pins,
						unsigned int *num_pins)
{
	return -EOPNOTSUPP;
}

static const struct pinctrl_ops mc33978_pinctrl_ops = {
	.get_groups_count = mc33978_pinctrl_get_groups_count,
	.get_group_name = mc33978_pinctrl_get_group_name,
	.get_group_pins = mc33978_pinctrl_get_group_pins,
#ifdef CONFIG_OF
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinconf_generic_dt_free_map,
#endif
};

static int mc33978_get_pull(struct mc33978_pinctrl *mpc, unsigned int pin, int *val)
{
	int ret;
	unsigned int data;

	ret = mc33978_read(mpc, MC33978_SPSG(MC33978_REG_TRI_SP, pin), &data);
	if (ret < 0)
		return ret;

	/* Is the pin tri-stated? */
	if (data & MC33978_PINMASK(pin)) {
		*val = MC33978_TRISTATE;
		return 0;
	}

	/* Pins 0..13 only support pull-up */
	if (!MC33978_IS_SP(pin)) {
		*val = MC33978_PU;
		return 0;
	}

	/* Check pin pull direction for pins 14..21 */
	ret = mc33978_read(mpc, MC33978_REG_CONFIG, &data);
	if (ret < 0)
		return ret;
	if (data & MC33978_PINMASK(pin))
		*val = MC33978_PD;
	else
		*val = MC33978_PU;
	return 0;
}

static int mc33978_set_pull(struct mc33978_pinctrl *mpc, unsigned int pin, int val)
{
	int ret;
	unsigned int mask = MC33978_PINMASK(pin);

	/* 1. Hardware-Schutz: SG-Pins haben physikalisch keine Pull-Downs */
	if ((val == MC33978_PD) && !MC33978_IS_SP(pin))
		return -EINVAL;

	/* 2. Richtung konfigurieren (Ausschließlich für SP-Pins) */
	if (MC33978_IS_SP(pin) && val != MC33978_TRISTATE) {
		/* CONFIG (0x02): 0 = Switch-to-Ground (PU), 1 = Switch-to-Battery (PD) */
		ret = mc33978_update_bits(mpc, MC33978_REG_CONFIG, mask,
					  (val == MC33978_PD) ? mask : 0);
		if (ret)
			return ret;
	}

	/* 3. Pull-Widerstand aktivieren oder in Tri-State versetzen
	 * TRI-Register: 0 = Pull aktiv, 1 = Tri-State (Hochohmig)
	 */
	ret = mc33978_update_bits(mpc, MC33978_SPSG(MC33978_REG_TRI_SP, pin),
				  mask,
				  (val == MC33978_TRISTATE) ? mask : 0);
	return ret;
}

static int mc33978_get_ds(struct mc33978_pinctrl *mpc, unsigned int pin,
		unsigned int *val)
{
	int ret;
	unsigned int data;

	ret = mc33978_read(mpc, MC33978_WREG(MC33978_REG_WET_SP, pin), &data);
	if (ret)
		return ret;

	data &= MC33978_WMASK(pin);
	data >>= MC33978_WSHIFT(pin);

	/* DS levels: 2, 6, 8, 10, 12, 14, 16, 20mA */
	if (!data)
		*val = 2;
	else if (data == 7)
		*val = 20;
	else
		*val = (data + 2) * 2;

	return 0;
}

static int mc33978_set_ds(struct mc33978_pinctrl *mpc, unsigned int pin,
		unsigned int val)
{
	int ret;

	/* DS levels: 2, 6, 8, 10, 12, 14, 16, 20mA */
	if ((val < 2) || (val > 20) || (val == 4) || (val == 18) || (val & 1))
		return -EOPNOTSUPP;

	val >>= 1;
	val--;
	if (val)
		val--;
	if (val > 7)
		val = 7;
	ret = mc33978_update_bits(mpc, MC33978_WREG(MC33978_REG_WET_SP, pin),
			MC33978_WMASK(pin),
			val << MC33978_WSHIFT(pin));

	return ret;
}

static int mc33978_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
		unsigned long *config)
{
	struct mc33978_pinctrl *mpc = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	unsigned int data, status;
	int ret;

	switch (param) {
	case PIN_CONFIG_BIAS_PULL_UP:
		ret = mc33978_get_pull(mpc, pin, &data);
		if (ret)
			return ret;
		status = (data == MC33978_PU);
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		ret = mc33978_get_pull(mpc, pin, &data);
		if (ret)
			return ret;
		status = (data == MC33978_PD);
		break;
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		ret = mc33978_get_pull(mpc, pin, &data);
		if (ret)
			return ret;
		status = (data == MC33978_TRISTATE);
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		ret = mc33978_get_ds(mpc, pin, &data);
		if (ret)
			return ret;
		*config = pinconf_to_config_packed(param, data);
		status = 1;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return status ? 0 : -EINVAL;
}

/*
 * Hardware constraint regarding PIN_CONFIG_BIAS_PULL_UP/DOWN:
 * The MC33978 utilizes active constant current sources (wetting currents)
 * rather than passive pull-resistors. Since the equivalent ohmic resistance
 * scales dynamically with the fluctuating board voltage (VBATP), computing
 * a static ohm value is physically invalid.
 * The driver intentionally ignores resistance arguments during configuration
 * and continuously reports 0 ohms to the pinctrl framework.
 */
static int mc33978_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
		unsigned long *configs, unsigned int num_configs)
{
	struct mc33978_pinctrl *mpc = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param;
	u32 arg;
	int ret = 0;
	int i;

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_DRIVE_OPEN_SOURCE:
		case PIN_CONFIG_BIAS_PULL_UP:
			ret = mc33978_set_pull(mpc, pin, MC33978_PU);
			break;
		case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		case PIN_CONFIG_BIAS_PULL_DOWN:
			if (!MC33978_IS_SP(pin)) {
				dev_err(mpc->dev, "Pin %u is SG and does not support pull-down\n",
					pin);
				return -EINVAL;
			}
			ret = mc33978_set_pull(mpc, pin, MC33978_PD);
			break;
		case PIN_CONFIG_DRIVE_STRENGTH_UA:
			arg /= 1000;
			fallthrough;
		case PIN_CONFIG_DRIVE_STRENGTH:
			ret = mc33978_set_ds(mpc, pin, arg);
			break;
		default:
			return -EOPNOTSUPP;
		}

		if (ret) {
			dev_err(mpc->dev, "Failed to set config param %04x for pin %u: %d\n",
					param, pin, ret);
			return ret;
		}
	}

	return ret;
}

static const struct pinconf_ops mc33978_pinconf_ops = {
	.pin_config_get = mc33978_pinconf_get,
	.pin_config_set = mc33978_pinconf_set,
	.is_generic = true,
};

static int mc33978_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);

	/*
	 * This chip only has inputs. We emulate outputs by setting a
	 * wetting current and/or using the tri-state register to turn it on
	 * and off. If a pin was an output and is now tri-stated, we should
	 * disable the tri-state now to make the input work correctly.
	 */
	mutex_lock(&mpc->lock);
	mc33978_update_bits(mpc, MC33978_SPSG(MC33978_REG_TRI_SP, offset),
			MC33978_PINMASK(offset), 0);
	mutex_unlock(&mpc->lock);

	return 0;
}

static int mc33978_get(struct gpio_chip *chip, unsigned int offset)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);
	int status, ret;
	bool is_switch_closed;
	bool is_switch_to_ground = true; /* Default for all SG pins */

	mutex_lock(&mpc->lock);

	/* Read hardware switch status (open or closed) */
	ret = mc33978_read(mpc, MC33978_REG_READ_IN, &status);
	if (ret < 0) {
		mutex_unlock(&mpc->lock);
		return 0;
	}
	is_switch_closed = !!(status & BIT(offset));

	/* Determine current topology for SP pins */
	if (MC33978_IS_SP(offset)) {
		int config_reg;

		ret = mc33978_read(mpc, MC33978_REG_CONFIG, &config_reg);
		if (ret == 0) {
			/* CONFIG: 0 = Switch-to-Ground (PU), 1 = Switch-to-Battery (PD) */
			if (config_reg & MC33978_PINMASK(offset))
				is_switch_to_ground = false;
		}
	}

	mutex_unlock(&mpc->lock);

	/* Translate hardware switch semantics to logical GPIO levels */
	if (is_switch_to_ground) {
		/* SG: Switch open -> High (1), Switch to GND -> Low (0) */
		status = !is_switch_closed;
	} else {
		/* SB: Switch open -> Low (0), Switch to Vbat -> High (1) */
		status = is_switch_closed;
	}

	return status;
}

static int mc33978_get_multiple(struct gpio_chip *chip,
				unsigned long *mask, unsigned long *bits)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);
	unsigned int status;
	unsigned int config_reg = 0;
	unsigned int inv_mask;
	int ret;

	mutex_lock(&mpc->lock);

	ret = mc33978_read(mpc, MC33978_REG_READ_IN, &status);
	if (ret)
		goto out_unlock;

	/* Read CONFIG register only if the requested mask involves SP pins */
	if (*mask & MC33978_SP_MASK) {
		ret = mc33978_read(mpc, MC33978_REG_CONFIG, &config_reg);
		if (ret)
			goto out_unlock;
	}

	/*
	 * SG pins (0-13) are always Switch-to-Ground.
	 * SP pins (14-21) are Switch-to-Ground if their CONFIG bit is 0.
	 * Switch-to-Ground logic: HW bit 0 (open) -> Logical 1 (High)
	 * HW bit 1 (closed) -> Logical 0 (Low)
	 * We create a mask for all Switch-to-Ground pins and XOR the status.
	 */
	inv_mask = MC33978_SG_MASK | (~(config_reg << MC33978_NUM_SG) & MC33978_SP_MASK);

	*bits = (status ^ inv_mask) & *mask;

out_unlock:
	mutex_unlock(&mpc->lock);

	return ret;
}

static int mc33978_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);
	int pull;
	int ret;

	/*
	 * We only have inputs with wetting current sources, that we mis-use
	 * as open-drain/-source outputs.
	 */
	if (MC33978_IS_SP(offset)) {
		pull = value ? MC33978_PU : MC33978_PD;
		value = 1;
	} else {
		pull = MC33978_PU;
	}

	mutex_lock(&mpc->lock);

	/*
	 * Break-before-make sequencing to prevent hardware glitches (spikes).
	 * Since SPI transfers take time, writing the pull and tri-state
	 * registers in the wrong order causes a brief moment where current
	 * flows to the pin before it is masked, causing a visible LED flash.
	 */
	if (value) {
		/*
		 * Turn ON: Configure the underlying current source (pull) first,
		 * then route it to the pin by disabling tri-state.
		 */
		ret = mc33978_set_pull(mpc, offset, pull);
		if (ret)
			goto out_unlock;

		ret = mc33978_update_bits(mpc, MC33978_SPSG(MC33978_REG_TRI_SP, offset),
					  MC33978_PINMASK(offset), 0);
	} else {
		/*
		 * Turn OFF: Isolate the pin first by enabling tri-state,
		 * then safely disable the underlying current source.
		 */
		ret = mc33978_update_bits(mpc, MC33978_SPSG(MC33978_REG_TRI_SP, offset),
					  MC33978_PINMASK(offset), MC33978_PINMASK(offset));
	}

out_unlock:
	mutex_unlock(&mpc->lock);

	return ret;
}

static int mc33978_set_multiple(struct gpio_chip *chip,
				unsigned long *mask, unsigned long *bits)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);
	unsigned int sgmask = (*mask & MC33978_SG_MASK) >> MC33978_SG_SHIFT;
	unsigned int sgbits = (*bits & MC33978_SG_MASK) >> MC33978_SG_SHIFT;
	unsigned int spmask = (*mask & MC33978_SP_MASK) >> MC33978_SP_SHIFT;
	unsigned int spbits = (*bits & MC33978_SP_MASK) >> MC33978_SP_SHIFT;

	mutex_lock(&mpc->lock);
	if (spmask)
		mc33978_update_bits(mpc, MC33978_REG_TRI_SP, spmask, ~spbits);
	if (sgmask)
		mc33978_update_bits(mpc, MC33978_REG_TRI_SG, sgmask, ~sgbits);
	mutex_unlock(&mpc->lock);

	return 0;
}

static int mc33978_direction_output(struct gpio_chip *chip, unsigned int offset,
		int value)
{
	return mc33978_set(chip, offset, value);
}

static int mc33978_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);
	int virq;

	if (!mpc->domain)
		return -ENXIO;

	/* * Erzeugt das Mapping zur Laufzeit (oder gibt ein bestehendes zurück).
	 * Ohne diesen Aufruf bleibt die lineare IRQ-Domain leer.
	 */
	virq = irq_create_mapping(mpc->domain, offset);
	if (!virq) {
		dev_err(mpc->dev, "Failed to map hwirq %u to virq\n", offset);
		return -ENXIO;
	}

	return virq;
}

static void mc33978_init_gpio_chip(struct mc33978_pinctrl *mpc,
				   struct device *dev)
{
	mpc->chip.label = dev_name(dev);
	mpc->chip.direction_input = mc33978_direction_input;
	mpc->chip.get = mc33978_get;
	mpc->chip.get_multiple = mc33978_get_multiple;
	mpc->chip.direction_output = mc33978_direction_output;
	mpc->chip.set = mc33978_set;
	mpc->chip.set_multiple = mc33978_set_multiple;
	mpc->chip.set_config = gpiochip_generic_config;

	mpc->chip.to_irq = mc33978_gpio_to_irq;

	mpc->chip.base = -1;
	mpc->chip.ngpio = MC33978_NGPIO;
	mpc->chip.can_sleep = true;
	mpc->chip.parent = dev;
	mpc->chip.owner = THIS_MODULE;
}

static void mc33978_init_pinctrl_desc(struct mc33978_pinctrl *mpc,
				      struct device *dev)
{
	mpc->pinctrl_desc.name = dev_name(dev);

	mpc->pinctrl_desc.pctlops = &mc33978_pinctrl_ops;
	mpc->pinctrl_desc.confops = &mc33978_pinconf_ops;
	mpc->pinctrl_desc.pins = mc33978_pins;
	mpc->pinctrl_desc.npins = MC33978_NGPIO;
	mpc->pinctrl_desc.owner = THIS_MODULE;
}

static int mc33978_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mc33978_pinctrl *mpc;
	struct device_node *np = dev->of_node;
	int ret;

	if (!np)
		return dev_err_probe(dev, -EINVAL, "Missing device tree node\n");

	mpc = devm_kzalloc(dev, sizeof(*mpc), GFP_KERNEL);
	if (!mpc)
		return -ENOMEM;

	mpc->dev = dev;

	/* Get regmap from parent MFD device */
	mpc->regmap = dev_get_regmap(dev->parent, NULL);
	if (!mpc->regmap)
		return dev_err_probe(dev, -ENODEV, "Failed to get parent regmap\n");

	/*
	 * Get IRQ domain from parent's interrupt-controller.
	 * The parent (MFD) node has interrupt-controller properties,
	 * so we can get the domain from there.
	 */
	mpc->domain = irq_find_host(dev->parent->of_node);
	if (!mpc->domain)
		return dev_err_probe(dev, -ENODEV, "Failed to find parent IRQ domain\n");

	mutex_init(&mpc->lock);

	/* 3. GPIO Chip Setup */
	mc33978_init_gpio_chip(mpc, dev);
	mc33978_init_pinctrl_desc(mpc, dev);

	mpc->pctldev = devm_pinctrl_register(dev, &mpc->pinctrl_desc, mpc);
	if (IS_ERR(mpc->pctldev))
		return dev_err_probe(dev, PTR_ERR(mpc->pctldev),
				     "can't register pinctrl\n");

	ret = devm_gpiochip_add_data(dev, &mpc->chip, mpc);
	if (ret < 0)
		return dev_err_probe(dev, ret, "can't add GPIO chip\n");

	platform_set_drvdata(pdev, mpc);

	return 0;
}

static const struct of_device_id mc33978_pinctrl_of_match[] = {
	{ .compatible = "nxp,mc33978-pinctrl" },
	{ .compatible = "nxp,mc34978-pinctrl" },
	{ }
};
MODULE_DEVICE_TABLE(of, mc33978_pinctrl_of_match);

static struct platform_driver mc33978_pinctrl_driver = {
	.driver = {
		.name = "mc33978-pinctrl",
		.of_match_table = mc33978_pinctrl_of_match,
	},
	.probe = mc33978_pinctrl_probe,
};
module_platform_driver(mc33978_pinctrl_driver);

MODULE_AUTHOR("David Jander <david@protonic.nl>");
MODULE_DESCRIPTION("NXP MC33978/MC33978 pinctrl driver");
MODULE_LICENSE("GPL");
