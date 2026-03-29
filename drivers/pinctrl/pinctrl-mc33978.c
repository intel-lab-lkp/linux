// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 David Jander <david@protonic.nl>, Protonic Holland
 * Copyright (C) 2026 Oleksij Rempel <kernel@pengutronix.de>, Pengutronix
 *
 * MC33978/MC34978 Multiple Switch Detection Interface - Pinctrl/GPIO Driver
 *
 * Provides GPIO and pinctrl interfaces for the 22 switch detection inputs.
 * Handles digital input reading and wetting current configuration. Analog AMUX
 * functionality is handled by a separate mux driver.
 *
 * GPIO Mapping:
 * - GPIO 0-13:  SG0-SG13 (Switch-to-Ground inputs)
 * - GPIO 14-21: SP0-SP7 (Programmable: Switch-to-Ground or Switch-to-Battery)
 * This is dictated by the READ_IN register where bits [21:14] = SP[7:0]
 * and bits [13:0] = SG[13:0].
 *
 * Register Organization:
 * Configuration registers are generally paired. The _SP register at offset N
 * controls SP0-SP7, and the _SG register at offset N+2 controls SG0-SG13.
 *
 * Wetting Currents vs. Pull Resistors:
 * The hardware physically lacks traditional passive pull-up or pull-down
 * resistors. Instead, it uses active, controllable current regulators
 * (wetting currents) to detect switch states and clean mechanical contacts.
 * - Because these are active current sources, specifying an ohmic value for
 * pull-up/down biases is physically invalid. The driver ignores ohm arguments.
 * - 8 selectable current values: 2, 6, 8, 10, 12, 14, 16, 20 mA.
 * - Exposed via the pinconf PIN_CONFIG_DRIVE_STRENGTH parameter (in mA).
 *
 * Emulated Outputs:
 * The hardware lacks traditional push-pull output drivers; it is strictly an
 * input device. "Outputs" are simulated by toggling the wetting currents and
 * physically isolating the pins via hardware tri-state registers. Consequently,
 * consumers MUST flag outputs with GPIO_OPEN_DRAIN or GPIO_OPEN_SOURCE in
 * the Device Tree.
 *
 * Input Detection Mechanics:
 * This input mechanism relies on the active current regulators rather than
 * passive hard resistors. For a Switch-to-Ground (SG) pin, the chip sources
 * a constant current. When the switch is open, the pin voltage floats up to
 * the battery voltage. When the switch closes, it creates a path to ground;
 * because the current is strictly regulated, the pin voltage drops sharply
 * below the internal 4.0V comparator threshold.
 * * The hardware evaluates this and reports an abstract "contact status"
 * (1 = closed, 0 = open). For SG pins, a closed switch (~0V) reports as '1'.
 * To align with gpiolib expectations where ~0V equals a physical logical '0',
 * this driver explicitly inverts the hardware status for all SG-configured
 * pins before reporting them.
 *
 * Interrupts:
 * The physical INT_B line and threaded IRQ domain are managed centrally by
 * the parent MFD core. This driver implements a hierarchical irq_chip
 * to proxy masking/unmasking and configuration to the parent domain.
 *
 * Written by David Jander <david@protonic.nl>
 *
 * Datasheet:
 * https://www.nxp.com/docs/en/data-sheet/MC33978.pdf
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
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
#define MC33978_SP_MASK		GENMASK(MC33978_NGPIO - 1, MC33978_NUM_SG)
#define MC33978_SG_MASK		GENMASK(MC33978_NUM_SG - 1, 0)
#define MC33978_SG_SHIFT	0
#define MC33978_SP_SHIFT	MC33978_NUM_SG

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

	/*
	 * Protects multi-register hardware sequences in .set() and atomic
	 * READ_IN + CONFIG reads in .get()
	 */
	struct mutex lock;
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

static inline bool mc33978_is_sp(unsigned int pin)
{
	return pin >= MC33978_NUM_SG;
}

/* Choose register offset for _SG/_SP registers. reg is always the _SP addr. */
static inline u8 mc33978_spsg(u8 reg, unsigned int pin)
{
	return mc33978_is_sp(pin) ? reg : reg + 2;
}

