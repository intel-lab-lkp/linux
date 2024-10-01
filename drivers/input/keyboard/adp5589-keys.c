// SPDX-License-Identifier: GPL-2.0-only
/*
 * Description:  keypad driver for ADP5589, ADP5585
 *		 I2C QWERTY Keypad and IO Expander
 * Bugs: Enter bugs at http://blackfin.uclinux.org/
 *
 * Copyright (C) 2010-2011 Analog Devices Inc.
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/pm_wakeirq.h>
#include <linux/property.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/i2c.h>
#include <linux/gpio/driver.h>
#include <linux/slab.h>
#include <linux/units.h>

/*
 * ADP5589 specific GPI and Keymap defines
 */
#define ADP5589_KEYMAPSIZE		88

#define ADP5589_COL_SHIFT		8
#define ADP5589_MAX_ROW_NUM		7
#define ADP5589_MAX_COL_NUM		10
/*
 * ADP5585 specific GPI and Keymap defines
 */
#define ADP5585_KEYMAPSIZE		30

#define ADP5585_COL_SHIFT		6
#define ADP5585_MAX_ROW_NUM		5
#define ADP5585_MAX_COL_NUM		4

/* ADP5589/ADP5585 Common Registers */
#define ADP5589_5_ID			0x00
#define ADP5589_5_INT_STATUS		0x01
#define ADP5589_5_STATUS		0x02
#define ADP5589_5_FIFO_1		0x03
#define ADP5589_5_FIFO_2		0x04
#define ADP5589_5_FIFO_3		0x05
#define ADP5589_5_FIFO_4		0x06
#define ADP5589_5_FIFO_5		0x07
#define ADP5589_5_FIFO_6		0x08
#define ADP5589_5_FIFO_7		0x09
#define ADP5589_5_FIFO_8		0x0A
#define ADP5589_5_FIFO_9		0x0B
#define ADP5589_5_FIFO_10		0x0C
#define ADP5589_5_FIFO_11		0x0D
#define ADP5589_5_FIFO_12		0x0E
#define ADP5589_5_FIFO_13		0x0F
#define ADP5589_5_FIFO_14		0x10
#define ADP5589_5_FIFO_15		0x11
#define ADP5589_5_FIFO_16		0x12
#define ADP5589_5_GPI_INT_STAT_A	0x13
#define ADP5589_5_GPI_INT_STAT_B	0x14

/* ADP5589 Registers */
#define ADP5589_GPI_INT_STAT_C		0x15
#define ADP5589_GPI_STATUS_A		0x16
#define ADP5589_GPI_STATUS_B		0x17
#define ADP5589_GPI_STATUS_C		0x18
#define ADP5589_RPULL_CONFIG_A		0x19
#define ADP5589_RPULL_CONFIG_B		0x1A
#define ADP5589_RPULL_CONFIG_C		0x1B
#define ADP5589_RPULL_CONFIG_D		0x1C
#define ADP5589_RPULL_CONFIG_E		0x1D
#define ADP5589_GPI_INT_LEVEL_A		0x1E
#define ADP5589_GPI_INT_LEVEL_B		0x1F
#define ADP5589_GPI_INT_LEVEL_C		0x20
#define ADP5589_GPI_EVENT_EN_A		0x21
#define ADP5589_GPI_EVENT_EN_B		0x22
#define ADP5589_GPI_EVENT_EN_C		0x23
#define ADP5589_GPI_INTERRUPT_EN_A	0x24
#define ADP5589_GPI_INTERRUPT_EN_B	0x25
#define ADP5589_GPI_INTERRUPT_EN_C	0x26
#define ADP5589_DEBOUNCE_DIS_A		0x27
#define ADP5589_DEBOUNCE_DIS_B		0x28
#define ADP5589_DEBOUNCE_DIS_C		0x29
#define ADP5589_GPO_DATA_OUT_A		0x2A
#define ADP5589_GPO_DATA_OUT_B		0x2B
#define ADP5589_GPO_DATA_OUT_C		0x2C
#define ADP5589_GPO_OUT_MODE_A		0x2D
#define ADP5589_GPO_OUT_MODE_B		0x2E
#define ADP5589_GPO_OUT_MODE_C		0x2F
#define ADP5589_GPIO_DIRECTION_A	0x30
#define ADP5589_GPIO_DIRECTION_B	0x31
#define ADP5589_GPIO_DIRECTION_C	0x32
#define ADP5589_UNLOCK1			0x33
#define ADP5589_UNLOCK2			0x34
#define ADP5589_EXT_LOCK_EVENT		0x35
#define ADP5589_UNLOCK_TIMERS		0x36
#define ADP5589_LOCK_CFG		0x37
#define ADP5589_RESET1_EVENT_A		0x38
#define ADP5589_RESET1_EVENT_B		0x39
#define ADP5589_RESET1_EVENT_C		0x3A
#define ADP5589_RESET2_EVENT_A		0x3B
#define ADP5589_RESET2_EVENT_B		0x3C
#define ADP5589_RESET_CFG		0x3D
#define ADP5589_PWM_OFFT_LOW		0x3E
#define ADP5589_PWM_OFFT_HIGH		0x3F
#define ADP5589_PWM_ONT_LOW		0x40
#define ADP5589_PWM_ONT_HIGH		0x41
#define ADP5589_PWM_CFG			0x42
#define ADP5589_CLOCK_DIV_CFG		0x43
#define ADP5589_LOGIC_1_CFG		0x44
#define ADP5589_LOGIC_2_CFG		0x45
#define ADP5589_LOGIC_FF_CFG		0x46
#define ADP5589_LOGIC_INT_EVENT_EN	0x47
#define ADP5589_POLL_PTIME_CFG		0x48
#define ADP5589_PIN_CONFIG_A		0x49
#define ADP5589_PIN_CONFIG_B		0x4A
#define ADP5589_PIN_CONFIG_C		0x4B
#define ADP5589_PIN_CONFIG_D		0x4C
#define ADP5589_GENERAL_CFG		0x4D
#define ADP5589_INT_EN			0x4E

