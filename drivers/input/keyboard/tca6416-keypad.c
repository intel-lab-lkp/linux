// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for keys on TCA6416 I2C IO expander
 *
 * Copyright (C) 2010 Texas Instruments
 *
 * Author : Sriramakrishnan.A.G. <srk@ti.com>
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/tca6416_keypad.h>
#include <linux/bitfield.h>

#define TCA6416_INPUT          0
#define TCA6416_OUTPUT         1
#define TCA6416_INVERT         2
#define TCA6416_DIRECTION      3

#define TCA6416_POLL_INTERVAL	100 /* msec */
#define TCA6416_MAX_IO_SIZE 16 /* maximum number of inputs */

static const struct i2c_device_id tca6416_id[] = {
	{ "tca6416-keys", 16, },
	{ "tca6408-keys", 8, },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tca6416_id);

struct tca6416_keypad_chip {
	uint16_t reg_output;
	uint16_t reg_direction;
	uint16_t reg_input;

	struct i2c_client *client;
	struct input_dev *input;
	int io_size;
	u16 pinmask;
	bool use_polling;
	struct tca6416_button buttons[];
};

static int tca6416_write_reg(struct tca6416_keypad_chip *chip, int reg, u16 val)
{
	int error;

	error = chip->io_size > 8 ?
		i2c_smbus_write_word_data(chip->client, reg << 1, val) :
		i2c_smbus_write_byte_data(chip->client, reg, val);
	if (error < 0) {
		dev_err(&chip->client->dev,
			"%s failed, reg: %d, val: %d, error: %d\n",
			__func__, reg, val, error);
		return error;
	}

	return 0;
}

static int tca6416_read_reg(struct tca6416_keypad_chip *chip, int reg, u16 *val)
{
	int retval;

	retval = chip->io_size > 8 ?
		 i2c_smbus_read_word_data(chip->client, reg << 1) :
		 i2c_smbus_read_byte_data(chip->client, reg);
	if (retval < 0) {
		dev_err(&chip->client->dev, "%s failed, reg: %d, error: %d\n",
			__func__, reg, retval);
		return retval;
	}

	*val = (u16)retval;
	return 0;
}

static void tca6416_keys_scan(struct input_dev *input)
{
	struct tca6416_keypad_chip *chip = input_get_drvdata(input);
	u16 reg_val, val;
	int error, i, pin_index;

	error = tca6416_read_reg(chip, TCA6416_INPUT, &reg_val);
	if (error)
		return;

	reg_val &= chip->pinmask;

	/* Figure out which lines have changed */
	val = reg_val ^ chip->reg_input;
	chip->reg_input = reg_val;

	for (i = 0, pin_index = 0; i < 16; i++) {
		if (val & (1 << i)) {
			struct tca6416_button *button = &chip->buttons[pin_index];
			unsigned int type = button->type ?: EV_KEY;
			int state = ((reg_val & (1 << i)) ? 1 : 0)
						^ button->active_low;

			input_event(input, type, button->code, !!state);
			input_sync(input);
		}

		if (chip->pinmask & (1 << i))
			pin_index++;
	}
}

/*
 * This is threaded IRQ handler and this can (and will) sleep.
 */
static irqreturn_t tca6416_keys_isr(int irq, void *dev_id)
{
	tca6416_keys_scan(dev_id);

	return IRQ_HANDLED;
}

static int tca6416_keys_open(struct input_dev *dev)
{
	struct tca6416_keypad_chip *chip = input_get_drvdata(dev);

	if (!chip->use_polling) {
		/* Get initial device state in case it has switches */
		tca6416_keys_scan(dev);
		enable_irq(chip->client->irq);
	}

	return 0;
}

static void tca6416_keys_close(struct input_dev *dev)
{
	struct tca6416_keypad_chip *chip = input_get_drvdata(dev);

	if (!chip->use_polling)
		disable_irq(chip->client->irq);
}

static int tca6416_setup_registers(struct tca6416_keypad_chip *chip)
{
	int error;

	error = tca6416_read_reg(chip, TCA6416_OUTPUT, &chip->reg_output);
	if (error)
		return error;

	error = tca6416_read_reg(chip, TCA6416_DIRECTION, &chip->reg_direction);
	if (error)
		return error;

	/* ensure that keypad pins are set to input */
	error = tca6416_write_reg(chip, TCA6416_DIRECTION,
				  chip->reg_direction | chip->pinmask);
	if (error)
		return error;

	error = tca6416_read_reg(chip, TCA6416_DIRECTION, &chip->reg_direction);
	if (error)
		return error;

	error = tca6416_read_reg(chip, TCA6416_INPUT, &chip->reg_input);
	if (error)
		return error;

	chip->reg_input &= chip->pinmask;

	return 0;
}

/* Configuration bitmap
 * | 31:18    |         17 | 16:14 | 13:10    | 9:0  |
 * | reserved | active_low | type  | reserved | code |
 */
#define CFG_CODE GENMASK(9, 0)
#define CFG_TYPE GENMASK(16, 14)
#define CFG_ACTIVE_LOW BIT(17)

