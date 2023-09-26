// SPDX-License-Identifier: GPL-2.0

#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <uapi/linux/serial.h>

#define LEDTRIG_TTY_INTERVAL	50

struct ledtrig_tty_data {
	struct led_classdev *led_cdev;
	struct delayed_work dwork;
	struct mutex mutex;
	const char *ttyname;
	struct tty_struct *tty;
	int rx, tx;
	unsigned long mode;
#define LEDTRIG_TTY_MODE_TX	0
#define LEDTRIG_TTY_MODE_RX	1
#define LEDTRIG_TTY_MODE_CTS	2
#define LEDTRIG_TTY_MODE_DSR	3
#define LEDTRIG_TTY_MODE_CAR	4
#define LEDTRIG_TTY_MODE_RNG	5
};

enum tty_led_state {
	TTY_LED_BLINK,
	TTY_LED_ENABLE,
	TTY_LED_DISABLE,
};

enum ledtrig_tty_attr {
	LEDTRIG_TTY_ATTR_TX,
	LEDTRIG_TTY_ATTR_RX,
	LEDTRIG_TTY_ATTR_CTS,
	LEDTRIG_TTY_ATTR_DSR,
	LEDTRIG_TTY_ATTR_CAR,
	LEDTRIG_TTY_ATTR_RNG,
};

static void ledtrig_tty_restart(struct ledtrig_tty_data *trigger_data)
{
	schedule_delayed_work(&trigger_data->dwork, 0);
}

static ssize_t ttyname_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct ledtrig_tty_data *trigger_data = led_trigger_get_drvdata(dev);
	ssize_t len = 0;

	mutex_lock(&trigger_data->mutex);

	if (trigger_data->ttyname)
		len = sprintf(buf, "%s\n", trigger_data->ttyname);

	mutex_unlock(&trigger_data->mutex);

	return len;
}

static ssize_t ttyname_store(struct device *dev,
			     struct device_attribute *attr, const char *buf,
			     size_t size)
{
	struct ledtrig_tty_data *trigger_data = led_trigger_get_drvdata(dev);
	char *ttyname;
	ssize_t ret = size;
	bool running;

	if (size > 0 && buf[size - 1] == '\n')
		size -= 1;

	if (size) {
		ttyname = kmemdup_nul(buf, size, GFP_KERNEL);
		if (!ttyname)
			return -ENOMEM;
	} else {
		ttyname = NULL;
	}

	mutex_lock(&trigger_data->mutex);

	running = trigger_data->ttyname != NULL;

	kfree(trigger_data->ttyname);
	tty_kref_put(trigger_data->tty);
	trigger_data->tty = NULL;

	trigger_data->ttyname = ttyname;

	mutex_unlock(&trigger_data->mutex);

	if (ttyname && !running)
		ledtrig_tty_restart(trigger_data);

	return ret;
}
static DEVICE_ATTR_RW(ttyname);

static ssize_t ledtrig_tty_attr_show(struct device *dev, char *buf,
	enum ledtrig_tty_attr attr)
{
	struct ledtrig_tty_data *trigger_data = led_trigger_get_drvdata(dev);
	int bit;

	switch (attr) {
	case LEDTRIG_TTY_ATTR_TX:
		bit = LEDTRIG_TTY_MODE_TX;
		break;
	case LEDTRIG_TTY_ATTR_RX:
		bit = LEDTRIG_TTY_MODE_RX;
		break;
	case LEDTRIG_TTY_ATTR_CTS:
		bit = LEDTRIG_TTY_MODE_CTS;
		break;
	case LEDTRIG_TTY_ATTR_DSR:
		bit = LEDTRIG_TTY_MODE_DSR;
		break;
	case LEDTRIG_TTY_ATTR_CAR:
		bit = LEDTRIG_TTY_MODE_CAR;
		break;
	case LEDTRIG_TTY_ATTR_RNG:
		bit = LEDTRIG_TTY_MODE_RNG;
		break;
	default:
		return -EINVAL;
	}

	return sprintf(buf, "%u\n", test_bit(bit, &trigger_data->mode));
}