/* Get the bit index into the corresponding register */
static inline unsigned int mc33978_pinshift(unsigned int pin)
{
	return mc33978_is_sp(pin) ? pin - MC33978_NUM_SG : pin;
}

#define MC33978_PINMASK(pin)	BIT(mc33978_pinshift(pin))

/*
 * Wetting current registers: 3 in total, each pin uses a 3-bit field,
 * 8 pins per register, except for the last one.
 */
static inline u8 mc33978_wreg(u8 reg, unsigned int pin)
{
	return reg + (mc33978_is_sp(pin) ? 0 : 2 + 2 * (pin / 8));
}

static inline unsigned int mc33978_wshift(unsigned int pin)
{
	return mc33978_is_sp(pin) ? 3 * (pin - MC33978_NUM_SG) : 3 * (pin % 8);
}

#define MC33978_WMASK(pin)	(7 << mc33978_wshift(pin))

static int mc33978_read(struct mc33978_pinctrl *mpc, u8 reg, u32 *val)
{
	int ret;

	ret = regmap_read(mpc->regmap, reg, val);
	if (ret)
		dev_err_ratelimited(mpc->dev, "Regmap read error %d at reg: %02x.\n",
				    ret, reg);
	return ret;
}

static int mc33978_update_bits(struct mc33978_pinctrl *mpc, u8 reg, u32 mask,
			       u32 val)
{
	int ret;

	ret = regmap_update_bits(mpc->regmap, reg, mask, val);
	if (ret)
		dev_err_ratelimited(mpc->dev, "Regmap update bits error %d at reg: %02x.\n",
				    ret, reg);
	return ret;
}

static const struct pinctrl_ops mc33978_pinctrl_ops = {
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static int mc33978_get_pull(struct mc33978_pinctrl *mpc, unsigned int pin, u32 *val)
{
	u32 data;
	int ret;

	lockdep_assert_held(&mpc->lock);

	ret = mc33978_read(mpc, mc33978_spsg(MC33978_REG_TRI_SP, pin), &data);
	if (ret)
		return ret;

	/* Is the pin tri-stated? */
	if (data & MC33978_PINMASK(pin)) {
		*val = MC33978_TRISTATE;
		return 0;
	}

	/* Pins 0..13 only support pull-up */
	if (!mc33978_is_sp(pin)) {
		*val = MC33978_PU;
		return 0;
	}

	/* Check pin pull direction for pins 14..21 */
	ret = mc33978_read(mpc, MC33978_REG_CONFIG, &data);
	if (ret)
		return ret;

	if (data & MC33978_PINMASK(pin))
		*val = MC33978_PD;
	else
		*val = MC33978_PU;

	return 0;
}

static int mc33978_set_pull(struct mc33978_pinctrl *mpc, unsigned int pin, int val)
{
	u32 mask = MC33978_PINMASK(pin);
	int ret;

	lockdep_assert_held(&mpc->lock);

	/* SG pins physically lack pull-downs current sources */
	if (val == MC33978_PD && !mc33978_is_sp(pin))
		return -EINVAL;

	/* Configure direction (Exclusively for SP pins) */
	if (mc33978_is_sp(pin) && val != MC33978_TRISTATE) {
		ret = mc33978_update_bits(mpc, MC33978_REG_CONFIG, mask,
					  (val == MC33978_PD) ? mask : 0);
		if (ret)
			return ret;
	}

	/* Enable current source or set to tri-state  */
	return mc33978_update_bits(mpc, mc33978_spsg(MC33978_REG_TRI_SP, pin),
				   mask,
				   (val == MC33978_TRISTATE) ? mask : 0);
}

static const unsigned int mc33978_wet_mA[] = { 2, 6, 8, 10, 12, 14, 16, 20 };

static int mc33978_set_ds(struct mc33978_pinctrl *mpc, unsigned int pin,
			  u32 val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mc33978_wet_mA); i++) {
		if (val == mc33978_wet_mA[i]) {
			return mc33978_update_bits(mpc,
					mc33978_wreg(MC33978_REG_WET_SP, pin),
					MC33978_WMASK(pin),
					i << mc33978_wshift(pin));
		}
	}

	return -EINVAL;
}

