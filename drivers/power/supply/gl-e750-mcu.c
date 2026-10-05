// SPDX-License-Identifier: GPL-2.0-only
/*
 * GL.iNet GL-E750 (Mudi) status MCU battery driver
 *
 * The MCU sits on the SoC UART. It drives the OLED screen, monitors the
 * battery and watches the power switch. The host sends one JSON object per
 * line, where every value is a string. When a line contains
 * "mcu_status": "1", the MCU answers with
 *
 *	{OK},<percent>,<temperature>,<charging>,<cycles>
 *
 * When the power switch is turned off, the MCU sends {"shut_down": "1"} and
 * cuts the power a few seconds later.
 *
 * Copyright (C) 2026 Serv Pol <servpol@tuta.io>
 */

#include <linux/completion.h>
#include <linux/ctype.h>
#include <linux/devm-helpers.h>
#include <linux/math.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/serdev.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define MCU_BAUDRATE		115200
#define MCU_RX_LINE_MAX		128
#define MCU_POLL_INTERVAL_MS	30000
#define MCU_REPLY_TIMEOUT_MS	1500

static const u8 mcu_status_req[] = "{ \"mcu_status\": \"1\" }\n";

struct gl_e750_mcu {
	struct serdev_device *serdev;
	struct power_supply *psy;
	struct delayed_work poll_work;
	struct work_struct poweroff_work;
	struct completion reply;

	/* receive buffer and battery state, filled from the serdev callback */
	spinlock_t lock;
	char rx_line[MCU_RX_LINE_MAX];
	size_t rx_len;
	bool valid;
	int capacity;		/* percent */
	int temp;		/* tenths of a degree Celsius */
	bool charging;
	int cycles;
};

/* "37.4", "40" or "-1.5" -> tenths of a degree */
static int mcu_parse_tenths(const char *s, int *out)
{
	const char *dot = strchr(s, '.');
	char whole_str[8];
	int whole, frac = 0;

	if (!dot) {
		if (kstrtoint(s, 10, &whole))
			return -EINVAL;
		*out = whole * 10;
		return 0;
	}

	if (dot == s || dot - s >= sizeof(whole_str))
		return -EINVAL;
	memcpy(whole_str, s, dot - s);
	whole_str[dot - s] = '\0';
	if (kstrtoint(whole_str, 10, &whole))
		return -EINVAL;
	if (isdigit(dot[1]))
		frac = dot[1] - '0';
	*out = whole * 10 + (*s == '-' ? -frac : frac);
	return 0;
}

/*
 * "{OK},<percent>,<temperature>,<charging>,<cycles>", called with the lock
 * held. Returns true if the line was a valid status reply.
 */
static bool mcu_parse_status(struct gl_e750_mcu *mcu, char *line)
{
	char *p = line + 4, *f[4];
	int capacity, temp, charging, cycles, i;
	bool changed;

	if (*p++ != ',')
		return false;
	for (i = 0; i < ARRAY_SIZE(f); i++) {
		f[i] = strsep(&p, ",");
		if (!f[i] || !*f[i])
			return false;
	}

	if (kstrtoint(f[0], 10, &capacity) || mcu_parse_tenths(f[1], &temp) ||
	    kstrtoint(f[2], 10, &charging) || kstrtoint(f[3], 10, &cycles))
		return false;
	if (capacity < 0 || capacity > 100)
		return false;

	changed = !mcu->valid || mcu->capacity != capacity ||
		  mcu->charging != !!charging || mcu->cycles != cycles ||
		  abs(mcu->temp - temp) >= 10;
	mcu->capacity = capacity;
	mcu->temp = temp;
	mcu->charging = !!charging;
	mcu->cycles = cycles;
	mcu->valid = true;

	complete(&mcu->reply);
	if (changed)
		power_supply_changed(mcu->psy);
	return true;
}

/* called with the lock held */
static void mcu_handle_line(struct gl_e750_mcu *mcu, char *line)
{
	if (!strncmp(line, "{OK}", 4))
		mcu_parse_status(mcu, line);
	else if (strstr(line, "\"shut_down\""))
		schedule_work(&mcu->poweroff_work);
	else
		dev_dbg_ratelimited(&mcu->serdev->dev, "unhandled: %s\n", line);
}

static size_t mcu_receive_buf(struct serdev_device *serdev, const u8 *data,
			      size_t count)
{
	struct gl_e750_mcu *mcu = serdev_device_get_drvdata(serdev);
	unsigned long flags;
	size_t i;

	spin_lock_irqsave(&mcu->lock, flags);
	for (i = 0; i < count; i++) {
		char c = data[i];

		if (c == '\r' || c == '\n') {
			if (mcu->rx_len) {
				mcu->rx_line[mcu->rx_len] = '\0';
				mcu_handle_line(mcu, mcu->rx_line);
				mcu->rx_len = 0;
			}
			continue;
		}
		if (mcu->rx_len < MCU_RX_LINE_MAX - 1)
			mcu->rx_line[mcu->rx_len++] = c;

		/* JSON events from the MCU come without a line ending */
		if (c == '}' && mcu->rx_line[0] == '{' &&
		    strncmp(mcu->rx_line, "{OK}", 4)) {
			mcu->rx_line[mcu->rx_len] = '\0';
			mcu_handle_line(mcu, mcu->rx_line);
			mcu->rx_len = 0;
		}
	}
	spin_unlock_irqrestore(&mcu->lock, flags);

	return count;
}