/* ADP5585 Registers */
#define ADP5585_GPI_STATUS_A		0x15
#define ADP5585_GPI_STATUS_B		0x16
#define ADP5585_RPULL_CONFIG_A		0x17
#define ADP5585_RPULL_CONFIG_B		0x18
#define ADP5585_RPULL_CONFIG_C		0x19
#define ADP5585_RPULL_CONFIG_D		0x1A
#define ADP5585_GPI_INT_LEVEL_A		0x1B
#define ADP5585_GPI_INT_LEVEL_B		0x1C
#define ADP5585_GPI_EVENT_EN_A		0x1D
#define ADP5585_GPI_EVENT_EN_B		0x1E
#define ADP5585_GPI_INTERRUPT_EN_A	0x1F
#define ADP5585_GPI_INTERRUPT_EN_B	0x20
#define ADP5585_DEBOUNCE_DIS_A		0x21
#define ADP5585_DEBOUNCE_DIS_B		0x22
#define ADP5585_GPO_DATA_OUT_A		0x23
#define ADP5585_GPO_DATA_OUT_B		0x24
#define ADP5585_GPO_OUT_MODE_A		0x25
#define ADP5585_GPO_OUT_MODE_B		0x26
#define ADP5585_GPIO_DIRECTION_A	0x27
#define ADP5585_GPIO_DIRECTION_B	0x28
#define ADP5585_RESET1_EVENT_A		0x29
#define ADP5585_RESET1_EVENT_B		0x2A
#define ADP5585_RESET1_EVENT_C		0x2B
#define ADP5585_RESET2_EVENT_A		0x2C
#define ADP5585_RESET2_EVENT_B		0x2D
#define ADP5585_RESET_CFG		0x2E
#define ADP5585_PWM_OFFT_LOW		0x2F
#define ADP5585_PWM_OFFT_HIGH		0x30
#define ADP5585_PWM_ONT_LOW		0x31
#define ADP5585_PWM_ONT_HIGH		0x32
#define ADP5585_PWM_CFG			0x33
#define ADP5585_LOGIC_CFG		0x34
#define ADP5585_LOGIC_FF_CFG		0x35
#define ADP5585_LOGIC_INT_EVENT_EN	0x36
#define ADP5585_POLL_PTIME_CFG		0x37
#define ADP5585_PIN_CONFIG_A		0x38
#define ADP5585_PIN_CONFIG_B		0x39
#define ADP5585_PIN_CONFIG_D		0x3A
#define ADP5585_GENERAL_CFG		0x3B
#define ADP5585_INT_EN			0x3C

/* ID Register */
#define ADP5589_5_DEVICE_ID_MASK	0xF
#define ADP5589_5_MAN_ID_MASK		0xF
#define ADP5589_5_MAN_ID_SHIFT		4
#define ADP5589_5_MAN_ID		0x02

/* GENERAL_CFG Register */
#define OSC_EN		BIT(7)
#define CORE_CLK(x)	(((x) & 0x3) << 5)
#define LCK_TRK_LOGIC	BIT(4)		/* ADP5589 only */
#define LCK_TRK_GPI	BIT(3)		/* ADP5589 only */
#define INT_CFG		BIT(1)
#define RST_CFG		BIT(0)

/* INT_EN Register */
#define LOGIC2_IEN	BIT(5)		/* ADP5589 only */
#define LOGIC1_IEN	BIT(4)
#define LOCK_IEN	BIT(3)		/* ADP5589 only */
#define OVRFLOW_IEN	BIT(2)
#define GPI_IEN		BIT(1)
#define EVENT_IEN	BIT(0)

/* Interrupt Status Register */
#define LOGIC2_INT	BIT(5)		/* ADP5589 only */
#define LOGIC1_INT	BIT(4)
#define LOCK_INT	BIT(3)		/* ADP5589 only */
#define OVRFLOW_INT	BIT(2)
#define GPI_INT		BIT(1)
#define EVENT_INT	BIT(0)

/* STATUS Register */
#define LOGIC2_STAT	BIT(7)		/* ADP5589 only */
#define LOGIC1_STAT	BIT(6)
#define LOCK_STAT	BIT(5)		/* ADP5589 only */
#define KEC		0x1F

/* PIN_CONFIG_D Register */
#define C4_EXTEND_CFG	BIT(6)		/* RESET2 */
#define R4_EXTEND_CFG	BIT(5)		/* RESET1 */

#define RESET1_POL		BIT(7)
#define RESET2_POL		BIT(6)
#define RST_PASSTHRU_EN		BIT(5)
#define RESET_TRIGGER_TIME	GENMASK(4, 2)
#define RESET_PULSE_WIDTH	GENMASK(1, 0)

/* LOCK_CFG */
#define LOCK_EN		BIT(0)

#define PTIME_MASK	0x3
#define LTIME_MASK	0x3		/* ADP5589 only */

/* Key Event Register xy */
#define KEY_EV_PRESSED	BIT(7)
#define KEY_EV_MASK	0x7F

#define KEYP_MAX_EVENT		16
#define ADP5589_MAXGPIO		19
#define ADP5585_MAXGPIO		11 /* 10 on the ADP5585-01, 11 on ADP5585-02 */

/* As needed for the matrix parsing code */
#define ADP5589_MAX_KEYMAPSIZE	123

#define ADP5589_MAX_UNLOCK_TIME_SEC	7

struct adp5589_info {
	u8 rpull_banks;
	u8 c4_extend_cfg;
	u8 maxgpio;
	u8 keymapsize;
	u8 gpi_pin_base;
	u8 gpi_pin_end;
	u8 max_row_num;
	u8 max_col_num;
	u8 col_shift;
	bool support_row5;
	bool is_adp5585;
	u8 (*bank)(u8 offset);
	u8 (*bit)(u8 offset);
	u8 (*reg)(u8 reg);
};

struct adp5589_kpad {
	struct i2c_client *client;
	struct input_dev *input;
	const struct adp5589_info *info;
	unsigned short keycode[ADP5589_MAX_KEYMAPSIZE];
	const struct adp5589_gpi_map *gpimap;
	unsigned long pull_up_100k_map;
	u32 keypad_en_mask;
	u32 key_poll_time;
	u32 row_shift;
	u32 unlock_time;
	u32 unlock_keys[2];
	u32 nkeys_unlock;
	u32 reset1_keys[3];
	u32 nkeys_reset1;
	u32 reset2_keys[2];
	u32 nkeys_reset2;
	unsigned short gpimapsize;
	unsigned int extend_cfg;
	unsigned char gpiomap[ADP5589_MAXGPIO];
	struct gpio_chip gc;
	struct mutex gpio_lock;	/* Protect cached dir, dat_out */
	u8 reset_cfg;
	u8 rpull[5];
	u8 debounce_dis[3];
	u8 dat_out[3];
	u8 dir[3];
	u8 int_en[3];
	u8 irq_mask[3];
};

/*
 *  ADP5589 / ADP5585 derivative / variant handling
 */


/* ADP5589 */

static unsigned char adp5589_bank(unsigned char offset)
{
	return offset >> 3;
}

static unsigned char adp5589_bit(unsigned char offset)
{
	return 1u << (offset & 0x7);
}

static unsigned char adp5589_reg(unsigned char reg)
{
	return reg;
}

/* ADP5585 */

static unsigned char adp5585_bank(unsigned char offset)
{
	return offset > ADP5585_MAX_ROW_NUM;
}

static unsigned char adp5585_bit(unsigned char offset)
{
	return (offset > ADP5585_MAX_ROW_NUM) ?
		1u << (offset - ADP5585_COL_SHIFT) : 1u << offset;
}