static int mc33978_get_ds(struct mc33978_pinctrl *mpc, unsigned int pin,
			  u32 *val)
{
	u32 data;
	int ret;

	ret = mc33978_read(mpc, mc33978_wreg(MC33978_REG_WET_SP, pin), &data);
	if (ret)
		return ret;

	data &= MC33978_WMASK(pin);
	data >>= mc33978_wshift(pin);

	if (data >= ARRAY_SIZE(mc33978_wet_mA))
		return -EINVAL;

	*val = mc33978_wet_mA[data];

	return 0;
}

static int mc33978_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
			       unsigned long *config)
{
	struct mc33978_pinctrl *mpc = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 arg;
	u32 data;
	int ret;

	guard(mutex)(&mpc->lock);

	switch (param) {
	case PIN_CONFIG_BIAS_PULL_UP:
		ret = mc33978_get_pull(mpc, pin, &data);
		if (ret)
			return ret;
		if (data != MC33978_PU)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		ret = mc33978_get_pull(mpc, pin, &data);
		if (ret)
			return ret;
		if (data != MC33978_PD)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		if (!mc33978_is_sp(pin))
			return -EINVAL;

		ret = mc33978_read(mpc, MC33978_REG_CONFIG, &data);
		if (ret)
			return ret;

		if (!(data & MC33978_PINMASK(pin)))
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_DRIVE_OPEN_SOURCE:
		if (mc33978_is_sp(pin)) {
			ret = mc33978_read(mpc, MC33978_REG_CONFIG, &data);
			if (ret)
				return ret;

			if (data & MC33978_PINMASK(pin))
				return -EINVAL;
		}
		arg = 1;
		break;
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		ret = mc33978_get_pull(mpc, pin, &data);
		if (ret)
			return ret;
		if (data != MC33978_TRISTATE)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		ret = mc33978_get_ds(mpc, pin, &data);
		if (ret)
			return ret;
		arg = data;
		break;
	default:
		/*
		 * Ignore checkpatch warning: the pinctrl core specifically
		 * expects -ENOTSUPP to silently skip unsupported generic
		 * parameters. Using -EOPNOTSUPP causes debugfs read failures.
		 */
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
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
	int ret = 0;
	u32 arg;
	int i;

	guard(mutex)(&mpc->lock);

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		/*
		 * The hardware physically lacks push-pull output drivers.
		 * By explicitly handling OPEN_DRAIN and OPEN_SOURCE here, we
		 * signal to gpiolib that we support these modes "natively".
		 * This crucially prevents gpiolib from falling back to its
		 * software emulation (which sets the pin to input mode to
		 * achieve High-Z). On the MC33978, input mode is NOT High-Z;
		 * it actively drives the line with a wetting current!
		 */
		switch (param) {
		case PIN_CONFIG_DRIVE_OPEN_SOURCE:
			/* Setup topology only; do not turn on current yet */
			if (mc33978_is_sp(pin))
				ret = mc33978_update_bits(mpc, MC33978_REG_CONFIG,
							  MC33978_PINMASK(pin), 0);
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			ret = mc33978_set_pull(mpc, pin, MC33978_PU);
			break;
		case PIN_CONFIG_DRIVE_OPEN_DRAIN:
			if (!mc33978_is_sp(pin)) {
				dev_err(mpc->dev, "Pin %u is SG and does not support open-drain\n",
					pin);
				return -EINVAL;
			}
			/* Setup topology only; do not turn on current yet */
			ret = mc33978_update_bits(mpc, MC33978_REG_CONFIG,
						  MC33978_PINMASK(pin),
						  MC33978_PINMASK(pin));
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			if (!mc33978_is_sp(pin)) {
				dev_err(mpc->dev, "Pin %u is SG and does not support pull-down\n",
					pin);
				return -EINVAL;
			}
			ret = mc33978_set_pull(mpc, pin, MC33978_PD);
			break;
		/*
		 * The MC33978 uses active wetting currents rather than passive
		 * pull-resistors. Disabling the bias (pull-up/down) is
		 * physically equivalent to putting the pin into a
		 * high-impedance state. Both actions are achieved by isolating
		 * the pin via the hardware tri-state registers.
		 */
		case PIN_CONFIG_BIAS_DISABLE:
		case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
			ret = mc33978_set_pull(mpc, pin, MC33978_TRISTATE);
			break;
		case PIN_CONFIG_DRIVE_STRENGTH_UA:
			arg /= 1000;
			fallthrough;
		case PIN_CONFIG_DRIVE_STRENGTH:
			ret = mc33978_set_ds(mpc, pin, arg);
			break;
		default:
			/*
			 * Required by the pinctrl core to safely fall back or
			 * skip unsupported configs. Do not use -EOPNOTSUPP.
			 */
			return -ENOTSUPP;
		}

		if (ret) {
			dev_err(mpc->dev, "Failed to set config param %04x for pin %u: %d\n",
				param, pin, ret);
			return ret;
		}
	}

	return 0;
}

