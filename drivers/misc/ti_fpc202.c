// SPDX-License-Identifier: GPL-2.0-only
/*
 * ti_fpc202.c - FPC202 Dual Port Controller driver
 *
 * Copyright (C) 2024 Bootlin
 *
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/i2c-atr.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>

#define FPC202_ALIASES_PER_PORT 2

/*
 * GPIO: port mapping
 *
 * 0: P0_S0_IN_A
 * 1: P0_S1_IN_A
 * 2: P1_S0_IN_A
 * 3: P1_S1_IN_A
 * 4: P0_S0_IN_B
 * ...
 * 8: P0_S0_IN_C
 * ...
 * 12: P0_S0_OUT_A
 * ...
 * 16: P0_S0_OUT_B
 * ...
 * 19: P1_S1_OUT_B
 *
 */

#define FPC202_GPIO_COUNT 20
#define FPC202_GPIO_P0_S0_IN_B  4
#define FPC202_GPIO_P0_S0_OUT_A 12

#define FPC202_REG_IN_A_INT    0x6
#define FPC202_REG_IN_C_IN_B   0x7
#define FPC202_REG_OUT_A_OUT_B 0x8

#define FPC202_REG_OUT_A_OUT_B_VAL 0xa

#define FPC202_REG_MOD_DEV(port, dev) (0xb4 + ((port) * 4) + (dev))
#define FPC202_REG_AUX_DEV(port, dev) (0xb6 + ((port) * 4) + (dev))

/*
 * The FPC202 doesn't support turning off address translation on a single port.
 * So just set an invalid I2C address as the translation target when no client
 * address is attached.
 */
#define FPC202_REG_DEV_INVALID 0

/* Even aliases are assigned to device 0 and odd aliases to device 1 */
#define fpc202_dev_num_from_alias(alias) ((alias) % 2)

struct fpc202_priv {
	struct i2c_client *client;
	struct i2c_atr *atr;
	struct gpio_desc *en_gpio;
	struct gpio_chip gpio;

	/* Lock REG_MOD/AUX_DEV and addr_caches during attach/detach */
	struct mutex reg_dev_lock;

	/* Cached device addresses for both ports and their devices */
	u8 addr_caches[2][2];
};

static void fpc202_fill_alias_table(struct i2c_client *client, u16 *aliases, int port_id)
{
	u16 first_alias;
	int i;

	/*
	 * There is a predefined list of aliases for each FPC202 I2C
	 * self-address.  This allows daisy-chained FPC202 units to
	 * automatically take on different sets of aliases.
	 * Each port of an FPC202 unit is assigned two aliases from this list.
	 */
	first_alias = 0x10 + 4 * port_id + 8 * ((u16)client->addr - 2);

	for (i = 0; i < FPC202_ALIASES_PER_PORT; i++)
		aliases[i] = first_alias + i;
}

static int fpc202_gpio_get_dir(int offset)
{
	return offset < FPC202_GPIO_P0_S0_OUT_A ? GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}

static int fpc202_read(struct fpc202_priv *priv, u8 reg)
{
	int val;

	val = i2c_smbus_read_byte_data(priv->client, reg);
	return val;
}

static int fpc202_write(struct fpc202_priv *priv, u8 reg, u8 value)
{
	return i2c_smbus_write_byte_data(priv->client, reg, value);
}

static void fpc202_set_enable(struct fpc202_priv *priv, int enable)
{
	if (!priv->en_gpio)
		return;

	gpiod_set_value(priv->en_gpio, enable);
}

static void fpc202_gpio_set(struct gpio_chip *chip, unsigned int offset,
			    int value)
{
	struct fpc202_priv *priv = gpiochip_get_data(chip);
	int ret;
	u8 val;

	if (fpc202_gpio_get_dir(offset) == GPIO_LINE_DIRECTION_IN)
		return;

	ret = fpc202_read(priv, FPC202_REG_OUT_A_OUT_B_VAL);
	if (ret < 0) {
		dev_err(&priv->client->dev, "Failed to set GPIO %d value! err %d\n", offset, ret);
		return;
	}

	val = (u8)ret;

	if (value)
		val |= BIT(offset - FPC202_GPIO_P0_S0_OUT_A);
	else
		val &= ~BIT(offset - FPC202_GPIO_P0_S0_OUT_A);

	fpc202_write(priv, FPC202_REG_OUT_A_OUT_B_VAL, val);
}