static ssize_t ledtrig_tty_attr_store(struct device *dev, const char *buf,
	size_t size, enum ledtrig_tty_attr attr)
{
	struct ledtrig_tty_data *trigger_data = led_trigger_get_drvdata(dev);
	unsigned long state;
	int ret;
	int bit;

	ret = kstrtoul(buf, 0, &state);
	if (ret)
		return ret;

	switch (attr) {
	case LEDTRIG_TTY_ATTR_TX:
		bit = LEDTRIG_TTY_MODE_TX;
		break;
	case LEDTRIG_TTY_ATTR_RX:
		bit = LEDTRIG_TTY_MODE_RX;
		break;
	case LEDTRIG_TTY_ATTR_CTS:
		bit = LEDTRIG_TTY_MODE_CTS;
		break;
	case LEDTRIG_TTY_ATTR_DSR:
		bit = LEDTRIG_TTY_MODE_DSR;
		break;
	case LEDTRIG_TTY_ATTR_CAR:
		bit = LEDTRIG_TTY_MODE_CAR;
		break;
	case LEDTRIG_TTY_ATTR_RNG:
		bit = LEDTRIG_TTY_MODE_RNG;
		break;
	default:
		return -EINVAL;
	}

	if (state)
		set_bit(bit, &trigger_data->mode);
	else
		clear_bit(bit, &trigger_data->mode);

	return size;
}

static ssize_t tx_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return ledtrig_tty_attr_show(dev, buf, LEDTRIG_TTY_ATTR_TX);
}

static ssize_t tx_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	return ledtrig_tty_attr_store(dev, buf, size, LEDTRIG_TTY_ATTR_TX);
}
static DEVICE_ATTR_RW(tx);

static ssize_t rx_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return ledtrig_tty_attr_show(dev, buf, LEDTRIG_TTY_ATTR_RX);
}

static ssize_t rx_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	return ledtrig_tty_attr_store(dev, buf, size, LEDTRIG_TTY_ATTR_RX);
}
static DEVICE_ATTR_RW(rx);

static ssize_t line_cts_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return ledtrig_tty_attr_show(dev, buf, LEDTRIG_TTY_ATTR_CTS);
}

static ssize_t line_cts_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	return ledtrig_tty_attr_store(dev, buf, size, LEDTRIG_TTY_ATTR_CTS);
}
static DEVICE_ATTR_RW(line_cts);

static ssize_t line_dsr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return ledtrig_tty_attr_show(dev, buf, LEDTRIG_TTY_ATTR_DSR);
}

static ssize_t line_dsr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	return ledtrig_tty_attr_store(dev, buf, size, LEDTRIG_TTY_ATTR_DSR);
}
static DEVICE_ATTR_RW(line_dsr);

static ssize_t line_car_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return ledtrig_tty_attr_show(dev, buf, LEDTRIG_TTY_ATTR_CAR);
}

static ssize_t line_car_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	return ledtrig_tty_attr_store(dev, buf, size, LEDTRIG_TTY_ATTR_CAR);
}
static DEVICE_ATTR_RW(line_car);

static ssize_t line_rng_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return ledtrig_tty_attr_show(dev, buf, LEDTRIG_TTY_ATTR_RNG);
}

static ssize_t line_rng_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	return ledtrig_tty_attr_store(dev, buf, size, LEDTRIG_TTY_ATTR_RNG);
}
static DEVICE_ATTR_RW(line_rng);


static int ledtrig_tty_flag(struct ledtrig_tty_data *trigger_data, unsigned int flag)
{
	unsigned int status;
	int ret;

	status = tty_get_mget(trigger_data->tty);
	if (status & flag)
		ret = 1;
	else
		ret = 0;

	return ret;
}