static const struct pinconf_ops mc33978_pinconf_ops = {
	.pin_config_get = mc33978_pinconf_get,
	.pin_config_set = mc33978_pinconf_set,
	.is_generic = true,
};

static int mc33978_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	/* This chip is strictly an input device (comparators always active) */
	return 0;
}

/*
 * The hardware evaluates pin voltage against a threshold (default 4.0V)
 * and reports an abstract contact status (1 = closed, 0 = open):
 *
 * SG (Switch-to-Ground) topology (pull-up current source):
 * - Voltage > Threshold: Switch Open   (HW reports 0) -> Physical High
 * - Voltage < Threshold: Switch Closed (HW reports 1) -> Physical Low
 *
 * SB (Switch-to-Battery) topology (pull-down current source):
 * - Voltage > Threshold: Switch Closed (HW reports 1) -> Physical High
 * - Voltage < Threshold: Switch Open   (HW reports 0) -> Physical Low
 *
 * We translate this contact status back into physical voltage levels by
 * inverting the hardware status for all pins operating in SG topology.
 */
static int mc33978_read_in_state(struct mc33978_pinctrl *mpc,
				 unsigned long mask, unsigned long *state)
{
	u32 status, inv_mask;
	u32 config_reg = 0;
	int ret;

	ret = mc33978_read(mpc, MC33978_REG_READ_IN, &status);
	if (ret)
		return ret;

	/* Read CONFIG register only if the requested mask involves SP pins */
	if (mask & MC33978_SP_MASK) {
		ret = mc33978_read(mpc, MC33978_REG_CONFIG, &config_reg);
		if (ret)
			return ret;
	}

	/*
	 * Create an inversion mask for all pins currently operating in
	 * Switch-to-Ground (SG) topology. SG pins always have pull-ups.
	 * For SP pins, CONFIG bit 0 = Switch-to-Ground (PU),
	 * CONFIG bit 1 = Switch-to-Battery (PD).
	 */
	inv_mask = MC33978_SG_MASK |
		   (~(config_reg << MC33978_NUM_SG) & MC33978_SP_MASK);

	*state = (status ^ inv_mask) & mask;

	return 0;
}

static int mc33978_get(struct gpio_chip *chip, unsigned int offset)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);
	unsigned long state;
	int ret;

	guard(mutex)(&mpc->lock);

	ret = mc33978_read_in_state(mpc, BIT(offset), &state);
	if (ret)
		return ret;

	return !!(state & BIT(offset));
}

static int mc33978_get_multiple(struct gpio_chip *chip,
				unsigned long *mask, unsigned long *bits)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);
	unsigned long state;
	int ret;

	guard(mutex)(&mpc->lock);

	ret = mc33978_read_in_state(mpc, *mask, &state);
	if (ret)
		return ret;

	*bits = (*bits & ~*mask) | state;

	return 0;
}

/*
 * Emulate output states by routing or isolating active wetting currents.
 * To turn the line ON, we disable the hardware tri-state (write 0).
 * To turn the line OFF (High-Z), we enable tri-state (write 1).
 *
 * For Open-Source (Pull-Up): value=1 turns it ON, value=0 is High-Z.
 * For Open-Drain (Pull-Down): value=0 turns it ON, value=1 is High-Z.
 * We dynamically read the CONFIG register to determine the topology
 * and invert the bits accordingly for Open-Drain pins.
 *
 * Note: The hardware physically lacks push-pull drivers. Toggling outputs
 * via tri-state isolation may cause transient spikes.
 */
