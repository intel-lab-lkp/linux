// SPDX-License-Identifier: GPL-2.0-only
/* Lenovo ThinkPad X13s embedded controller */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/leds.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/slab.h>

#define X13S_EC_CMD_READ		0x02
#define X13S_EC_CMD_WRITE		0x03
#define X13S_EC_CMD_EVENT		0xf0
#define X13S_EC_REG_POWER_STATE	0x80
#define X13S_EC_POWER_STATE_ENTER	0x55
#define X13S_EC_POWER_STATE_EXIT	0xaa
#define X13S_EC_REG_KBD_BACKLIGHT	0xc0
#define X13S_EC_BACKLIGHT_MASK	GENMASK(5, 4)
#define X13S_EC_REG_KBD_BACKLIGHT_SAVED	0x15
#define X13S_EC_BACKLIGHT_SAVED_MASK	GENMASK(2, 1)
#define X13S_EC_EVENT_FN_SPACE	0x1f
#define X13S_EC_EVENT_LID_OPEN	0x52

struct x13s_ec {
	struct i2c_client *client;
	struct gpio_desc *power_state;
	/* Serializes EC transactions, backlight RMW and suspend/event state. */
	struct mutex lock;
	struct led_classdev led;
	enum led_brightness saved_brightness;
	bool suspended;
	bool event_pending;
};

/*
 * Each single-message transfer ends in STOP. Hold the bus segment across
 * both halves so another client cannot interleave them. Keep the 10 ms
 * post-command settling delay, including on failure.
 */
static int x13s_read(struct x13s_ec *ec, u8 cmd, u8 reg, u8 *value)
{
	struct i2c_client *client = ec->client;
	u8 buf[] = { cmd, reg, 0, 1 };
	struct i2c_msg request = {
		.addr = client->addr,
		.flags = I2C_M_STOP,
		.len = sizeof(buf),
		.buf = buf,
	};
	struct i2c_msg response = {
		.addr = client->addr,
		.flags = I2C_M_RD,
		.len = 1,
		.buf = value,
	};
	int ret;

	lockdep_assert_held(&ec->lock);
	if (ec->suspended)
		return -EBUSY;

	i2c_lock_bus(client->adapter, I2C_LOCK_SEGMENT);
	ret = __i2c_transfer(client->adapter, &request, 1);
	if (ret == 1)
		ret = __i2c_transfer(client->adapter, &response, 1);
	ret = ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
	i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
	fsleep(10000);
	return ret;
}

/* Also used by normal PM callbacks while ordinary EC access is blocked. */
static int x13s_write(struct x13s_ec *ec, u8 reg, u8 value)
{
	u8 buf[] = { X13S_EC_CMD_WRITE, reg, 0, 1, value };
	int ret;

	lockdep_assert_held(&ec->lock);

	ret = i2c_master_send(ec->client, buf, sizeof(buf));
	ret = ret == sizeof(buf) ? 0 : (ret < 0 ? ret : -EIO);
	fsleep(10000);
	return ret;
}

static int x13s_brightness(struct x13s_ec *ec)
{
	u8 value;
	int ret;

	ret = x13s_read(ec, X13S_EC_CMD_READ, X13S_EC_REG_KBD_BACKLIGHT, &value);
	if (ret)
		return ret;

	ret = FIELD_GET(X13S_EC_BACKLIGHT_MASK, value);
	return ret == 3 ? -EINVAL : ret;
}

static enum led_brightness x13s_brightness_get(struct led_classdev *led)
{
	struct x13s_ec *ec = container_of(led, struct x13s_ec, led);
	int ret;

	mutex_lock(&ec->lock);
	ret = x13s_brightness(ec);
	mutex_unlock(&ec->lock);
	return ret;
}

static int x13s_set_brightness_locked(struct x13s_ec *ec,
				      enum led_brightness brightness)
{
	u8 old, new;
	int ret;

	lockdep_assert_held(&ec->lock);
	if (brightness > 2)
		return -EINVAL;

	ret = x13s_read(ec, X13S_EC_CMD_READ, X13S_EC_REG_KBD_BACKLIGHT, &old);
	if (ret)
		return ret;
	/* Never interpret or overwrite a reserved current brightness. */
	if (FIELD_GET(X13S_EC_BACKLIGHT_MASK, old) == 3)
		return -EINVAL;
	new = (old & ~X13S_EC_BACKLIGHT_MASK) |
		FIELD_PREP(X13S_EC_BACKLIGHT_MASK, brightness);
	return x13s_write(ec, X13S_EC_REG_KBD_BACKLIGHT, new);
}