static int fpc202_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct fpc202_priv *priv = gpiochip_get_data(chip);
	u8 reg, bit;
	int ret;

	if (offset < FPC202_GPIO_P0_S0_IN_B) {
		reg = FPC202_REG_IN_A_INT;
		bit = BIT(4 + offset);
	} else if (offset < FPC202_GPIO_P0_S0_OUT_A) {
		reg = FPC202_REG_IN_C_IN_B;
		bit = BIT(offset - FPC202_GPIO_P0_S0_IN_B);
	} else {
		reg = FPC202_REG_OUT_A_OUT_B_VAL;
		bit = BIT(offset - FPC202_GPIO_P0_S0_OUT_A);
	}

	ret = fpc202_read(priv, reg);
	if (ret < 0)
		return ret;

	return !!(((u8)ret) & bit);
}

static int fpc202_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	if (fpc202_gpio_get_dir(offset) == GPIO_LINE_DIRECTION_OUT)
		return -EINVAL;

	return 0;
}

static int fpc202_gpio_direction_output(struct gpio_chip *chip, unsigned int offset,
					int value)
{
	struct fpc202_priv *priv = gpiochip_get_data(chip);
	int ret;
	u8 val;

	if (fpc202_gpio_get_dir(offset) == GPIO_LINE_DIRECTION_IN)
		return -EINVAL;

	fpc202_gpio_set(chip, offset, value);

	ret = fpc202_read(priv, FPC202_REG_OUT_A_OUT_B);
	if (ret < 0)
		return ret;

	val = (u8)ret | BIT(offset - FPC202_GPIO_P0_S0_OUT_A);

	return fpc202_write(priv, FPC202_REG_OUT_A_OUT_B, val);
}

/*
 * Set the translation table entry associated with a port and device number.
 *
 * Each downstream port of the FPC202 has two fixed aliases corresponding to
 * device numbers 0 and 1. If one of these aliases is found in an incoming I2C
 * transfer, it will be translated to the address given by the corresponding
 * translation table entry.
 */
static int fpc202_write_dev_addr(struct fpc202_priv *priv, u32 port_id, int dev_num, u16 addr)
{
	int ret, reg_mod, reg_aux;
	u8 val;

	mutex_lock(&priv->reg_dev_lock);

	reg_mod = FPC202_REG_MOD_DEV(port_id, dev_num);
	reg_aux = FPC202_REG_AUX_DEV(port_id, dev_num);
	val = addr & 0x7f;

	ret = fpc202_write(priv, reg_mod, val);
	if (ret)
		goto out_unlock;

	/*
	 * The FPC202 datasheet is unclear about the role of the AUX registers.
	 * Empirically, writing to them as well seems to be necessary for
	 * address translation to function properly.
	 */
	ret = fpc202_write(priv, reg_aux, val);

	priv->addr_caches[port_id][dev_num] = val;

out_unlock:
	mutex_unlock(&priv->reg_dev_lock);
	return ret;
}

static int fpc202_attach_addr(struct i2c_atr *atr, u32 chan_id,
			      u16 addr, u16 alias)
{
	struct fpc202_priv *priv = i2c_atr_get_driver_data(atr);

	dev_dbg(&priv->client->dev, "attaching address 0x%02x to alias 0x%02x\n", addr, alias);

	return fpc202_write_dev_addr(priv, chan_id, fpc202_dev_num_from_alias(alias), addr);
}

static void fpc202_detach_addr(struct i2c_atr *atr, u32 chan_id,
			       u16 addr)
{
	struct fpc202_priv *priv = i2c_atr_get_driver_data(atr);
	int dev_num, reg_mod, val;

	for (dev_num = 0; dev_num < 2; dev_num++) {
		reg_mod = FPC202_REG_MOD_DEV(chan_id, dev_num);

		mutex_lock(&priv->reg_dev_lock);

		val = priv->addr_caches[chan_id][dev_num];

		mutex_unlock(&priv->reg_dev_lock);

		if (val < 0) {
			dev_err(&priv->client->dev, "failed to read register 0x%x while detaching address 0x%02x\n",
				reg_mod, addr);
			return;
		}

		if (val == (addr & 0x7f)) {
			fpc202_write_dev_addr(priv, chan_id, dev_num, FPC202_REG_DEV_INVALID);
			return;
		}
	}
}

struct i2c_atr_ops fpc202_atr_ops = {
	.attach_addr = fpc202_attach_addr,
	.detach_addr = fpc202_detach_addr,
};