static const unsigned char adp5585_reg_lut[] = {
	[ADP5589_GPI_STATUS_A]		= ADP5585_GPI_STATUS_A,
	[ADP5589_GPI_STATUS_B]		= ADP5585_GPI_STATUS_B,
	[ADP5589_RPULL_CONFIG_A]	= ADP5585_RPULL_CONFIG_A,
	[ADP5589_RPULL_CONFIG_B]	= ADP5585_RPULL_CONFIG_B,
	[ADP5589_RPULL_CONFIG_C]	= ADP5585_RPULL_CONFIG_C,
	[ADP5589_RPULL_CONFIG_D]	= ADP5585_RPULL_CONFIG_D,
	[ADP5589_GPI_INT_LEVEL_A]	= ADP5585_GPI_INT_LEVEL_A,
	[ADP5589_GPI_INT_LEVEL_B]	= ADP5585_GPI_INT_LEVEL_B,
	[ADP5589_GPI_EVENT_EN_A]	= ADP5585_GPI_EVENT_EN_A,
	[ADP5589_GPI_EVENT_EN_B]	= ADP5585_GPI_EVENT_EN_B,
	[ADP5589_GPI_INTERRUPT_EN_A]	= ADP5585_GPI_INTERRUPT_EN_A,
	[ADP5589_GPI_INTERRUPT_EN_B]	= ADP5585_GPI_INTERRUPT_EN_B,
	[ADP5589_DEBOUNCE_DIS_A]	= ADP5585_DEBOUNCE_DIS_A,
	[ADP5589_DEBOUNCE_DIS_B]	= ADP5585_DEBOUNCE_DIS_B,
	[ADP5589_GPO_DATA_OUT_A]	= ADP5585_GPO_DATA_OUT_A,
	[ADP5589_GPO_DATA_OUT_B]	= ADP5585_GPO_DATA_OUT_B,
	[ADP5589_GPO_OUT_MODE_A]	= ADP5585_GPO_OUT_MODE_A,
	[ADP5589_GPO_OUT_MODE_B]	= ADP5585_GPO_OUT_MODE_B,
	[ADP5589_GPIO_DIRECTION_A]	= ADP5585_GPIO_DIRECTION_A,
	[ADP5589_GPIO_DIRECTION_B]	= ADP5585_GPIO_DIRECTION_B,
	[ADP5589_RESET1_EVENT_A]	= ADP5585_RESET1_EVENT_A,
	[ADP5589_RESET1_EVENT_B]	= ADP5585_RESET1_EVENT_B,
	[ADP5589_RESET1_EVENT_C]	= ADP5585_RESET1_EVENT_C,
	[ADP5589_RESET2_EVENT_A]	= ADP5585_RESET2_EVENT_A,
	[ADP5589_RESET2_EVENT_B]	= ADP5585_RESET2_EVENT_B,
	[ADP5589_RESET_CFG]		= ADP5585_RESET_CFG,
	[ADP5589_PWM_OFFT_LOW]		= ADP5585_PWM_OFFT_LOW,
	[ADP5589_PWM_OFFT_HIGH]		= ADP5585_PWM_OFFT_HIGH,
	[ADP5589_PWM_ONT_LOW]		= ADP5585_PWM_ONT_LOW,
	[ADP5589_PWM_ONT_HIGH]		= ADP5585_PWM_ONT_HIGH,
	[ADP5589_PWM_CFG]		= ADP5585_PWM_CFG,
	[ADP5589_LOGIC_1_CFG]		= ADP5585_LOGIC_CFG,
	[ADP5589_LOGIC_FF_CFG]		= ADP5585_LOGIC_FF_CFG,
	[ADP5589_LOGIC_INT_EVENT_EN]	= ADP5585_LOGIC_INT_EVENT_EN,
	[ADP5589_POLL_PTIME_CFG]	= ADP5585_POLL_PTIME_CFG,
	[ADP5589_PIN_CONFIG_A]		= ADP5585_PIN_CONFIG_A,
	[ADP5589_PIN_CONFIG_B]		= ADP5585_PIN_CONFIG_B,
	[ADP5589_PIN_CONFIG_D]		= ADP5585_PIN_CONFIG_D,
	[ADP5589_GENERAL_CFG]		= ADP5585_GENERAL_CFG,
	[ADP5589_INT_EN]		= ADP5585_INT_EN,
};

static unsigned char adp5585_reg(unsigned char reg)
{
	return adp5585_reg_lut[reg];
}

static int adp5589_read(struct i2c_client *client, u8 reg, u8 *val)
{
	int ret = i2c_smbus_read_byte_data(client, reg);

	if (ret < 0) {
		dev_err(&client->dev, "Read Error\n");
		return ret;
	}

	*val = ret;
	return 0;
}

static int adp5589_write(struct i2c_client *client, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(client, reg, val);
}

static int adp5589_gpio_get_value(struct gpio_chip *chip, unsigned int off)
{
	struct adp5589_kpad *kpad = gpiochip_get_data(chip);
	unsigned int bank = kpad->info->bank(kpad->gpiomap[off]);
	unsigned int bit = kpad->info->bit(kpad->gpiomap[off]);
	int error;
	u8 val;

	guard(mutex)(&kpad->gpio_lock);
	if (kpad->dir[bank] & bit)
		return !!(kpad->dat_out[bank] & bit);

	error = adp5589_read(kpad->client,
			     kpad->info->reg(ADP5589_GPI_STATUS_A) + bank,
			     &val);
	if (error)
		return error;

	return !!(val & bit);
}

static void adp5589_gpio_set_value(struct gpio_chip *chip,
				   unsigned int off, int val)
{
	struct adp5589_kpad *kpad = gpiochip_get_data(chip);
	unsigned int bank = kpad->info->bank(kpad->gpiomap[off]);
	unsigned int bit = kpad->info->bit(kpad->gpiomap[off]);

	guard(mutex)(&kpad->gpio_lock);

	if (val)
		kpad->dat_out[bank] |= bit;
	else
		kpad->dat_out[bank] &= ~bit;

	adp5589_write(kpad->client, kpad->info->reg(ADP5589_GPO_DATA_OUT_A) +
		      bank, kpad->dat_out[bank]);
}

static int adp5589_gpio_direction_input(struct gpio_chip *chip,
					unsigned int off)
{
	struct adp5589_kpad *kpad = gpiochip_get_data(chip);
	unsigned int bank = kpad->info->bank(kpad->gpiomap[off]);
	unsigned int bit = kpad->info->bit(kpad->gpiomap[off]);

	guard(mutex)(&kpad->gpio_lock);

	kpad->dir[bank] &= ~bit;
	return adp5589_write(kpad->client,
			     kpad->info->reg(ADP5589_GPIO_DIRECTION_A) + bank,
			     kpad->dir[bank]);
}

static int adp5589_gpio_direction_output(struct gpio_chip *chip,
					 unsigned int off, int val)
{
	struct adp5589_kpad *kpad = gpiochip_get_data(chip);
	unsigned int bank = kpad->info->bank(kpad->gpiomap[off]);
	unsigned int bit = kpad->info->bit(kpad->gpiomap[off]);
	int ret;

	guard(mutex)(&kpad->gpio_lock);

	kpad->dir[bank] |= bit;

	if (val)
		kpad->dat_out[bank] |= bit;
	else
		kpad->dat_out[bank] &= ~bit;

	ret = adp5589_write(kpad->client, kpad->info->reg(ADP5589_GPO_DATA_OUT_A)
			    + bank, kpad->dat_out[bank]);
	if (ret)
		return ret;

	return adp5589_write(kpad->client,
			     kpad->info->reg(ADP5589_GPIO_DIRECTION_A) + bank,
			     kpad->dir[bank]);
}