/* SCMS(0x20): save the brightness obtained from C0 in the EC. */
static int x13s_save_brightness_locked(struct x13s_ec *ec,
				       enum led_brightness brightness)
{
	u8 value;
	int ret;

	lockdep_assert_held(&ec->lock);
	ret = x13s_read(ec, X13S_EC_CMD_READ, X13S_EC_REG_KBD_BACKLIGHT_SAVED,
			&value);
	if (ret)
		return ret;

	value &= ~X13S_EC_BACKLIGHT_SAVED_MASK;
	value |= FIELD_PREP(X13S_EC_BACKLIGHT_SAVED_MASK, brightness);
	return x13s_write(ec, X13S_EC_REG_KBD_BACKLIGHT_SAVED, value);
}

/* SCMS(0x21): restore C0 from the EC's saved brightness. */
static int x13s_restore_brightness_locked(struct x13s_ec *ec)
{
	u8 value;
	int brightness, ret;

	lockdep_assert_held(&ec->lock);
	ret = x13s_read(ec, X13S_EC_CMD_READ, X13S_EC_REG_KBD_BACKLIGHT_SAVED,
			&value);
	if (ret)
		return ret;

	brightness = FIELD_GET(X13S_EC_BACKLIGHT_SAVED_MASK, value);
	ret = x13s_set_brightness_locked(ec, brightness);
	return ret ? ret : brightness;
}

static int x13s_brightness_set(struct led_classdev *led,
			       enum led_brightness brightness)
{
	struct x13s_ec *ec = container_of(led, struct x13s_ec, led);
	int ret;

	mutex_lock(&ec->lock);
	ret = x13s_set_brightness_locked(ec, brightness);
	/* Keep software selections across the EC's lid blank/restore cycle. */
	if (!ret)
		ret = x13s_save_brightness_locked(ec, brightness);
	mutex_unlock(&ec->lock);
	return ret;
}

static void x13s_refresh_brightness(struct x13s_ec *ec)
{
	int brightness, ret;

	lockdep_assert_held(&ec->lock);
	/* Firmware owns the transition; save its result as _Q1F does. */
	brightness = x13s_brightness(ec);
	if (brightness < 0) {
		dev_err_ratelimited(&ec->client->dev,
				    "Backlight refresh failed: %d\n", brightness);
		return;
	}
	ret = x13s_save_brightness_locked(ec, brightness);
	if (ret)
		dev_err_ratelimited(&ec->client->dev,
				    "Backlight save failed: %d\n", ret);
	led_classdev_notify_brightness_hw_changed(&ec->led, brightness);
}

static void x13s_query_event(struct x13s_ec *ec)
{
	u8 event;
	int ret;

	lockdep_assert_held(&ec->lock);
	/* One query per interrupt, with no event-draining loop or retry. */
	ret = x13s_read(ec, X13S_EC_CMD_EVENT, 0, &event);
	if (ret) {
		dev_err_ratelimited(&ec->client->dev, "Event query failed: %d\n", ret);
		return;
	}
	if (event == X13S_EC_EVENT_FN_SPACE) {
		x13s_refresh_brightness(ec);
	} else if (event == X13S_EC_EVENT_LID_OPEN) {
		ret = x13s_restore_brightness_locked(ec);
		if (ret < 0) {
			dev_err_ratelimited(&ec->client->dev,
					    "Backlight lid restore failed: %d\n", ret);
			return;
		}
		ec->led.brightness = ret;
		led_classdev_notify_brightness_hw_changed(&ec->led, ret);
	}
}

static void x13s_process_pending_event(struct x13s_ec *ec)
{
	lockdep_assert_held(&ec->lock);
	if (ec->event_pending) {
		ec->event_pending = false;
		x13s_query_event(ec);
	}
}

static irqreturn_t x13s_irq(int irq, void *data)
{
	struct x13s_ec *ec = data;

	mutex_lock(&ec->lock);
	/* Coalesce suspended IRQs into one query; never access I2C here. */
	if (ec->suspended)
		ec->event_pending = true;
	else
		x13s_query_event(ec);
	mutex_unlock(&ec->lock);
	return IRQ_HANDLED;
}