static int mc33978_update_tri_state(struct mc33978_pinctrl *mpc, u32 mask,
				    u32 bits)
{
	u32 sgmask = (mask & MC33978_SG_MASK) >> MC33978_SG_SHIFT;
	u32 sgbits = (bits & MC33978_SG_MASK) >> MC33978_SG_SHIFT;
	u32 spmask = (mask & MC33978_SP_MASK) >> MC33978_SP_SHIFT;
	u32 spbits = (bits & MC33978_SP_MASK) >> MC33978_SP_SHIFT;
	u32 config_reg = 0;
	int ret = 0;

	if (spmask) {
		/* Read topology: 1 = PD (Open-Drain), 0 = PU (Open-Source) */
		ret = mc33978_read(mpc, MC33978_REG_CONFIG, &config_reg);
		if (ret)
			return ret;

		/*
		 * Invert bits for Open-Drain (PD) pins.
		 * The Open-Drain API contract expects value=1 to be High-Z.
		 */
		spbits ^= (config_reg & spmask);

		ret = mc33978_update_bits(mpc, MC33978_REG_TRI_SP, spmask,
					  ~spbits);
		if (ret)
			return ret;
	}

	/* SG pins are always Pull-Up (Open-Source), no inversion needed */
	if (sgmask)
		ret = mc33978_update_bits(mpc, MC33978_REG_TRI_SG, sgmask,
					  ~sgbits);

	return ret;
}

static int mc33978_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);
	u32 mask = BIT(offset);
	u32 bits = value ? mask : 0;

	guard(mutex)(&mpc->lock);

	return mc33978_update_tri_state(mpc, mask, bits);
}

static int mc33978_set_multiple(struct gpio_chip *chip,
				unsigned long *mask, unsigned long *bits)
{
	struct mc33978_pinctrl *mpc = gpiochip_get_data(chip);

	guard(mutex)(&mpc->lock);

	return mc33978_update_tri_state(mpc, *mask, *bits);
}

static int mc33978_direction_output(struct gpio_chip *chip, unsigned int offset,
				    int value)
{
	return mc33978_set(chip, offset, value);
}

static int mc33978_gpio_child_to_parent_hwirq(struct gpio_chip *gc,
					      unsigned int child,
					      unsigned int child_type,
					      unsigned int *parent,
					      unsigned int *parent_type)
{
	*parent_type = child_type;
	*parent = child;

	return 0;
}

/*
 * Defensive wrappers for hierarchical IRQ proxying.
 *
 * gpiolib's hierarchical allocation exposes a lifecycle gap: the child
 * descriptor is registered before irq_domain_alloc_irqs_parent() fully
 * instantiates the parent chip.
 *
 * During consumer probe (e.g., gpiod_to_irq()), irq_create_fwspec_mapping()
 * allocates the hierarchy. As part of this, irq_domain_set_info() initializes
 * the top-level irq_desc and calls __irq_set_handler(). If the irq_desc
 * requires locking, __irq_get_desc_lock() will invoke the child's
 * .irq_bus_lock before the parent allocation is complete.
 *
 * Upstream generic helpers (e.g., irq_chip_mask_parent) blindly dereference
 * data->parent_data->chip, causing an immediate NULL pointer panic during
 * this gap. These wrappers check for a valid parent chip to safely drop
 * premature locking or masking events while the legacy subsystem hierarchy
 * is still assembling itself.
 */
static void mc33978_gpio_irq_mask(struct irq_data *data)
{
	struct irq_data *parent = data->parent_data;

	if (parent && parent->chip && parent->chip->irq_mask)
		parent->chip->irq_mask(parent);
}

static void mc33978_gpio_irq_unmask(struct irq_data *data)
{
	struct irq_data *parent = data->parent_data;

	if (parent && parent->chip && parent->chip->irq_unmask)
		parent->chip->irq_unmask(parent);
}

static int mc33978_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct irq_data *parent = data->parent_data;

	if (parent && parent->chip && parent->chip->irq_set_type)
		return parent->chip->irq_set_type(parent, type);

	return -EINVAL;
}

static void mc33978_gpio_irq_bus_lock(struct irq_data *data)
{
	struct irq_data *parent = data->parent_data;

	if (parent && parent->chip && parent->chip->irq_bus_lock)
		parent->chip->irq_bus_lock(parent);
}