static const struct serdev_device_ops mcu_serdev_ops = {
	.receive_buf = mcu_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

static void mcu_poll_work(struct work_struct *work)
{
	struct gl_e750_mcu *mcu = container_of(to_delayed_work(work),
					       struct gl_e750_mcu, poll_work);
	unsigned long timeout = msecs_to_jiffies(MCU_REPLY_TIMEOUT_MS);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&mcu->lock, flags);
	mcu->rx_len = 0;
	reinit_completion(&mcu->reply);
	spin_unlock_irqrestore(&mcu->lock, flags);

	ret = serdev_device_write(mcu->serdev, mcu_status_req,
				  sizeof(mcu_status_req) - 1, timeout);
	if (ret < 0) {
		dev_warn_ratelimited(&mcu->serdev->dev, "write failed: %d\n", ret);
	} else if (!wait_for_completion_timeout(&mcu->reply, timeout)) {
		/* the reply may also come without a line ending */
		spin_lock_irqsave(&mcu->lock, flags);
		mcu->rx_line[mcu->rx_len] = '\0';
		if (!(mcu->rx_len > 4 && !strncmp(mcu->rx_line, "{OK}", 4) &&
		      mcu_parse_status(mcu, mcu->rx_line)))
			dev_dbg(&mcu->serdev->dev, "no status reply\n");
		mcu->rx_len = 0;
		spin_unlock_irqrestore(&mcu->lock, flags);
	}

	schedule_delayed_work(&mcu->poll_work,
			      msecs_to_jiffies(MCU_POLL_INTERVAL_MS));
}

static void mcu_poweroff_work(struct work_struct *work)
{
	struct gl_e750_mcu *mcu = container_of(work, struct gl_e750_mcu,
					       poweroff_work);

	/* the MCU cuts the power a few seconds after reporting the switch */
	dev_info(&mcu->serdev->dev, "power switch turned off, shutting down\n");
	orderly_poweroff(true);
}

static const enum power_supply_property mcu_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
};

static int mcu_battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct gl_e750_mcu *mcu = power_supply_get_drvdata(psy);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&mcu->lock, flags);
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!mcu->valid)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else if (mcu->charging)
			val->intval = mcu->capacity >= 100 ?
				      POWER_SUPPLY_STATUS_FULL :
				      POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (mcu->valid)
			val->intval = mcu->capacity;
		else
			ret = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (mcu->valid)
			val->intval = mcu->temp;
		else
			ret = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		if (mcu->valid)
			val->intval = mcu->cycles;
		else
			ret = -ENODATA;
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&mcu->lock, flags);

	return ret;
}

static const struct power_supply_desc mcu_battery_desc = {
	.name = "gl-e750-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mcu_battery_props,
	.num_properties = ARRAY_SIZE(mcu_battery_props),
	.get_property = mcu_battery_get_property,
};

static int gl_e750_mcu_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct power_supply_config psy_cfg = {};
	struct gl_e750_mcu *mcu;
	int ret;

	mcu = devm_kzalloc(dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->serdev = serdev;
	spin_lock_init(&mcu->lock);
	init_completion(&mcu->reply);
	serdev_device_set_drvdata(serdev, mcu);
	serdev_device_set_client_ops(serdev, &mcu_serdev_ops);

	/*
	 * devm teardown runs in reverse: the poll work is cancelled before the
	 * port is closed, the power-off work (queued from the receive path)
	 * and the battery only after it.
	 */
	psy_cfg.drv_data = mcu;
	psy_cfg.fwnode = dev_fwnode(dev);
	mcu->psy = devm_power_supply_register(dev, &mcu_battery_desc, &psy_cfg);
	if (IS_ERR(mcu->psy))
		return dev_err_probe(dev, PTR_ERR(mcu->psy),
				     "failed to register battery\n");

	ret = devm_work_autocancel(dev, &mcu->poweroff_work, mcu_poweroff_work);
	if (ret)
		return ret;

	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to open serdev\n");

	serdev_device_set_baudrate(serdev, MCU_BAUDRATE);
	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set parity\n");

	ret = devm_delayed_work_autocancel(dev, &mcu->poll_work, mcu_poll_work);
	if (ret)
		return ret;

	schedule_delayed_work(&mcu->poll_work, 0);
	return 0;
}

static const struct of_device_id gl_e750_mcu_of_match[] = {
	{ .compatible = "glinet,gl-e750-mcu" },
	{ }
};
MODULE_DEVICE_TABLE(of, gl_e750_mcu_of_match);

static struct serdev_device_driver gl_e750_mcu_driver = {
	.probe = gl_e750_mcu_probe,
	.driver = {
		.name = "gl-e750-mcu",
		.of_match_table = gl_e750_mcu_of_match,
	},
};
module_serdev_device_driver(gl_e750_mcu_driver);

MODULE_AUTHOR("Serv Pol <servpol@tuta.io>");
MODULE_DESCRIPTION("GL.iNet GL-E750 status MCU battery driver");
MODULE_LICENSE("GPL");