static int x13s_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct x13s_ec *ec;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "Missing event IRQ\n");
	if (irq_get_trigger_type(client->irq) != IRQ_TYPE_EDGE_FALLING)
		return dev_err_probe(dev, -EINVAL, "Expected falling-edge IRQ\n");

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;
	ec->client = client;
	ret = devm_mutex_init(dev, &ec->lock);
	if (ret)
		return ret;
	i2c_set_clientdata(client, ec);

	ec->power_state = devm_gpiod_get(dev, "power-state", GPIOD_OUT_HIGH);
	if (IS_ERR(ec->power_state))
		return dev_err_probe(dev, PTR_ERR(ec->power_state),
				     "Failed to acquire power-state GPIO high\n");

	ec->led.name = "platform::kbd_backlight";
	ec->led.max_brightness = 2;
	ec->led.flags = LED_BRIGHT_HW_CHANGED | LED_RETAIN_AT_SHUTDOWN;
	ec->led.brightness_get = x13s_brightness_get;
	ec->led.brightness_set_blocking = x13s_brightness_set;
	ret = devm_led_classdev_register(dev, &ec->led);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register keyboard backlight\n");

	/* Devres synchronizes the IRQ before unregistering the LED. */
	ret = devm_request_threaded_irq(dev, client->irq, NULL, x13s_irq,
					IRQF_ONESHOT, dev_name(dev), ec);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request event IRQ\n");

	/* The I2C core manages the wake IRQ described by wakeup-source. */
	return 0;
}

static int x13s_power_gpio(struct x13s_ec *ec, int value)
{
	int ret;

	lockdep_assert_held(&ec->lock);
	ret = gpiod_set_value_cansleep(ec->power_state, value);
	if (ret)
		dev_err(&ec->client->dev, "Failed to set power-state GPIO to %d: %d\n",
			value, ret);
	return ret;
}

static void x13s_exit_low_power(struct x13s_ec *ec)
{
	int ret;

	lockdep_assert_held(&ec->lock);
	x13s_power_gpio(ec, 1);
	/* DSDT: GPIO176 high, Sleep(10), then register 0x80 <- 0xaa. */
	fsleep(10000);
	ret = x13s_write(ec, X13S_EC_REG_POWER_STATE, X13S_EC_POWER_STATE_EXIT);
	if (ret)
		dev_err(&ec->client->dev, "Failed to exit low power: %d\n", ret);

	/* Resume ordinary access even if the EC exit attempt failed. */
	ec->suspended = false;
}

static int x13s_suspend(struct device *dev)
{
	struct x13s_ec *ec = dev_get_drvdata(dev);
	int ret;

	/* Finish in-flight access while the GENI parent is still usable. */
	mutex_lock(&ec->lock);
	if (ec->suspended)
		goto out;
	/* Keep the previous snapshot if reading the hardware fails. */
	ret = x13s_brightness(ec);
	if (ret < 0)
		dev_err(dev, "Failed to save backlight brightness: %d\n", ret);
	else
		ec->saved_brightness = ret;

	/* DSDT: register 0x80 <- 0x55, then GPIO176 low. */
	ret = x13s_write(ec, X13S_EC_REG_POWER_STATE, X13S_EC_POWER_STATE_ENTER);
	if (ret)
		dev_err(dev, "Failed to enter low power: %d\n", ret);
	else
		x13s_power_gpio(ec, 0);
	/* Defer event queries throughout host suspend, even on EC failure. */
	ec->suspended = true;
out:
	mutex_unlock(&ec->lock);
	return 0;
}

static int x13s_resume(struct device *dev)
{
	struct x13s_ec *ec = dev_get_drvdata(dev);
	int ret;

	/*
	 * Normal resume runs after the adapter and its GENI parent. In
	 * particular, device_resume_early() has re-enabled runtime PM:
	 * GENI's resume_noirq() alone is not sufficient for I2C transfers.
	 * Consume the pending query under the IRQ thread's mutex, without
	 * a worker that could race a new IRQ or the next suspend.
	 */
	mutex_lock(&ec->lock);
	x13s_exit_low_power(ec);
	ret = x13s_set_brightness_locked(ec, ec->saved_brightness);
	if (ret)
		dev_err(dev, "Failed to restore backlight brightness: %d\n", ret);
	else
		/* A software restore is not a hardware brightness change. */
		ec->led.brightness = ec->saved_brightness;

	/* A deferred lid-open restore must supersede the PM snapshot. */
	x13s_process_pending_event(ec);
	mutex_unlock(&ec->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(x13s_pm_ops, x13s_suspend, x13s_resume);

static const struct of_device_id x13s_of_match[] = {
	{ .compatible = "lenovo,thinkpad-x13s-ec" },
	{ }
};
MODULE_DEVICE_TABLE(of, x13s_of_match);

static struct i2c_driver x13s_driver = {
	.probe = x13s_probe,
	.driver = {
		.name = "thinkpad-x13s-ec",
		.of_match_table = x13s_of_match,
		.pm = pm_sleep_ptr(&x13s_pm_ops),
	},
};
module_i2c_driver(x13s_driver);

MODULE_DESCRIPTION("Lenovo ThinkPad X13s Embedded Controller");
MODULE_LICENSE("GPL");