static int fpc202_probe_port(struct fpc202_priv *priv, int port_id)
{
	struct device *dev = &priv->client->dev;
	u16 aliases[FPC202_ALIASES_PER_PORT] = { };
	struct i2c_atr_adap_desc desc = { };
	struct device_node *i2c_handle;
	char port_name[6];
	int ret = 0;

	if (port_id > 2)
		return -EINVAL;

	snprintf(port_name, 6, "port%d", port_id);

	i2c_handle = of_get_child_by_name(dev->of_node, port_name);
	if (!i2c_handle)
		return -EINVAL;

	desc.chan_id = port_id;
	desc.parent = dev;
	desc.bus_handle = of_node_to_fwnode(i2c_handle);
	desc.num_aliases = FPC202_ALIASES_PER_PORT;

	fpc202_fill_alias_table(priv->client, aliases, port_id);
	desc.aliases = aliases;

	ret = i2c_atr_add_adapter(priv->atr, &desc);
	if (ret)
		return ret;

	of_node_put(i2c_handle);

	ret = fpc202_write_dev_addr(priv, port_id, 0, FPC202_REG_DEV_INVALID);
	if (ret)
		return ret;

	return fpc202_write_dev_addr(priv, port_id, 1, FPC202_REG_DEV_INVALID);
}

static int fpc202_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fpc202_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->reg_dev_lock);

	priv->client = client;
	i2c_set_clientdata(client, priv);

	priv->en_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->en_gpio)) {
		ret = PTR_ERR(priv->en_gpio);
		dev_err(dev, "failed to fetch enable GPIO! err %d\n", ret);
		goto destroy_mutex;
	}

	priv->gpio.label = "gpio-fpc202";
	priv->gpio.base = -1;
	priv->gpio.direction_input = fpc202_gpio_direction_input;
	priv->gpio.direction_output = fpc202_gpio_direction_output;
	priv->gpio.set = fpc202_gpio_set;
	priv->gpio.get = fpc202_gpio_get;
	priv->gpio.ngpio = FPC202_GPIO_COUNT;
	priv->gpio.parent = dev;
	priv->gpio.owner = THIS_MODULE;

	ret = gpiochip_add_data(&priv->gpio, priv);
	if (ret) {
		priv->gpio.parent = NULL;
		dev_err(dev, "failed to add gpiochip err %d", ret);
		goto disable_gpio;
	}

	priv->atr = i2c_atr_new(client->adapter, dev, &fpc202_atr_ops, 2, I2C_ATR_FLAG_DYNAMIC_C2A);
	if (IS_ERR(priv->atr)) {
		ret = PTR_ERR(priv->atr);
		dev_err(dev, "failed to create i2c atr err %d", ret);
		goto disable_gpio;
	}

	i2c_atr_set_driver_data(priv->atr, priv);

	ret = fpc202_probe_port(priv, 0);
	if (ret) {
		dev_err(dev, "Failed to probe port 0, err %d\n", ret);
		goto delete_atr;
	}

	ret = fpc202_probe_port(priv, 1);
	if (ret) {
		dev_err(dev, "Failed to probe port 1, err %d\n", ret);
		goto delete_atr;
	}

	dev_info(&client->dev, "%s FPC202 Dual Port controller found\n", client->name);

	goto out;

delete_atr:
	i2c_atr_delete(priv->atr);
disable_gpio:
	fpc202_set_enable(priv, 0);
	gpiochip_remove(&priv->gpio);
destroy_mutex:
	mutex_destroy(&priv->reg_dev_lock);
out:
	return ret;
}

static void fpc202_remove(struct i2c_client *client)
{
	struct fpc202_priv *priv = i2c_get_clientdata(client);

	mutex_destroy(&priv->reg_dev_lock);

	i2c_atr_delete(priv->atr);

	fpc202_set_enable(priv, 0);
	gpiochip_remove(&priv->gpio);
}

static const struct of_device_id fpc202_of_match[] = {
	{ .compatible = "ti,fpc202" },
	{}
};
MODULE_DEVICE_TABLE(of, fpc202_of_match);

static struct i2c_driver fpc202_driver = {
	.driver = {
		.name = "fpc202",
		.of_match_table = fpc202_of_match,
	},
	.probe = fpc202_probe,
	.remove = fpc202_remove,
};

module_i2c_driver(fpc202_driver);

MODULE_AUTHOR("Romain Gantois <romain.gantois@bootlin.com>");
MODULE_DESCRIPTION("TI FPC202 Dual Port Controller driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(I2C_ATR);