static void mc33978_gpio_irq_bus_sync_unlock(struct irq_data *data)
{
	struct irq_data *parent = data->parent_data;

	if (parent && parent->chip && parent->chip->irq_bus_sync_unlock)
		parent->chip->irq_bus_sync_unlock(parent);
}

static const struct irq_chip mc33978_gpio_irqchip = {
	.name = "mc33978-gpio",
	.irq_mask = mc33978_gpio_irq_mask,
	.irq_unmask = mc33978_gpio_irq_unmask,
	.irq_set_type = mc33978_gpio_irq_set_type,
	.irq_bus_lock = mc33978_gpio_irq_bus_lock,
	.irq_bus_sync_unlock = mc33978_gpio_irq_bus_sync_unlock,
	.irq_set_wake = irq_chip_set_wake_parent,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void mc33978_init_gpio_chip(struct mc33978_pinctrl *mpc,
				   struct device *dev)
{
	struct gpio_irq_chip *girq;

	mpc->chip.label = dev_name(dev);
	mpc->chip.direction_input = mc33978_direction_input;
	mpc->chip.get = mc33978_get;
	mpc->chip.get_multiple = mc33978_get_multiple;
	mpc->chip.direction_output = mc33978_direction_output;
	mpc->chip.set = mc33978_set;
	mpc->chip.set_multiple = mc33978_set_multiple;
	mpc->chip.set_config = gpiochip_generic_config;

	mpc->chip.base = -1;
	mpc->chip.ngpio = MC33978_NGPIO;
	mpc->chip.can_sleep = true;
	mpc->chip.parent = dev;
	mpc->chip.owner = THIS_MODULE;

	girq = &mpc->chip.irq;
	gpio_irq_chip_set_chip(girq, &mc33978_gpio_irqchip);
	girq->fwnode = dev_fwnode(dev);
	girq->parent_domain = mpc->domain;
	girq->child_to_parent_hwirq = mc33978_gpio_child_to_parent_hwirq;
	girq->handler = handle_simple_irq;
	girq->default_type = IRQ_TYPE_NONE;
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
	int ret;

	device_set_node(dev, dev_fwnode(dev->parent));

	mpc = devm_kzalloc(dev, sizeof(*mpc), GFP_KERNEL);
	if (!mpc)
		return -ENOMEM;

	mpc->dev = dev;

	mpc->regmap = dev_get_regmap(dev->parent, NULL);
	if (!mpc->regmap)
		return dev_err_probe(dev, -ENODEV, "Failed to get parent regmap\n");

	mpc->domain = irq_find_matching_fwnode(dev_fwnode(dev->parent), DOMAIN_BUS_ANY);
	if (!mpc->domain)
		return dev_err_probe(dev, -ENODEV, "Failed to find parent IRQ domain\n");

	mutex_init(&mpc->lock);

	mc33978_init_gpio_chip(mpc, dev);
	mc33978_init_pinctrl_desc(mpc, dev);

	mpc->pctldev = devm_pinctrl_register(dev, &mpc->pinctrl_desc, mpc);
	if (IS_ERR(mpc->pctldev))
		return dev_err_probe(dev, PTR_ERR(mpc->pctldev),
				     "can't register pinctrl\n");

	ret = devm_gpiochip_add_data(dev, &mpc->chip, mpc);
	if (ret)
		return dev_err_probe(dev, ret, "can't add GPIO chip\n");

	ret = gpiochip_add_pin_range(&mpc->chip, dev_name(dev), 0, 0,
				     MC33978_NGPIO);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add pin range\n");

	platform_set_drvdata(pdev, mpc);

	return 0;
}

static const struct platform_device_id mc33978_pinctrl_id[] = {
	{ "mc33978-pinctrl", },
	{ "mc34978-pinctrl", },
	{ }
};
MODULE_DEVICE_TABLE(platform, mc33978_pinctrl_id);

static struct platform_driver mc33978_pinctrl_driver = {
	.driver = {
		.name = "mc33978-pinctrl",
	},
	.probe = mc33978_pinctrl_probe,
	.id_table = mc33978_pinctrl_id,
};
module_platform_driver(mc33978_pinctrl_driver);

MODULE_AUTHOR("David Jander <david@protonic.nl>");
MODULE_DESCRIPTION("NXP MC33978/MC34978 pinctrl driver");
MODULE_LICENSE("GPL");
