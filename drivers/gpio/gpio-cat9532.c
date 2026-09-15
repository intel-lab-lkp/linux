// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

struct cat9532 {
	struct regmap *regmap;
	struct gpio_chip gpio;

	spinlock_t lock;
	unsigned int direction;
};

#define CAT9532_REG_INPUT0	0
#define CAT9532_REG_LS0		6

/* Set output to Hi-Z, LED is OFF */
#define CAT9532_LED_OUT_HIZ	0
/* Set output low  */
#define CAT9532_LED_OUT_LOW	1
#define CAT9532_LED_OUT_MASK	3

static const struct regmap_config cat9532_regmap_cfg = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 9,
	/*
	 * Auto increment can be enabled by setting bit 4 in the
	 * register address, but we don't really need bulk access.
	 */
	.use_single_read = true,
	.use_single_write = true,
	.can_sleep = true,
};

static int cat9532_gpio_get_direction(struct gpio_chip *chip,
					unsigned int offset)
{
	struct cat9532 *cat9532 = container_of(chip, struct cat9532, gpio);

	scoped_guard(spinlock, &cat9532->lock)
		if (cat9532->direction & BIT(offset))
			return GPIO_LINE_DIRECTION_OUT;
		else
			return GPIO_LINE_DIRECTION_IN;
}

static int cat9532_gpio_direction_input(struct gpio_chip *chip,
					unsigned int offset)
{
	struct cat9532 *cat9532 = container_of(chip, struct cat9532, gpio);
	unsigned int reg = CAT9532_REG_LS0 + (offset >> 2);
	unsigned int shift = (offset & 3) << 1;
	int err;

	err = regmap_update_bits(cat9532->regmap, reg,
				 CAT9532_LED_OUT_MASK << shift,
				 CAT9532_LED_OUT_HIZ << shift);
	if (err < 0)
		return err;

	scoped_guard(spinlock, &cat9532->lock)
		cat9532->direction &= ~BIT(offset);

	return 0;
}

static int cat9532_gpio_get(struct gpio_chip *chip,
			    unsigned int offset)
{
	struct cat9532 *cat9532 = container_of(chip, struct cat9532, gpio);
	unsigned int reg = CAT9532_REG_INPUT0 + (offset >> 3);
	unsigned int mask = BIT(offset & 7);
	unsigned int val;
	int err;

	err = regmap_read(cat9532->regmap, reg, &val);
	if (err < 0)
		return err;

	return !!(val & mask);
}

static int cat9532_gpio_set(struct gpio_chip *chip,
			    unsigned int offset, int value)
{
	struct cat9532 *cat9532 = container_of(chip, struct cat9532, gpio);
	unsigned int reg = CAT9532_REG_LS0 + (offset >> 2);
	unsigned int shift = (offset & 3) << 1;
	unsigned int val;

	val = value ? CAT9532_LED_OUT_HIZ : CAT9532_LED_OUT_LOW;
	return regmap_update_bits(cat9532->regmap, reg,
				  CAT9532_LED_OUT_MASK << shift,
				  val << shift);
}

static int cat9532_gpio_direction_output(struct gpio_chip *chip,
					 unsigned int offset, int value)
{
	struct cat9532 *cat9532 = container_of(chip, struct cat9532, gpio);
	int err;

	err = cat9532_gpio_set(chip, offset, value);
	if (err < 0)
		return err;

	scoped_guard(spinlock, &cat9532->lock)
		cat9532->direction |= BIT(offset);

	return 0;
}

static int cat9532_i2c_probe(struct i2c_client *client)
{
	struct cat9532 *cat9532;
	struct gpio_chip *chip;

	cat9532 = devm_kzalloc(&client->dev, sizeof(*cat9532), GFP_KERNEL);
	if (!cat9532)
		return -ENOMEM;

	spin_lock_init(&cat9532->lock);

	cat9532->regmap = devm_regmap_init_i2c(client, &cat9532_regmap_cfg);
	if (IS_ERR(cat9532->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(cat9532->regmap),
				     "failed to initialize regmap\n");

	chip = &cat9532->gpio;
	chip->label = "cat9532";
	chip->parent = &client->dev;
	chip->owner = THIS_MODULE;
	chip->can_sleep = true;
	chip->base = -1;
	chip->ngpio = 16;

	chip->get_direction = cat9532_gpio_get_direction;
	chip->direction_input = cat9532_gpio_direction_input;
	chip->direction_output = cat9532_gpio_direction_output;
	chip->get = cat9532_gpio_get;
	chip->set = cat9532_gpio_set;

	return devm_gpiochip_add_data(&client->dev, chip, cat9532);
}

static const struct i2c_device_id cat9532_i2c_id[] = {
	{ "cat9532" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, cat9532_i2c_id);

static const struct of_device_id cat9532_of_match[] = {
	{ .compatible = "onnn,cat9532" },
	{ },
};
MODULE_DEVICE_TABLE(of, cat9532_of_match);

static struct i2c_driver cat9532_i2c_driver = {
	.driver = {
		.name = "gpio-cat9532",
		.of_match_table = cat9532_of_match,
	},
	.probe = cat9532_i2c_probe,
	.id_table = cat9532_i2c_id,
};
module_i2c_driver(cat9532_i2c_driver);

MODULE_DESCRIPTION("On Semiconductor CAT9532 GPIO driver");
MODULE_AUTHOR("Alban Bedel <alban.bedel@lht.dlh.de>");
MODULE_LICENSE("GPL");