static struct tca6416_keys_platform_data *
tca6416_parse_properties(struct device *dev, uint8_t io_size)
{
	static const char keymap_property[] = "linux,gpio-keymap";
	struct tca6416_keys_platform_data *pdata;
	u32 keymap[TCA6416_MAX_IO_SIZE];
	struct tca6416_button *buttons;
	int ret, i;
	u8 pin;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	ret = device_property_count_u32(dev, keymap_property);
	if (ret <= 0)
		return NULL;

	pdata->nbuttons = ret;
	if (pdata->nbuttons > io_size)
		pdata->nbuttons = io_size;

	ret = device_property_read_u32_array(dev, keymap_property, keymap,
					     pdata->nbuttons);
	if (ret)
		return NULL;

	buttons = devm_kcalloc(dev, pdata->nbuttons, sizeof(*buttons),
			       GFP_KERNEL);
	if (!buttons)
		return NULL;

	for (i = 0; i < pdata->nbuttons; i++) {
		buttons[i].code = FIELD_GET(CFG_CODE, keymap[i]);
		buttons[i].type = FIELD_GET(CFG_TYPE, keymap[i]);
		buttons[i].active_low = FIELD_GET(CFG_ACTIVE_LOW, keymap[i]);
		/* enable all inputs by default */
		pdata->pinmask |= BIT(i);
	}

	pdata->buttons = buttons;

	pdata->rep = device_property_read_bool(dev, "autorepeat");
	/* we can ignore the result as by default all inputs are enabled */
	device_property_read_u16(dev, "pinmask", &pdata->pinmask);
	pdata->use_polling = device_property_read_bool(dev, "polling");

	return pdata;
}

static int tca6416_keypad_probe(struct i2c_client *client)
{
	uint8_t io_size = (uintptr_t)i2c_get_match_data(client);
	struct tca6416_keys_platform_data *pdata;
	struct tca6416_keypad_chip *chip;
	struct input_dev *input;
	int error;
	int i;

	/* Check functionality */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
		dev_err(&client->dev, "%s adapter not supported\n",
			dev_driver_string(&client->adapter->dev));
		return -ENODEV;
	}

	pdata = dev_get_platdata(&client->dev);
	if (!pdata && dev_fwnode(&client->dev)) {
		pdata = tca6416_parse_properties(&client->dev, io_size);
		if (!pdata) {
			dev_err(&client->dev,
				"Failed to parse device configuration from properties\n");
			return -EINVAL;
		}
	}

	chip = devm_kzalloc(&client->dev,
			    struct_size(chip, buttons, pdata->nbuttons),
			    GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	input = devm_input_allocate_device(&client->dev);
	if (!input)
		return -ENOMEM;

	chip->client = client;
	chip->input = input;
	chip->io_size = io_size;
	chip->pinmask = pdata->pinmask;
	chip->use_polling = pdata->use_polling;

	input->phys = "tca6416-keys/input0";
	input->name = client->name;

	input->open = tca6416_keys_open;
	input->close = tca6416_keys_close;

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	/* Enable auto repeat feature of Linux input subsystem */
	if (pdata->rep)
		__set_bit(EV_REP, input->evbit);

	for (i = 0; i < pdata->nbuttons; i++) {
		unsigned int type;

		chip->buttons[i] = pdata->buttons[i];
		type = (pdata->buttons[i].type) ?: EV_KEY;
		input_set_capability(input, type, pdata->buttons[i].code);
	}

	input_set_drvdata(input, chip);

	/*
	 * Initialize cached registers from their original values.
	 * we can't share this chip with another i2c master.
	 */
	error = tca6416_setup_registers(chip);
	if (error)
		return error;

	if (chip->use_polling) {
		error = input_setup_polling(input, tca6416_keys_scan);
		if (error) {
			dev_err(&client->dev, "Failed to setup polling\n");
			return error;
		}

		input_set_poll_interval(input, TCA6416_POLL_INTERVAL);
	} else {
		error = devm_request_threaded_irq(&client->dev, client->irq,
						  NULL, tca6416_keys_isr,
						  IRQF_TRIGGER_FALLING |
							IRQF_ONESHOT |
							IRQF_NO_AUTOEN,
						  "tca6416-keypad", input);
		if (error) {
			dev_dbg(&client->dev,
				"Unable to claim irq %d; error %d\n",
				client->irq, error);
			return error;
		}
	}

	error = input_register_device(input);
	if (error) {
		dev_dbg(&client->dev,
			"Unable to register input device, error: %d\n", error);
		return error;
	}

	i2c_set_clientdata(client, chip);

	return 0;
}

static const struct of_device_id tca6416_of_match[] = {
	{
		.compatible = "ti,tca6416_keys",
		.data = (void *)16,
	},
	{
		.compatible = "ti,tca6408_keys",
		.data = (void *)8,
	},
	{}
};
MODULE_DEVICE_TABLE(of, tca6416_of_match);

static struct i2c_driver tca6416_keypad_driver = {
	.driver = {
		.name	= "tca6416-keypad",
		.of_match_table = tca6416_of_match,
	},
	.probe		= tca6416_keypad_probe,
	.id_table	= tca6416_id,
};

static int __init tca6416_keypad_init(void)
{
	return i2c_add_driver(&tca6416_keypad_driver);
}

subsys_initcall(tca6416_keypad_init);

static void __exit tca6416_keypad_exit(void)
{
	i2c_del_driver(&tca6416_keypad_driver);
}
module_exit(tca6416_keypad_exit);

MODULE_AUTHOR("Sriramakrishnan <srk@ti.com>");
MODULE_DESCRIPTION("Keypad driver over tca6416 IO expander");
MODULE_LICENSE("GPL");