static const u8 adp5589_rpull_masks[] = {
	GENMASK(1, 0),
	GENMASK(3, 2),
	GENMASK(5, 4),
	GENMASK(7, 6)
};

static int adp5589_gpio_set_bias(struct adp5589_kpad *kpad, unsigned int pin,
				 enum pin_config_param cfg)
{
	unsigned int bank, msk;
	u8 val;

	guard(mutex)(&kpad->gpio_lock);

	if (cfg == PIN_CONFIG_BIAS_PULL_UP)
		val = test_bit(pin, &kpad->pull_up_100k_map) ? 2 : 0;
	else if (cfg == PIN_CONFIG_BIAS_PULL_DOWN)
		val = 1;
	else
		val = 3;

	/*
	 * For the BIAS configs, we have 4 pins per register. We need to take
	 * care for the adp5585 variant where we have a hole in the registers
	 * between rows and columns. Hence, we detect when a pin is a column and
	 * apply the proper offset and pin normalization.
	 */
	if (kpad->info->is_adp5585 && pin >= kpad->info->col_shift) {
		bank = 2 + (pin - kpad->info->col_shift) / 4;
		msk = adp5589_rpull_masks[(pin - kpad->info->col_shift) % 4];
	} else {
		bank = pin / 4;
		msk = adp5589_rpull_masks[pin % 4];
	}

	val <<= __bf_shf(msk);
	kpad->rpull[bank] = (kpad->rpull[bank] & ~msk) | (val & msk);

	return adp5589_write(kpad->client,
			     kpad->info->reg(ADP5589_RPULL_CONFIG_A) + bank,
			     kpad->rpull[bank]);
}

static int adp5589_gpio_set_config(struct gpio_chip *chip,  unsigned int off,
				   unsigned long config)
{
	struct adp5589_kpad *kpad = gpiochip_get_data(chip);
	unsigned int bank, bit, reg, pin = kpad->gpiomap[off];
	unsigned int cfg = pinconf_to_config_param(config);
	unsigned int val;
	int error;

	switch (cfg) {
	case PIN_CONFIG_BIAS_PULL_UP:
		fallthrough;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		fallthrough;
	case PIN_CONFIG_BIAS_DISABLE:
		return adp5589_gpio_set_bias(kpad, pin, cfg);
	case PIN_CONFIG_INPUT_DEBOUNCE:
		bank = kpad->info->bank(pin);
		bit = kpad->info->bit(pin);
		reg = kpad->info->reg(ADP5589_DEBOUNCE_DIS_A) + bank;

		val = pinconf_to_config_argument(config);
		if (val && val != 50) {
			dev_err(&kpad->client->dev, "Debounce needs to be 50us(%u)\n",
				val);
			return -EINVAL;
		}

		scoped_guard(mutex, &kpad->gpio_lock) {
			if (!val)
				kpad->debounce_dis[bank] |= bit;
			else
				kpad->debounce_dis[bank] &= bit;

			error = adp5589_write(kpad->client, reg,
					      kpad->debounce_dis[bank]);
		}
		return error;
	default:
		return -EOPNOTSUPP;
	}
}

static int adp5589_build_gpiomap(struct adp5589_kpad *kpad)
{
	bool pin_used[ADP5589_MAXGPIO];
	int n_unused = 0;
	int i;

	memset(pin_used, false, sizeof(pin_used));

	for (i = 0; i < kpad->info->maxgpio; i++)
		if (kpad->keypad_en_mask & BIT(i))
			pin_used[i] = true;

	if (kpad->extend_cfg & R4_EXTEND_CFG)
		pin_used[4] = true;

	if (kpad->extend_cfg & C4_EXTEND_CFG)
		pin_used[kpad->info->c4_extend_cfg] = true;

	if (!kpad->info->support_row5)
		pin_used[5] = true;

	for (i = 0; i < kpad->info->maxgpio; i++)
		if (!pin_used[i])
			kpad->gpiomap[n_unused++] = i;

	return n_unused;
}

static void adp5589_irq_bus_lock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adp5589_kpad *kpad = gpiochip_get_data(gc);

	mutex_lock(&kpad->gpio_lock);
}

static void adp5589_irq_bus_sync_unlock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adp5589_kpad *kpad = gpiochip_get_data(gc);
	int i;

	for (i = 0; i <= kpad->info->bank(kpad->info->maxgpio); i++) {
		if (kpad->int_en[i] ^ kpad->irq_mask[i]) {
			kpad->int_en[i] = kpad->irq_mask[i];
			adp5589_write(kpad->client,
				      kpad->info->reg(ADP5589_GPI_EVENT_EN_A) + i,
				      kpad->int_en[i]);
		}
	}

	mutex_unlock(&kpad->gpio_lock);
}

static void adp5589_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adp5589_kpad *kpad = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	unsigned long real_irq = kpad->gpiomap[hwirq];
	unsigned int bank = kpad->info->bank(real_irq);

	kpad->irq_mask[bank] &= ~kpad->info->bit(real_irq);
	gpiochip_disable_irq(gc, hwirq);
}

static void adp5589_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adp5589_kpad *kpad = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	unsigned long real_irq = kpad->gpiomap[hwirq];
	unsigned int bank = kpad->info->bank(real_irq);

	gpiochip_enable_irq(gc, hwirq);
	kpad->irq_mask[bank] |= kpad->info->bit(real_irq);
}