static void ledtrig_tty_work(struct work_struct *work)
{
	struct ledtrig_tty_data *trigger_data =
		container_of(work, struct ledtrig_tty_data, dwork.work);
	struct led_classdev *led_cdev = trigger_data->led_cdev;
	struct serial_icounter_struct icount;
	enum tty_led_state state;
	int ret;

	state = TTY_LED_DISABLE;
	mutex_lock(&trigger_data->mutex);

	if (!trigger_data->ttyname) {
		/* exit without rescheduling */
		mutex_unlock(&trigger_data->mutex);
		return;
	}

	/* try to get the tty corresponding to $ttyname */
	if (!trigger_data->tty) {
		dev_t devno;
		struct tty_struct *tty;
		int ret;

		ret = tty_dev_name_to_number(trigger_data->ttyname, &devno);
		if (ret < 0)
			/*
			 * A device with this name might appear later, so keep
			 * retrying.
			 */
			goto out;

		tty = tty_kopen_shared(devno);
		if (IS_ERR(tty) || !tty)
			/* What to do? retry or abort */
			goto out;

		trigger_data->tty = tty;
	}

	if (test_bit(LEDTRIG_TTY_MODE_CTS, &trigger_data->mode)) {
		ret = ledtrig_tty_flag(trigger_data, TIOCM_CTS);
		if (ret)
			state = TTY_LED_ENABLE;
	}

	if (test_bit(LEDTRIG_TTY_MODE_DSR, &trigger_data->mode)) {
		ret = ledtrig_tty_flag(trigger_data, TIOCM_DSR);
		if (ret)
			state = TTY_LED_ENABLE;
	}

	if (test_bit(LEDTRIG_TTY_MODE_CAR, &trigger_data->mode)) {
		ret = ledtrig_tty_flag(trigger_data, TIOCM_CAR);
		if (ret)
			state = TTY_LED_ENABLE;
	}

	if (test_bit(LEDTRIG_TTY_MODE_RNG, &trigger_data->mode)) {
		ret = ledtrig_tty_flag(trigger_data, TIOCM_RNG);
		if (ret)
			state = TTY_LED_ENABLE;
	}

	/* The rx/tx handling must come after the evaluation of TIOCM_*,
	 * since the display for rx/tx has priority
	 */
	if (test_bit(LEDTRIG_TTY_MODE_TX, &trigger_data->mode) ||
	    test_bit(LEDTRIG_TTY_MODE_RX, &trigger_data->mode)) {
		ret = tty_get_icount(trigger_data->tty, &icount);
		if (ret) {
			dev_info(trigger_data->tty->dev, "Failed to get icount, stopped polling\n");
			mutex_unlock(&trigger_data->mutex);
			return;
		}

		if (test_bit(LEDTRIG_TTY_MODE_TX, &trigger_data->mode) &&
		    (icount.tx != trigger_data->tx)) {
			trigger_data->tx = icount.tx;
			state = TTY_LED_BLINK;
		}

		if (test_bit(LEDTRIG_TTY_MODE_RX, &trigger_data->mode) &&
		    (icount.rx != trigger_data->rx)) {
			trigger_data->rx = icount.rx;
			state = TTY_LED_BLINK;
		}
	}

	switch (state) {
	case TTY_LED_BLINK:
		unsigned long interval = LEDTRIG_TTY_INTERVAL;
		led_blink_set_oneshot(trigger_data->led_cdev, &interval,
				      &interval, 0);
		break;
	case TTY_LED_ENABLE:
		led_set_brightness(led_cdev, led_cdev->blink_brightness);
		break;
	case TTY_LED_DISABLE:
		fallthrough;
	default:
		led_set_brightness(led_cdev, 0);
		break;
	}

out:
	mutex_unlock(&trigger_data->mutex);
	schedule_delayed_work(&trigger_data->dwork,
			      msecs_to_jiffies(LEDTRIG_TTY_INTERVAL * 2));
}

static struct attribute *ledtrig_tty_attrs[] = {
	&dev_attr_ttyname.attr,
	&dev_attr_rx.attr,
	&dev_attr_tx.attr,
	&dev_attr_line_cts.attr,
	&dev_attr_line_dsr.attr,
	&dev_attr_line_car.attr,
	&dev_attr_line_rng.attr,
	NULL
};
ATTRIBUTE_GROUPS(ledtrig_tty);

static int ledtrig_tty_activate(struct led_classdev *led_cdev)
{
	struct ledtrig_tty_data *trigger_data;

	trigger_data = kzalloc(sizeof(*trigger_data), GFP_KERNEL);
	if (!trigger_data)
		return -ENOMEM;

	/* Enable default rx/tx LED blink */
	set_bit(LEDTRIG_TTY_MODE_TX, &trigger_data->mode);
	set_bit(LEDTRIG_TTY_MODE_RX, &trigger_data->mode);

	led_set_trigger_data(led_cdev, trigger_data);

	INIT_DELAYED_WORK(&trigger_data->dwork, ledtrig_tty_work);
	trigger_data->led_cdev = led_cdev;
	mutex_init(&trigger_data->mutex);

	return 0;
}

static void ledtrig_tty_deactivate(struct led_classdev *led_cdev)
{
	struct ledtrig_tty_data *trigger_data = led_get_trigger_data(led_cdev);

	cancel_delayed_work_sync(&trigger_data->dwork);

	kfree(trigger_data);
}

static struct led_trigger ledtrig_tty = {
	.name = "tty",
	.activate = ledtrig_tty_activate,
	.deactivate = ledtrig_tty_deactivate,
	.groups = ledtrig_tty_groups,
};
module_led_trigger(ledtrig_tty);

MODULE_AUTHOR("Uwe Kleine-König <u.kleine-koenig@pengutronix.de>");
MODULE_DESCRIPTION("UART LED trigger");
MODULE_LICENSE("GPL v2");
