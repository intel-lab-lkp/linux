// SPDX-License-Identifier: GPL-2.0-only
/*
 * pca9532.c - 16-bit Led dimmer
 *
 * Copyright (C) 2011 Jan Weitzel
 * Copyright (C) 2008 Riku Voipio
 *
 * Datasheet: http://www.nxp.com/documents/data_sheet/PCA9532.pdf
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/leds-pca9532.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>

/* m =  num_leds*/
#define PCA9532_REG_INPUT(i)	((i) >> 3)
#define PCA9532_REG_OFFSET(m)	((m) >> 4)
#define PCA9532_REG_PSC(m, i)	(PCA9532_REG_OFFSET(m) + 0x1 + (i) * 2)
#define PCA9532_REG_PWM(m, i)	(PCA9532_REG_OFFSET(m) + 0x2 + (i) * 2)
#define LED_REG(m, led)		(PCA9532_REG_OFFSET(m) + 0x5 + (led >> 2))
#define LED_NUM(led)		(led & 0x3)
#define LED_SHIFT(led)		(LED_NUM(led) * 2)
#define LED_MASK(led)		(0x3 << LED_SHIFT(led))

#define PCA9532_PWM_PERIOD_DIV	152
#define PCA9532_PWM_DUTY_DIV	256

#define ldev_to_led(c)       container_of(c, struct pca9532_led, ldev)

struct pca9532_chip_info {
	u8	num_leds;
};

struct pca9532_data {
	struct i2c_client *client;
	struct pca9532_led leds[16];
	struct mutex update_lock;
	/* Protects beep_pwm between the event callback and workqueue. */
	spinlock_t beep_lock;
	u8 beep_pwm;
	struct input_dev *idev;
	struct work_struct work;
#ifdef CONFIG_LEDS_PCA9532_GPIO
	struct gpio_chip gpio;
#endif
	const struct pca9532_chip_info *chip_info;

#define PCA9532_PWM_ID_0	0
#define PCA9532_PWM_ID_1	1
	u8 pwm[2];
	u8 psc[2];
	bool hw_blink;
};

static int pca9532_probe(struct i2c_client *client);
static void pca9532_remove(struct i2c_client *client);

enum {
	pca9530,
	pca9531,
	pca9532,
	pca9533,
};