static int adp5589_irq_set_type(struct irq_data *d, unsigned int type)
{
	if (!(type & IRQ_TYPE_EDGE_BOTH))
		return -EINVAL;

	irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static const struct irq_chip adp5589_irq_chip = {
	.name = "adp5589",
	.irq_mask = adp5589_irq_mask,
	.irq_unmask = adp5589_irq_unmask,
	.irq_bus_lock = adp5589_irq_bus_lock,
	.irq_bus_sync_unlock = adp5589_irq_bus_sync_unlock,
	.irq_set_type = adp5589_irq_set_type,
	.flags = IRQCHIP_SKIP_SET_WAKE | IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int adp5589_gpio_add(struct adp5589_kpad *kpad)
{
	struct device *dev = &kpad->client->dev;
	struct gpio_irq_chip *girq;
	int i, error;

	kpad->gc.parent = dev;
	kpad->gc.ngpio = adp5589_build_gpiomap(kpad);
	if (kpad->gc.ngpio == 0) {
		dev_info(dev, "No unused gpios left to export\n");
		return 0;
	}

	kpad->gc.direction_input = adp5589_gpio_direction_input;
	kpad->gc.direction_output = adp5589_gpio_direction_output;
	kpad->gc.get = adp5589_gpio_get_value;
	kpad->gc.set = adp5589_gpio_set_value;
	kpad->gc.set_config = adp5589_gpio_set_config;
	kpad->gc.can_sleep = 1;

	kpad->gc.base = -1;
	kpad->gc.label = kpad->client->name;
	kpad->gc.owner = THIS_MODULE;

	if (device_property_present(dev, "interrupt-controller")) {
		if (!kpad->client->irq) {
			dev_err(dev, "Unable to serve as interrupt controller without IRQ\n");
			return -EINVAL;
		}

		girq = &kpad->gc.irq;
		gpio_irq_chip_set_chip(girq, &adp5589_irq_chip);
		girq->handler = handle_bad_irq;
		girq->threaded = true;
	}

	mutex_init(&kpad->gpio_lock);

	error = devm_gpiochip_add_data(dev, &kpad->gc, kpad);
	if (error)
		return error;

	for (i = 0; i <= kpad->info->bank(kpad->info->maxgpio); i++) {
		error = adp5589_read(kpad->client,
				     kpad->info->reg(ADP5589_GPO_DATA_OUT_A) + i,
				     &kpad->dat_out[i]);
		if (error)
			return error;

		error = adp5589_read(kpad->client,
				     kpad->info->reg(ADP5589_GPIO_DIRECTION_A) + i,
				     &kpad->dir[i]);
		if (error)
			return error;

		error = adp5589_read(kpad->client,
				     kpad->info->reg(ADP5589_DEBOUNCE_DIS_A) + i,
				     &kpad->debounce_dis[i]);
		if (error)
			return error;
	}

	for (i = 0; i < kpad->info->rpull_banks; i++) {
		error = adp5589_read(kpad->client,
				     kpad->info->reg(ADP5589_RPULL_CONFIG_A) + i,
				     &kpad->rpull[i]);
		if (error)
			return error;
	}

	return 0;
}

static unsigned long adp5589_gpiomap_get_hwirq(struct device *dev,
					       const u8 *map, unsigned int gpio,
					       unsigned int ngpios)
{
	unsigned int hwirq;

	for (hwirq = 0; hwirq < ngpios; hwirq++)
		if (map[hwirq] == gpio)
			return hwirq;

	/* should never happen */
	dev_warn_ratelimited(dev, "could not find the hwirq for gpio(%u)\n", gpio);

	return INVALID_HWIRQ;
}

static void adp5589_gpio_irq_handle(struct adp5589_kpad *kpad, int key_val,
				    int key_press)
{
	unsigned int irq, gpio = key_val - kpad->info->gpi_pin_base, irq_type;
	struct i2c_client *client = kpad->client;
	struct irq_data *irqd;
	unsigned long hwirq;

	hwirq = adp5589_gpiomap_get_hwirq(&client->dev, kpad->gpiomap,
					  gpio, kpad->gc.ngpio);
	if (hwirq == INVALID_HWIRQ) {
		dev_err(&client->dev, "Could not get hwirq for key(%u)\n", key_val);
		return;
	}

	irq = irq_find_mapping(kpad->gc.irq.domain, hwirq);
	if (!irq)
		return;

	irqd = irq_get_irq_data(irq);
	if (!irqd) {
		dev_err(&client->dev, "Could not get irq(%u) data\n", irq);
		return;
	}

	irq_type = irqd_get_trigger_type(irqd);

	/*
	 * Default is active low which means key_press is asserted on
	 * the falling edge.
	 */
	if ((irq_type & IRQ_TYPE_EDGE_RISING && !key_press) ||
	    (irq_type & IRQ_TYPE_EDGE_FALLING && key_press))
		handle_nested_irq(irq);
}

static void adp5589_report_events(struct adp5589_kpad *kpad, int ev_cnt)
{
	int i;

	for (i = 0; i < ev_cnt; i++) {
		u8 key, key_val, key_press;
		int error;

		error = adp5589_read(kpad->client, ADP5589_5_FIFO_1 + i, &key);
		if (error)
			return;

		key_val = key & KEY_EV_MASK;
		key_press = key & KEY_EV_PRESSED;

		if (key_val >= kpad->info->gpi_pin_base &&
		    key_val <= kpad->info->gpi_pin_end) {
			/* gpio line used as IRQ source */
			adp5589_gpio_irq_handle(kpad, key_val, key_press);
		} else {
			int row = (key_val - 1) / (kpad->info->max_col_num + 1);
			int col = (key_val - 1) % (kpad->info->max_col_num + 1);
			int code = MATRIX_SCAN_CODE(row, col, kpad->row_shift);

			dev_dbg_ratelimited(&kpad->client->dev,
					    "report key(%d) r(%d) c(%d) code(%d)\n",
					    key_val, row, col, kpad->keycode[code]);

			input_report_key(kpad->input,
					 kpad->keycode[code], key_press);
		}
	}
}

static irqreturn_t adp5589_irq(int irq, void *handle)
{
	struct adp5589_kpad *kpad = handle;
	struct i2c_client *client = kpad->client;
	u8 status, ev_cnt;
	int error;

	error = adp5589_read(client, ADP5589_5_INT_STATUS, &status);
	if (error)
		return IRQ_HANDLED;

	if (status & OVRFLOW_INT)	/* Unlikely and should never happen */
		dev_err(&client->dev, "Event Overflow Error\n");

	if (status & EVENT_INT) {
		error = adp5589_read(client, ADP5589_5_STATUS, &ev_cnt);
		if (error || !(ev_cnt & KEC))
			goto out_irq;

		adp5589_report_events(kpad, ev_cnt & KEC);
		input_sync(kpad->input);
	}

out_irq:
	adp5589_write(client, ADP5589_5_INT_STATUS, status); /* Status is W1C */

	return IRQ_HANDLED;
}

static int adp5589_setup(struct adp5589_kpad *kpad)
{
	struct i2c_client *client = kpad->client;
	u8 (*reg)(u8) = kpad->info->reg;
	int i, ret;
	u8 dummy;

	ret = adp5589_write(client, reg(ADP5589_PIN_CONFIG_A),
			    kpad->keypad_en_mask);
	if (ret)
		return ret;

	ret = adp5589_write(client, reg(ADP5589_PIN_CONFIG_B),
			    kpad->keypad_en_mask >> kpad->info->col_shift);
	if (ret)
		return ret;

	if (!kpad->info->is_adp5585) {
		ret = adp5589_write(client, ADP5589_PIN_CONFIG_C,
				    kpad->keypad_en_mask >> 16);
		if (ret)
			return ret;
	}

	/* unlock keys */
	for (i = 0; i < kpad->nkeys_unlock; i++) {
		ret = adp5589_write(client, ADP5589_UNLOCK1 + i,
				    kpad->unlock_keys[i] | KEY_EV_PRESSED);
		if (ret)
			return ret;
	}

	if (kpad->nkeys_unlock) {
		ret = adp5589_write(client, ADP5589_UNLOCK_TIMERS,
				    kpad->unlock_time);
		if (ret)
			return ret;

		ret = adp5589_write(client, ADP5589_LOCK_CFG, LOCK_EN);
		if (ret)
			return ret;
	}

	for (i = 0; i < KEYP_MAX_EVENT; i++) {
		ret = adp5589_read(client, ADP5589_5_FIFO_1 + i, &dummy);
		if (ret)
			return ret;
	}

	/* reset keys */
	for (i = 0; i < kpad->nkeys_reset1; i++) {
		ret = adp5589_write(client, reg(ADP5589_RESET1_EVENT_A + i),
				    kpad->reset1_keys[i] | KEY_EV_PRESSED);
		if (ret)
			return ret;
	}

	for (i = 0; i < kpad->nkeys_reset2; i++) {
		ret = adp5589_write(client, reg(ADP5589_RESET2_EVENT_A + i),
				    kpad->reset2_keys[i] | KEY_EV_PRESSED);
		if (ret)
			return ret;
	}

	if (kpad->extend_cfg) {
		ret = adp5589_write(client, reg(ADP5589_RESET_CFG),
				    kpad->reset_cfg);
		if (ret)
			return ret;

		ret = adp5589_write(client, reg(ADP5589_PIN_CONFIG_D),
				    kpad->extend_cfg);
		if (ret)
			return ret;
	}

	ret = adp5589_write(client, reg(ADP5589_POLL_PTIME_CFG),
			    kpad->key_poll_time);
	if (ret)
		return ret;

	ret = adp5589_write(client, ADP5589_5_INT_STATUS,
			    (kpad->info->is_adp5585 ? 0 : LOGIC2_INT) |
			    LOGIC1_INT | OVRFLOW_INT |
			    (kpad->info->is_adp5585 ? 0 : LOCK_INT) |
			    GPI_INT | EVENT_INT);	/* Status is W1C */
	if (ret)
		return ret;

	ret = adp5589_write(client, reg(ADP5589_GENERAL_CFG),
			    INT_CFG | OSC_EN | CORE_CLK(3));
	if (ret)
		return ret;

	ret = adp5589_write(client, reg(ADP5589_INT_EN),
			    OVRFLOW_IEN | GPI_IEN | EVENT_IEN);
	if (ret) {
		dev_err(&client->dev, "Write Error\n");
		return ret;
	}

	return 0;
}

static int adp5589_validate_key(struct adp5589_kpad *kpad, u32 key, bool is_gpi)
{
	struct i2c_client *client = kpad->client;
	u32 row, col;

	if (is_gpi) {
		u32 gpi = key - kpad->info->gpi_pin_base;

		if (gpi == 5 && !kpad->info->support_row5) {
			dev_err(&client->dev,
				"Invalid unlock/reset GPI(%u) not supported\n",
				gpi);
			return -EINVAL;
		}

		/* check if it's being used in the keypad */
		if (BIT(gpi) & kpad->keypad_en_mask) {
			dev_err(&client->dev,
				"Invalid unlock/reset GPI(%u) being used in the keypad(%x)\n",
				gpi, kpad->keypad_en_mask);
			return -EINVAL;
		}

		return 0;
	}

	row = (key - 1) / (kpad->info->max_col_num + 1);
	col = (key - 1) % (kpad->info->max_col_num + 1);

	/* both the row and col must be part of the keypad */
	if (BIT(row) & kpad->keypad_en_mask &&
	    BIT(col) << kpad->info->col_shift & kpad->keypad_en_mask)
		return 0;

	dev_err(&client->dev, "Invalid unlock/reset key(%u) not used in the keypad(%x)\n",
		key, kpad->keypad_en_mask);

	return -EINVAL;
}

static int adp5589_parse_key_array(struct adp5589_kpad *kpad, const char *prop,
				   u32 *keys, u32 *n_keys, u32 max_keys,
				   bool reset_key)
{
	struct i2c_client *client = kpad->client;
	unsigned int key, max_keypad;
	int ret;

	ret = device_property_count_u32(&client->dev, prop);
	if (ret < 0)
		return 0;

	*n_keys = ret;

	if (kpad->info->is_adp5585 && !reset_key) {
		dev_err(&client->dev, "Unlock keys not supported for adp5585\n");
		return -EOPNOTSUPP;
	}

	if (*n_keys  > max_keys) {
		dev_err(&client->dev, "Invalid number of keys(%u > %u) for %s\n",
			*n_keys, max_keys, prop);
		return -EINVAL;
	}

	ret = device_property_read_u32_array(&client->dev, prop, keys, *n_keys);
	if (ret)
		return ret;

	max_keypad = (kpad->info->max_row_num + 1) * (kpad->info->max_col_num + 1);

	for (key = 0; key < *n_keys; key++) {
		/* part of the keypad... */
		if (in_range(keys[key], 1, max_keypad)) {
			/* is it part of the keypad?! */
			ret = adp5589_validate_key(kpad, keys[key], false);
			if (ret)
				return ret;

			continue;
		}

		/* part of gpio-keys... */
		if (in_range(keys[key], kpad->info->gpi_pin_base,
			     kpad->info->maxgpio)) {
			/* is the GPI being used as part of the keypad?! */
			ret = adp5589_validate_key(kpad, keys[key], true);
			if (ret)
				return ret;

			continue;
		}

		/* wildcard unlock event... */
		if (!reset_key && keys[key] == 127)
			continue;

		dev_err(&client->dev, "Invalid key(%u) for %s\n", keys[key],
			prop);

		return -EINVAL;
	}

	return 0;
}

static int adp5589_unlock_parse(struct adp5589_kpad *kpad)
{
	struct i2c_client *client = kpad->client;
	int error;

	error = adp5589_parse_key_array(kpad, "adi,unlock-keys",
					kpad->unlock_keys, &kpad->nkeys_unlock,
					ARRAY_SIZE(kpad->unlock_keys), false);
	if (error)
		return error;
	if (!kpad->nkeys_unlock)
		/* no unlock keys */
		return 0;

	error = device_property_read_u32(&client->dev, "adi,unlock-trigger-sec",
					 &kpad->unlock_time);
	if (!error) {
		if (kpad->unlock_time > ADP5589_MAX_UNLOCK_TIME_SEC) {
			dev_err(&client->dev, "Invalid unlock time(%u > %d)\n",
				kpad->unlock_time, ADP5589_MAX_UNLOCK_TIME_SEC);
			return -EINVAL;
		}
	}

	return 0;
}

static int adp5589_reset_parse(struct adp5589_kpad *kpad)
{
	struct i2c_client *client = kpad->client;
	u32 prop_val;
	int error;

	error = adp5589_parse_key_array(kpad, "adi,reset1-keys",
					kpad->reset1_keys, &kpad->nkeys_reset1,
					ARRAY_SIZE(kpad->reset1_keys), true);
	if (error)
		return error;
	if (kpad->nkeys_reset1 > 0) {
		/*
		 * Then R4 is used as reset output. Make sure it's not being used
		 * in the keypad.
		 */
		if (BIT(4) & kpad->keypad_en_mask) {
			dev_err(&client->dev, "Row4 cannot be used if reset1 is used\n");
			return -EINVAL;
		}

		kpad->extend_cfg = R4_EXTEND_CFG;
	}

	error = adp5589_parse_key_array(kpad, "adi,reset2-keys",
					kpad->reset2_keys, &kpad->nkeys_reset2,
					ARRAY_SIZE(kpad->reset2_keys), true);
	if (error)
		return error;
	if (kpad->nkeys_reset2 > 0) {
		/*
		 * Then C4 is used as reset output. Make sure it's not being used
		 * in the keypad.
		 */
		if (BIT(kpad->info->c4_extend_cfg) & kpad->keypad_en_mask) {
			dev_err(&client->dev, "Col4 cannot be used if reset2 is used\n");
			return -EINVAL;
		}

		kpad->extend_cfg |= C4_EXTEND_CFG;
	}

	if (!kpad->nkeys_reset2 && !kpad->nkeys_reset1)
		return 0;

	if (device_property_read_bool(&client->dev, "adi,reset1-active-high"))
		kpad->reset_cfg |= FIELD_PREP(RESET1_POL, 1);

	if (device_property_read_bool(&client->dev, "adi,reset2-active-high"))
		kpad->reset_cfg |= FIELD_PREP(RESET2_POL, 1);

	if (device_property_read_bool(&client->dev, "adi,rst-passtrough-enable"))
		kpad->reset_cfg |= FIELD_PREP(RST_PASSTHRU_EN, 1);

	error = device_property_read_u32(&client->dev, "adi,reset-trigger-ms",
					 &prop_val);
	if (!error) {
		switch (prop_val) {
		case 0:
			kpad->reset_cfg |= FIELD_PREP(RESET_TRIGGER_TIME, 0);
			break;
		case 1000:
			kpad->reset_cfg |= FIELD_PREP(RESET_TRIGGER_TIME, 1);
			break;
		case 1500:
			kpad->reset_cfg |= FIELD_PREP(RESET_TRIGGER_TIME, 2);
			break;
		case 2000:
			kpad->reset_cfg |= FIELD_PREP(RESET_TRIGGER_TIME, 3);
			break;
		case 2500:
			kpad->reset_cfg |= FIELD_PREP(RESET_TRIGGER_TIME, 4);
			break;
		case 3000:
			kpad->reset_cfg |= FIELD_PREP(RESET_TRIGGER_TIME, 5);
			break;
		case 3500:
			kpad->reset_cfg |= FIELD_PREP(RESET_TRIGGER_TIME, 6);
			break;
		case 4000:
			kpad->reset_cfg |= FIELD_PREP(RESET_TRIGGER_TIME, 7);
			break;
		default:
			dev_err(&client->dev, "Invalid value(%u) for adi,reset-trigger-ms\n",
				prop_val);
			return -EINVAL;
		}
	}

	error = device_property_read_u32(&client->dev,
					 "adi,reset-pulse-width-us", &prop_val);
	if (!error) {
		switch (prop_val) {
		case 500:
			kpad->reset_cfg |= FIELD_PREP(RESET_PULSE_WIDTH, 0);
			break;
		case 1000:
			kpad->reset_cfg |= FIELD_PREP(RESET_PULSE_WIDTH, 1);
			break;
		case 2000:
			kpad->reset_cfg |= FIELD_PREP(RESET_PULSE_WIDTH, 2);
			break;
		case 10000:
			kpad->reset_cfg |= FIELD_PREP(RESET_PULSE_WIDTH, 3);
			break;
		default:
			dev_err(&client->dev, "Invalid value(%u) for adi,reset-pulse-width-us\n",
				prop_val);
			return -EINVAL;
		}
	}

	return 0;
}

static int adp5589_gpio_parse(struct adp5589_kpad *kpad)
{
	struct i2c_client *client = kpad->client;
	u32 reg, pullup;
	int error;

	device_for_each_child_node_scoped(&client->dev, child) {
		error = fwnode_property_read_u32(child, "reg", &reg);
		if (error) {
			dev_err(&client->dev, "Failed to get reg property\n");
			return -EINVAL;
		}

		if (reg >= kpad->info->maxgpio) {
			dev_err(&client->dev, "Invalid gpio(%u > %u)\n",
				reg, kpad->info->maxgpio);
			return -EINVAL;
		}

		if (BIT(reg) & kpad->keypad_en_mask) {
			dev_err(&client->dev, "Invalid gpio(%u) used in keypad\n",
				reg);
			return -EINVAL;
		}

		if (reg == 5 && !kpad->info->support_row5) {
			dev_err(&client->dev, "Invalid gpio(%u) not supported\n",
				reg);
			return -EINVAL;
		}

		/* Check if it's gpio4 and R4 is being used as reset */
		if (kpad->extend_cfg & R4_EXTEND_CFG && reg == 4) {
			dev_err(&client->dev, "Invalid gpio(%u) used as reset1\n",
				reg);
			return -EINVAL;
		}

		/* Check if the gpio is being used as reset2 */
		if (kpad->extend_cfg & C4_EXTEND_CFG && reg == kpad->info->c4_extend_cfg) {
			dev_err(&client->dev, "Invalid gpio(%u) used as reset2\n",
				reg);
			return -EINVAL;
		}

		error = fwnode_property_read_u32(child, "adi,pull-up-ohms",
						 &pullup);
		if (!error) {
			if (pullup != 100 * KILO && pullup != 300 * KILO) {
				dev_err(&client->dev, "Invalid pullup resistor val(%u)",
					pullup);
				return -EINVAL;
			}

			if (pullup == 100 * KILO)
				__set_bit(reg, &kpad->pull_up_100k_map);
		}
	}

	return 0;
}

static int adp5589_parse_fw(struct adp5589_kpad *kpad)
{
	struct i2c_client *client = kpad->client;
	u32 cols = 0, rows = 0, prop_val;
	int error;

	error = device_property_read_u32(&client->dev, "adi,cols-mask",
					 &prop_val);
	if (!error) {
		if (prop_val > GENMASK(kpad->info->max_col_num, 0)) {
			dev_err(&client->dev, "Invalid column mask(%x)\n",
				prop_val);
			return -EINVAL;
		}

		kpad->keypad_en_mask = prop_val << kpad->info->col_shift;
		/*
		 * Note that given that we get a mask (and the HW allows it), we
		 * can have holes in our keypad (eg: row0, row1 and row7 enabled).
		 * However, for the matrix parsing functions we need to pass the
		 * number of rows/cols as the maximum row/col used plus 1. This
		 * pretty much means we will also have holes in our SW keypad.
		 */
		cols = fls(prop_val);
	}

	error = device_property_read_u32(&client->dev, "adi,rows-mask",
					 &prop_val);
	if (!error) {
		if (prop_val > GENMASK(kpad->info->max_row_num, 0)) {
			dev_err(&client->dev, "Invalid row mask(%x)\n",
				prop_val);
			return -EINVAL;
		}

		if (prop_val & BIT(5) && !kpad->info->support_row5) {
			dev_err(&client->dev, "Row5 not supported!\n");
			return -EINVAL;
		}

		kpad->keypad_en_mask |= prop_val;
		rows = fls(prop_val);
	}

	if (cols && !rows) {
		dev_err(&client->dev, "Cannot have columns with no rows!\n");
		return -EINVAL;
	}

	if (rows && !cols) {
		dev_err(&client->dev, "Cannot have rows with no columns!\n");
		return -EINVAL;
	}

	if (rows && cols) {
		if (!client->irq) {
			dev_err(&client->dev,
				"Keymaps won't work without interrupts\n");
			return -EINVAL;
		}

		error = matrix_keypad_build_keymap(NULL, NULL, rows, cols,
						   kpad->keycode, kpad->input);
		if (error)
			return error;

		kpad->row_shift = get_count_order(cols);
	}

	if (device_property_present(&client->dev, "autorepeat"))
		__set_bit(EV_REP, kpad->input->evbit);

	error = device_property_read_u32(&client->dev, "adi,key-poll-ms",
					 &prop_val);
	if (!error) {
		switch (prop_val) {
		case 10:
			fallthrough;
		case 20:
			fallthrough;
		case 30:
			fallthrough;
		case 40:
			kpad->key_poll_time = prop_val / 10 - 1;
			break;
		default:
			dev_err(&client->dev, "Invalid value(%u) for adi,key-poll-ms\n",
				prop_val);
			return -EINVAL;
		}
	}

	error = adp5589_unlock_parse(kpad);
	if (error)
		return error;

	error = adp5589_reset_parse(kpad);
	if (error)
		return error;

	return adp5589_gpio_parse(kpad);
}

static int adp5589_keypad_add(struct adp5589_kpad *kpad, unsigned int revid)
{
	struct i2c_client *client = kpad->client;
	struct input_dev *input;
	int error;

	input = devm_input_allocate_device(&client->dev);
	if (!input)
		return -ENOMEM;

	kpad->input = input;

	input->name = client->name;
	input->phys = "adp5589-keys/input0";
	input->dev.parent = &client->dev;

	input_set_drvdata(input, kpad);

	input->id.bustype = BUS_I2C;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = revid;

	error = adp5589_parse_fw(kpad);
	if (error)
		return error;

	error = input_register_device(input);
	if (error) {
		dev_err(&client->dev, "unable to register input device\n");
		return error;
	}

	if (!client->irq)
		return 0;

	error = devm_request_threaded_irq(&client->dev, client->irq,
					  NULL, adp5589_irq,
					  IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					  client->dev.driver->name, kpad);
	if (error) {
		dev_err(&client->dev, "unable to request irq %d\n", client->irq);
		return error;
	}

	return 0;
}

static void adp5589_clear_config(void *data)
{
	struct adp5589_kpad *kpad = data;

	adp5589_write(kpad->client, kpad->info->reg(ADP5589_GENERAL_CFG), 0);
}

static const struct adp5589_info adp5589_info = {
	.support_row5 = true,
	.rpull_banks = ADP5589_RPULL_CONFIG_E - ADP5589_RPULL_CONFIG_A + 1,
	.c4_extend_cfg = 12,
	.maxgpio = ADP5589_MAXGPIO,
	.keymapsize = ADP5589_KEYMAPSIZE,
	.gpi_pin_base = 97,
	.gpi_pin_end = 115,
	.max_row_num = ADP5589_MAX_ROW_NUM,
	.max_col_num = ADP5589_MAX_COL_NUM,
	.col_shift = ADP5589_COL_SHIFT,
	.bank = adp5589_bank,
	.bit = adp5589_bit,
	.reg = adp5589_reg,
};

static const struct adp5589_info adp5585_info = {
	.is_adp5585 = true,
	.rpull_banks = ADP5585_RPULL_CONFIG_D - ADP5585_RPULL_CONFIG_A + 1,
	.c4_extend_cfg = 10,
	.maxgpio = ADP5585_MAXGPIO,
	.keymapsize = ADP5585_KEYMAPSIZE,
	.gpi_pin_base = 37,
	.gpi_pin_end = 47,
	.max_row_num = ADP5585_MAX_ROW_NUM,
	.max_col_num = ADP5585_MAX_COL_NUM,
	.col_shift = ADP5585_COL_SHIFT,
	.bank = adp5585_bank,
	.bit = adp5585_bit,
	.reg = adp5585_reg,
};

static const struct adp5589_info adp5585_2_info = {
	.is_adp5585 = true,
	.support_row5 = true,
	.rpull_banks = ADP5585_RPULL_CONFIG_D - ADP5585_RPULL_CONFIG_A + 1,
	.c4_extend_cfg = 10,
	.maxgpio = ADP5585_MAXGPIO,
	.keymapsize = ADP5585_KEYMAPSIZE,
	.gpi_pin_base = 37,
	.gpi_pin_end = 47,
	.max_row_num = ADP5585_MAX_ROW_NUM,
	.max_col_num = ADP5585_MAX_COL_NUM,
	.col_shift = ADP5585_COL_SHIFT,
	.bank = adp5585_bank,
	.bit = adp5585_bit,
	.reg = adp5585_reg,
};

static int adp5589_probe(struct i2c_client *client)
{
	struct adp5589_kpad *kpad;
	unsigned int revid;
	int error;
	u8 id;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "SMBUS Byte Data not Supported\n");
		return -EIO;
	}

	kpad = devm_kzalloc(&client->dev, sizeof(*kpad), GFP_KERNEL);
	if (!kpad)
		return -ENOMEM;

	kpad->client = client;

	kpad->info = i2c_get_match_data(client);
	if (!kpad->info)
		return -ENODEV;

	error = devm_add_action_or_reset(&client->dev, adp5589_clear_config,
					 kpad);
	if (error)
		return error;

	error = adp5589_read(client, ADP5589_5_ID, &id);
	if (error)
		return error;

	revid = id & ADP5589_5_DEVICE_ID_MASK;

	error = adp5589_keypad_add(kpad, revid);
	if (error)
		return error;

	error = adp5589_setup(kpad);
	if (error)
		return error;

	error = adp5589_gpio_add(kpad);
	if (error)
		return error;

	dev_info(&client->dev, "Rev.%d keypad, irq %d\n", revid, client->irq);
	return 0;
}

static int adp5589_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct adp5589_kpad *kpad = i2c_get_clientdata(client);

	if (kpad->input)
		disable_irq(client->irq);

	return 0;
}

static int adp5589_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct adp5589_kpad *kpad = i2c_get_clientdata(client);

	if (kpad->input)
		enable_irq(client->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(adp5589_dev_pm_ops, adp5589_suspend, adp5589_resume);

static const struct i2c_device_id adp5589_id[] = {
	{"adp5589-keys", (kernel_ulong_t)&adp5589_info},
	{"adp5585-keys", (kernel_ulong_t)&adp5585_info},
	{"adp5585-02-keys", (kernel_ulong_t)&adp5585_2_info}, /* Adds ROW5 to ADP5585 */
	{}
};

MODULE_DEVICE_TABLE(i2c, adp5589_id);

static const struct of_device_id adp5589_of_id[] = {
	{ .compatible = "adi,adp5589", .data = &adp5589_info},
	{ .compatible = "adi,adp5585", .data = &adp5585_info},
	{ .compatible = "adi,adp5585-2", .data = &adp5585_2_info},
	{}
};
MODULE_DEVICE_TABLE(of, adp5589_of_id);

static struct i2c_driver adp5589_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.pm = pm_sleep_ptr(&adp5589_dev_pm_ops),
		.of_match_table = adp5589_of_id,
	},
	.probe = adp5589_probe,
	.id_table = adp5589_id,
};

module_i2c_driver(adp5589_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Hennerich <hennerich@blackfin.uclinux.org>");
MODULE_DESCRIPTION("ADP5589/ADP5585 Keypad driver");