static const struct i2c_device_id pca9532_id[] = {
	{ .name = "pca9530", .driver_data = pca9530 },
	{ .name = "pca9531", .driver_data = pca9531 },
	{ .name = "pca9532", .driver_data = pca9532 },
	{ .name = "pca9533", .driver_data = pca9533 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, pca9532_id);

static const struct pca9532_chip_info pca9532_chip_info_tbl[] = {
	[pca9530] = {
		.num_leds = 2,
	},
	[pca9531] = {
		.num_leds = 8,
	},
	[pca9532] = {
		.num_leds = 16,
	},
	[pca9533] = {
		.num_leds = 4,
	},
};

#ifdef CONFIG_OF
static const struct of_device_id of_pca9532_leds_match[] = {
	{ .compatible = "nxp,pca9530", .data = (void *)pca9530 },
	{ .compatible = "nxp,pca9531", .data = (void *)pca9531 },
	{ .compatible = "nxp,pca9532", .data = (void *)pca9532 },
	{ .compatible = "nxp,pca9533", .data = (void *)pca9533 },
	{},
};

MODULE_DEVICE_TABLE(of, of_pca9532_leds_match);
#endif

static struct i2c_driver pca9532_driver = {
	.driver = {
		.name = "leds-pca953x",
		.of_match_table = of_match_ptr(of_pca9532_leds_match),
	},
	.probe = pca9532_probe,
	.remove = pca9532_remove,
	.id_table = pca9532_id,
};

/* We have two pwm/blinkers, but 16 possible leds to drive. Additionally,
 * the clever Thecus people are using one pwm to drive the beeper. So,
 * as a compromise we average one pwm to the values requested by all
 * leds that are not ON/OFF.
 * */
static int pca9532_calcpwm(struct i2c_client *client, int pwm, int blink,
	enum led_brightness value)
{
	int a = 0, b = 0, i = 0;
	struct pca9532_data *data = i2c_get_clientdata(client);
	for (i = 0; i < data->chip_info->num_leds; i++) {
		if (data->leds[i].type == PCA9532_TYPE_LED &&
			data->leds[i].state == PCA9532_PWM0+pwm) {
				a++;
				b += data->leds[i].ldev.brightness;
		}
	}
	if (a == 0) {
		dev_err(&client->dev,
		"fear of division by zero %d/%d, wanted %d\n",
			b, a, value);
		return -EINVAL;
	}
	b = b/a;
	if (b > 0xFF)
		return -EINVAL;
	data->pwm[pwm] = b;
	data->psc[pwm] = blink;
	return 0;
}

static int __pca9532_setpwm(struct i2c_client *client, int pwm)
{
	struct pca9532_data *data = i2c_get_clientdata(client);
	u8 maxleds = data->chip_info->num_leds;
	int ret;

	if (pwm < 0 || pwm > 1)
		return -EINVAL;

	ret = i2c_smbus_write_byte_data(client,
					PCA9532_REG_PWM(maxleds, pwm),
					data->pwm[pwm]);
	if (ret < 0) {
		dev_warn(&client->dev,
			 "Failed to write PWM register: %d\n", ret);
		return ret;
	}

	ret = i2c_smbus_write_byte_data(client,
					PCA9532_REG_PSC(maxleds, pwm),
					data->psc[pwm]);
	if (ret < 0)
		dev_warn(&client->dev,
			 "Failed to write PSC register: %d\n", ret);

	return ret;
}
/* Set LED routing */
static int __pca9532_setled(struct pca9532_led *led)
{
	struct i2c_client *client = led->client;
	struct pca9532_data *data = i2c_get_clientdata(client);
	u8 maxleds = data->chip_info->num_leds;
	int reg;
	int ret;

	reg = i2c_smbus_read_byte_data(client, LED_REG(maxleds, led->id));
	if (reg < 0) {
		ret = reg;
		dev_warn(&client->dev, "Failed to read LED register: %d\n", ret);
		return ret;
	}

	/* zero led bits */
	reg &= ~LED_MASK(led->id);

	/* set the new value */
	reg |= led->state << LED_SHIFT(led->id);

	ret = i2c_smbus_write_byte_data(client, LED_REG(maxleds, led->id), reg);

	if (ret < 0)
		dev_warn(&client->dev, "Failed to write LED register: %d\n", ret);

	return ret;
}

static int pca9532_setled(struct pca9532_led *led)
{
	struct pca9532_data *data = i2c_get_clientdata(led->client);
	int ret;

	mutex_lock(&data->update_lock);
	ret = __pca9532_setled(led);
	mutex_unlock(&data->update_lock);

	return ret;
}

static int pca9532_set_brightness(struct led_classdev *led_cdev,
				  enum led_brightness value)
{
	struct pca9532_led *led = ldev_to_led(led_cdev);
	struct pca9532_data *data = i2c_get_clientdata(led->client);
	int old_state;
	u8 old_pwm;
	u8 old_psc;
	bool pwm_updated = false;
	int restore_err;
	int err = 0;

	mutex_lock(&data->update_lock);
	old_state = led->state;
	old_pwm = data->pwm[PCA9532_PWM_ID_0];
	old_psc = data->psc[PCA9532_PWM_ID_0];

	if (value == LED_OFF) {
		led->state = PCA9532_OFF;
	} else if (led->state == PCA9532_PWM1) {
		goto out;
	} else if (value == LED_FULL) {
		led->state = PCA9532_ON;
	} else {
		led->state = PCA9532_PWM0;
		err = pca9532_calcpwm(led->client, PCA9532_PWM_ID_0,
				      0, value);
		if (err)
			goto restore_state;
	}

	if (led->state == PCA9532_PWM0) {
		pwm_updated = true;
		err = __pca9532_setpwm(led->client, PCA9532_PWM_ID_0);
		if (err < 0)
			goto restore_state;
	}

	err = __pca9532_setled(led);
	if (err < 0)
		goto restore_state;

	goto out;

restore_state:
	led->state = old_state;
	data->pwm[PCA9532_PWM_ID_0] = old_pwm;
	data->psc[PCA9532_PWM_ID_0] = old_psc;

	if (pwm_updated) {
		restore_err = __pca9532_setpwm(led->client,
					       PCA9532_PWM_ID_0);
		if (restore_err < 0)
			dev_warn(&led->client->dev,
				 "Failed to restore PWM settings: %d\n",
				 restore_err);
	}
out:
	mutex_unlock(&data->update_lock);
	return err;
}

static int pca9532_update_hw_blink(struct pca9532_led *led,
				   unsigned long delay_on,
				   unsigned long delay_off)
{
	struct pca9532_data *data = i2c_get_clientdata(led->client);
	unsigned int psc;
	unsigned long period;
	u8 old_pwm = data->pwm[PCA9532_PWM_ID_1];
	u8 old_psc = data->psc[PCA9532_PWM_ID_1];
	int i, ret;

	/* Look for others LEDs that already use PWM1 */
	for (i = 0; i < data->chip_info->num_leds; i++) {
		struct pca9532_led *other = &data->leds[i];

		if (other == led)
			continue;

		if (other->state == PCA9532_PWM1) {
			if (other->ldev.blink_delay_on != delay_on ||
			    other->ldev.blink_delay_off != delay_off) {
				/* HW can handle only one blink configuration at a time */
				return -EINVAL;
			}
		}
	}

	if (delay_on > ULONG_MAX - delay_off)
		return -EINVAL;

	period = delay_on + delay_off;

	if (!delay_on || !delay_off)
		return -EINVAL;

	if (period > ((U8_MAX + 1) * 1000) /
		     PCA9532_PWM_PERIOD_DIV)
		return -EINVAL;

	psc = (period * PCA9532_PWM_PERIOD_DIV - 1) / 1000;

	data->psc[PCA9532_PWM_ID_1] = psc;
	data->pwm[PCA9532_PWM_ID_1] =
		(delay_on * PCA9532_PWM_DUTY_DIV) / period;

	ret = __pca9532_setpwm(data->client, PCA9532_PWM_ID_1);
	if (ret < 0) {
		data->pwm[PCA9532_PWM_ID_1] = old_pwm;
		data->psc[PCA9532_PWM_ID_1] = old_psc;

		if (__pca9532_setpwm(data->client, PCA9532_PWM_ID_1) < 0)
			dev_warn(&data->client->dev,
				 "Failed to restore PWM settings\n");

		return ret;
	}

	led->state = PCA9532_PWM1;

	return 0;
}

static int pca9532_set_blink(struct led_classdev *led_cdev,
			     unsigned long *delay_on,
			     unsigned long *delay_off)
{
	struct pca9532_led *led = ldev_to_led(led_cdev);
	struct i2c_client *client = led->client;
	struct pca9532_data *data = i2c_get_clientdata(client);
	int old_state;
	int restore_err;
	u8 old_pwm;
	u8 old_psc;
	int err;

	mutex_lock(&data->update_lock);

	old_state = led->state;
	old_pwm = data->pwm[PCA9532_PWM_ID_1];
	old_psc = data->psc[PCA9532_PWM_ID_1];

	if (!data->hw_blink) {
		err = -EINVAL;
		goto out;
	}

	if (*delay_on == 0 && *delay_off == 0) {
		/* led subsystem ask us for a blink rate */
		*delay_on = 500;
		*delay_off = 500;
	}

	err = pca9532_update_hw_blink(led, *delay_on, *delay_off);
	if (err)
		goto restore_state;

	err = __pca9532_setled(led);
	if (err < 0) {
		led->state = old_state;
		data->pwm[PCA9532_PWM_ID_1] = old_pwm;
		data->psc[PCA9532_PWM_ID_1] = old_psc;

		restore_err = __pca9532_setpwm(client, PCA9532_PWM_ID_1);
		if (restore_err < 0)
			dev_warn(&client->dev,
				 "Failed to restore PWM settings: %d\n",
				 restore_err);
	}

	goto out;

restore_state:
	led->state = old_state;
out:
	mutex_unlock(&data->update_lock);
	return err;
}

static int pca9532_event(struct input_dev *dev, unsigned int type,
			 unsigned int code, int value)
{
	struct pca9532_data *data = input_get_drvdata(dev);
	unsigned long flags;

	if (!(type == EV_SND && (code == SND_BELL || code == SND_TONE)))
		return -1;

	spin_lock_irqsave(&data->beep_lock, flags);

	if (value > 1 && value < 32767)
		data->beep_pwm = 127;
	else
		data->beep_pwm = 0;

	spin_unlock_irqrestore(&data->beep_lock, flags);

	schedule_work(&data->work);

	return 0;
}

static void pca9532_input_work(struct work_struct *work)
{
	struct pca9532_data *data =
		container_of(work, struct pca9532_data, work);
	u8 maxleds = data->chip_info->num_leds;
	unsigned long flags;
	u8 old_pwm;
	u8 pwm;
	int ret;

	spin_lock_irqsave(&data->beep_lock, flags);
	pwm = data->beep_pwm;
	spin_unlock_irqrestore(&data->beep_lock, flags);

	mutex_lock(&data->update_lock);

	old_pwm = data->pwm[PCA9532_PWM_ID_1];
	data->pwm[PCA9532_PWM_ID_1] = pwm;

	ret = i2c_smbus_write_byte_data(data->client,
					PCA9532_REG_PWM(maxleds, 1), pwm);
	if (ret < 0) {
		data->pwm[PCA9532_PWM_ID_1] = old_pwm;
		dev_warn(&data->client->dev,
			 "Failed to write PWM register: %d\n", ret);
	}

	mutex_unlock(&data->update_lock);
}

static int pca9532_getled(struct pca9532_led *led)
{
	struct i2c_client *client = led->client;
	struct pca9532_data *data = i2c_get_clientdata(client);
	u8 maxleds = data->chip_info->num_leds;
	int reg;
	int ret;

	mutex_lock(&data->update_lock);
	reg = i2c_smbus_read_byte_data(client, LED_REG(maxleds, led->id));

	if (reg >= 0) {
		ret = (reg & LED_MASK(led->id)) >> LED_SHIFT(led->id);
	} else {
		ret = reg;
		dev_warn(&client->dev, "Failed to read LED register: %d\n", ret);
	}
	mutex_unlock(&data->update_lock);
	return ret;
}

#ifdef CONFIG_LEDS_PCA9532_GPIO
static int pca9532_gpio_request_pin(struct gpio_chip *gc, unsigned offset)
{
	struct pca9532_data *data = gpiochip_get_data(gc);
	struct pca9532_led *led = &data->leds[offset];

	if (led->type == PCA9532_TYPE_GPIO)
		return 0;

	return -EBUSY;
}

static int pca9532_gpio_set_value(struct gpio_chip *gc, unsigned int offset,
				  int val)
{
	struct pca9532_data *data = gpiochip_get_data(gc);
	struct pca9532_led *led = &data->leds[offset];
	int old_state;
	int ret;

	mutex_lock(&data->update_lock);
	old_state = led->state;

	if (val)
		led->state = PCA9532_OFF;
	else
		led->state = PCA9532_ON;

	ret = __pca9532_setled(led);
	if (ret < 0)
		led->state = old_state;

	mutex_unlock(&data->update_lock);

	return ret;
}

static int pca9532_gpio_get_value(struct gpio_chip *gc, unsigned offset)
{
	struct pca9532_data *data = gpiochip_get_data(gc);
	int reg;

	reg = i2c_smbus_read_byte_data(data->client, PCA9532_REG_INPUT(offset));

	if (reg < 0)
		return reg;

	return !!(reg & (1 << (offset % 8)));
}

static int pca9532_gpio_direction_input(struct gpio_chip *gc, unsigned offset)
{
	return pca9532_gpio_set_value(gc, offset, 1);
}

static int pca9532_gpio_direction_output(struct gpio_chip *gc, unsigned offset, int val)
{
	return pca9532_gpio_set_value(gc, offset, val);
}
#endif /* CONFIG_LEDS_PCA9532_GPIO */

static void pca9532_destroy_devices(struct pca9532_data *data, int n_devs)
{
	int i = n_devs;

	while (--i >= 0) {
		switch (data->leds[i].type) {
		case PCA9532_TYPE_NONE:
		case PCA9532_TYPE_GPIO:
			break;
		case PCA9532_TYPE_LED:
			led_classdev_unregister(&data->leds[i].ldev);
			break;
		case PCA9532_TYPE_N2100_BEEP:
			if (data->idev != NULL) {
				cancel_work_sync(&data->work);
				data->idev = NULL;
			}
			break;
		}
	}

#ifdef CONFIG_LEDS_PCA9532_GPIO
	if (data->gpio.parent)
		gpiochip_remove(&data->gpio);
#endif
}

static int pca9532_configure(struct i2c_client *client,
	struct pca9532_data *data, struct pca9532_platform_data *pdata)
{
	int i, err = 0;
#ifdef CONFIG_LEDS_PCA9532_GPIO
	int gpios = 0;
#endif
	u8 maxleds = data->chip_info->num_leds;

	for (i = 0; i < 2; i++) {
		data->pwm[i] = pdata->pwm[i];
		data->psc[i] = pdata->psc[i];

		err = i2c_smbus_write_byte_data(client,
						PCA9532_REG_PWM(maxleds, i),
						data->pwm[i]);
		if (err < 0)
			return err;

		err = i2c_smbus_write_byte_data(client,
						PCA9532_REG_PSC(maxleds, i),
						data->psc[i]);
		if (err < 0)
			return err;
	}

	data->hw_blink = true;

	for (i = 0; i < data->chip_info->num_leds; i++) {
		struct pca9532_led *led = &data->leds[i];
		struct pca9532_led *pled = &pdata->leds[i];

		led->client = client;
		led->id = i;
		led->type = pled->type;

		switch (led->type) {
		case PCA9532_TYPE_NONE:
			break;
		case PCA9532_TYPE_GPIO:
#ifdef CONFIG_LEDS_PCA9532_GPIO
			gpios++;
#endif
			break;
		case PCA9532_TYPE_LED:
			if (pled->state == PCA9532_KEEP) {
				err = pca9532_getled(led);
				if (err < 0)
					goto exit;

				led->state = err;
			} else {
				led->state = pled->state;
			}

			led->name = pled->name;
			led->ldev.name = led->name;
			led->ldev.default_trigger = pled->default_trigger;
			led->ldev.brightness = LED_OFF;
			led->ldev.brightness_set_blocking =
						pca9532_set_brightness;
			led->ldev.blink_set = pca9532_set_blink;

			err = pca9532_setled(led);
			if (err < 0) {
				dev_err(&client->dev,
					"couldn't set LED %s state\n",
					led->name);
				goto exit;
			}

			err = led_classdev_register(&client->dev, &led->ldev);
			if (err < 0) {
				dev_err(&client->dev,
					"couldn't register LED %s\n",
					led->name);
				goto exit;
			}
			break;
		case PCA9532_TYPE_N2100_BEEP:
			/* PWM1 is reserved for beeper so blink will not use hardware */
			data->hw_blink = false;
			BUG_ON(data->idev);

			led->state = PCA9532_PWM1;
			err = pca9532_setled(led);
			if (err < 0) {
				dev_err(&client->dev,
					"couldn't set beeper state\n");
				goto exit;
			}

			data->idev = devm_input_allocate_device(&client->dev);
			if (data->idev == NULL) {
				err = -ENOMEM;
				goto exit;
			}

			data->idev->name = pled->name;
			data->idev->phys = "i2c/pca9532";
			data->idev->id.bustype = BUS_HOST;
			data->idev->id.vendor = 0x001f;
			data->idev->id.product = 0x0001;
			data->idev->id.version = 0x0100;
			data->idev->evbit[0] = BIT_MASK(EV_SND);
			data->idev->sndbit[0] = BIT_MASK(SND_BELL) |
						BIT_MASK(SND_TONE);
			data->idev->event = pca9532_event;
			input_set_drvdata(data->idev, data);
			INIT_WORK(&data->work, pca9532_input_work);

			err = input_register_device(data->idev);
			if (err) {
				cancel_work_sync(&data->work);
				data->idev = NULL;
				goto exit;
			}
			break;
		}
	}

#ifdef CONFIG_LEDS_PCA9532_GPIO
	if (gpios) {
		data->gpio.label = "gpio-pca9532";
		data->gpio.direction_input = pca9532_gpio_direction_input;
		data->gpio.direction_output = pca9532_gpio_direction_output;
		data->gpio.set = pca9532_gpio_set_value;
		data->gpio.get = pca9532_gpio_get_value;
		data->gpio.request = pca9532_gpio_request_pin;
		data->gpio.can_sleep = 1;
		data->gpio.base = pdata->gpio_base;
		data->gpio.ngpio = data->chip_info->num_leds;
		data->gpio.parent = &client->dev;
		data->gpio.owner = THIS_MODULE;

		err = gpiochip_add_data(&data->gpio, data);
		if (err) {
			/* Use data->gpio.dev as a flag for freeing gpiochip */
			data->gpio.parent = NULL;
			dev_warn(&client->dev, "could not add gpiochip\n");
		} else {
			dev_info(&client->dev, "gpios %i...%i\n",
				 data->gpio.base, data->gpio.base +
				 data->gpio.ngpio - 1);
		}
	}
#endif

	return 0;

exit:
	pca9532_destroy_devices(data, i);
	return err;
}

static struct pca9532_platform_data *
pca9532_of_populate_pdata(struct device *dev, struct device_node *np)
{
	struct pca9532_platform_data *pdata;
	int devid, maxleds;
	int i = 0;
	const char *state;

	devid = (int)(uintptr_t)of_device_get_match_data(dev);
	maxleds = pca9532_chip_info_tbl[devid].num_leds;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->gpio_base = -1;

	of_property_read_u8_array(np, "nxp,pwm", &pdata->pwm[PCA9532_PWM_ID_0],
				  ARRAY_SIZE(pdata->pwm));
	of_property_read_u8_array(np, "nxp,psc", &pdata->psc[PCA9532_PWM_ID_0],
				  ARRAY_SIZE(pdata->psc));

	for_each_available_child_of_node_scoped(np, child) {
		if (of_property_read_string(child, "label",
					    &pdata->leds[i].name))
			pdata->leds[i].name = child->name;
		of_property_read_u32(child, "type", &pdata->leds[i].type);
		of_property_read_string(child, "linux,default-trigger",
					&pdata->leds[i].default_trigger);
		if (!of_property_read_string(child, "default-state", &state)) {
			if (!strcmp(state, "on"))
				pdata->leds[i].state = PCA9532_ON;
			else if (!strcmp(state, "keep"))
				pdata->leds[i].state = PCA9532_KEEP;
		}
		if (++i >= maxleds)
			break;
	}

	return pdata;
}

static int pca9532_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	int devid;
	struct pca9532_data *data = i2c_get_clientdata(client);
	struct pca9532_platform_data *pca9532_pdata =
			dev_get_platdata(&client->dev);
	struct device_node *np = dev_of_node(&client->dev);

	if (!pca9532_pdata) {
		if (np) {
			pca9532_pdata =
				pca9532_of_populate_pdata(&client->dev, np);
			if (IS_ERR(pca9532_pdata))
				return PTR_ERR(pca9532_pdata);
		} else {
			dev_err(&client->dev, "no platform data\n");
			return -EINVAL;
		}
		devid = (int)(uintptr_t)of_device_get_match_data(&client->dev);
	} else {
		devid = id->driver_data;
	}

	if (!i2c_check_functionality(client->adapter,
		I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->chip_info = &pca9532_chip_info_tbl[devid];

	dev_info(&client->dev, "setting platform data\n");
	i2c_set_clientdata(client, data);
	data->client = client;
	mutex_init(&data->update_lock);
	spin_lock_init(&data->beep_lock);

	return pca9532_configure(client, data, pca9532_pdata);
}

static void pca9532_remove(struct i2c_client *client)
{
	struct pca9532_data *data = i2c_get_clientdata(client);

	pca9532_destroy_devices(data, data->chip_info->num_leds);
}

module_i2c_driver(pca9532_driver);

MODULE_AUTHOR("Riku Voipio");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PCA 9532 LED dimmer");
