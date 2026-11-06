// SPDX-License-Identifier: GPL-2.0-only
/*
 * ChromeOS EC Power Monitor Driver
 *
 * Copyright (c) 2026 Google Inc.
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#define PWRMON_MAX_CHANNELS 64
#define PWRMON_MKBP_TIMEOUT_MS 1000

struct cros_ec_pwrmon_state {
	struct cros_ec_command *msg;
	struct cros_ec_dev *ec_dev;
	struct cros_ec_device *ec;
	/* Serializes EC host commands and energy accumulation state */
	struct mutex lock;
	/* Protects MKBP event results and collection state */
	spinlock_t event_lock;
	struct notifier_block notifier;
	wait_queue_head_t wq;
	struct {
		s64 value;
		bool valid;
	} results[PWRMON_MAX_CHANNELS];
	int results_received;
	bool collecting;
	u64 *accumulated_energy;
	char (*channel_names)[32];
	int num_channels;
	u64 last_time_ms;
};

/*
 * MKBP event handler — called once per channel event from the EC.
 */
static int cros_ec_pwrmon_notify(struct notifier_block *nb,
				 unsigned long queued_during_suspend,
				 void *data)
{
	struct cros_ec_pwrmon_state *st =
		container_of(nb, struct cros_ec_pwrmon_state, notifier);
	struct cros_ec_device *ec_dev = st->ec;
	s64 val;
	u8 ch;

	if (ec_dev->event_data.event_type != EC_MKBP_EVENT_PWRMON)
		return NOTIFY_DONE;

	ch = ec_dev->event_data.data.pwrmon_data.channel_id;
	val = get_unaligned_le64(&ec_dev->event_data.data.pwrmon_data.value);

	scoped_guard(spinlock, &st->event_lock) {
		if (!st->collecting)
			return NOTIFY_OK;

		if (ch < st->num_channels) {
			st->results[ch].value = val;
			st->results[ch].valid = true;
		}

		WRITE_ONCE(st->results_received, st->results_received + 1);
	}

	wake_up(&st->wq);

	return NOTIFY_OK;
}

/*
 * Expose ODPM results in the form of accumulated energy
 * through the energy_value sysfs node.
 * Format requirement:
 * t=<Measurement timestamp, ms>
 * CH<N>(T=<Duration, ms>)[<Schematic name>], <Accumulated Energy, uWs>
 */
static ssize_t energy_value_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct cros_ec_pwrmon_state *st = iio_priv(indio_dev);
	struct ec_params_pwrmon *params;
	u64 current_time_ms;
	s64 delta_uws;
	u32 duration_ms;
	int received;
	ssize_t len = 0;
	int ret, i;

	guard(mutex)(&st->lock);

	/* Reset results and prepare for MKBP events */
	scoped_guard(spinlock, &st->event_lock) {
		memset(st->results, 0, sizeof(st->results));
		WRITE_ONCE(st->results_received, 0);
		st->collecting = true;
	}

	/* Latch all PACs — this also triggers async MKBP delivery. */
	params = (struct ec_params_pwrmon *)st->msg->data;
	params->cmd = EC_PWRMON_LATCH;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0) {
		scoped_guard(spinlock, &st->event_lock)
			st->collecting = false;
		dev_warn(dev, "Failed to latch PAC accumulators: %d\n", ret);
		return ret;
	}

	/* Wait for all MKBP events to arrive from the EC */
	ret = wait_event_timeout(st->wq,
				 READ_ONCE(st->results_received) >= st->num_channels,
				 msecs_to_jiffies(PWRMON_MKBP_TIMEOUT_MS));

	scoped_guard(spinlock, &st->event_lock) {
		st->collecting = false;
		received = st->results_received;
	}

	if (!ret)
		dev_warn(dev, "Timed out waiting for PWRMON MKBP data (received %d/%d)\n",
			 received, st->num_channels);

	/* Timestamp of snapshot taken. */
	current_time_ms = ktime_to_ms(ktime_get_boottime());
	len += sysfs_emit_at(buf, len, "t=%llu\n", current_time_ms);

	duration_ms = current_time_ms - st->last_time_ms;

	for (i = 0; i < st->num_channels; i++) {
		if (!st->results[i].valid) {
			dev_warn(dev, "Missing data for channel %d\n", i);
			continue;
		}

		delta_uws = div_s64(st->results[i].value * duration_ms,
				    MSEC_PER_SEC);
		if (delta_uws > 0)
			st->accumulated_energy[i] += delta_uws;

		len += sysfs_emit_at(buf, len, "CH%d(T=%u)[%s], %llu\n",
				     i, duration_ms, st->channel_names[i],
				     st->accumulated_energy[i]);
	}

	st->last_time_ms = current_time_ms;

	return len;
}

static IIO_DEVICE_ATTR(energy_value, 0444, energy_value_show, NULL, 0);

static struct attribute *cros_ec_pwrmon_attrs[] = {
	&iio_dev_attr_energy_value.dev_attr.attr,
	NULL
};

static const struct attribute_group cros_ec_pwrmon_attr_group = {
	.attrs = cros_ec_pwrmon_attrs,
};

static const struct iio_info cros_ec_pwrmon_info = {
	.attrs = &cros_ec_pwrmon_attr_group,
};

static void cros_ec_pwrmon_stop(void *data)
{
	struct cros_ec_pwrmon_state *st = data;
	struct ec_params_pwrmon *params;
	int ret;

	guard(mutex)(&st->lock);

	params = (struct ec_params_pwrmon *)st->msg->data;
	params->cmd = EC_PWRMON_STOP;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0)
		dev_warn(st->ec_dev->dev, "Failed to stop PACs: %d\n", ret);
}

static void cros_ec_pwrmon_unregister_notifier(void *data)
{
	struct cros_ec_pwrmon_state *st = data;

	blocking_notifier_chain_unregister(&st->ec->event_notifier,
					   &st->notifier);
}

static int cros_ec_pwrmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_dev *ec_dev = dev_get_drvdata(dev->parent);
	struct cros_ec_pwrmon_state *st;
	struct ec_params_pwrmon *params;
	struct pwrmon_dump_info *dump;
	struct iio_dev *indio_dev;
	size_t msg_size;
	int ret, i;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->ec_dev = ec_dev;
	st->ec = ec_dev->ec_dev;

	spin_lock_init(&st->event_lock);
	init_waitqueue_head(&st->wq);

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	msg_size = sizeof(*st->msg) + max(sizeof(struct ec_params_pwrmon),
					  sizeof(struct ec_response_pwrmon));
	st->msg = devm_kzalloc(dev, msg_size, GFP_KERNEL);
	if (!st->msg)
		return -ENOMEM;

	st->msg->version = 0;
	st->msg->command = EC_CMD_PWRMON + st->ec_dev->cmd_offset;
	params = (struct ec_params_pwrmon *)st->msg->data;
	st->msg->outsize = sizeof(*params);
	st->msg->insize = sizeof(struct ec_response_pwrmon);

	/* Start PACs */
	params->cmd = EC_PWRMON_START;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0) {
		dev_warn(dev, "Failed to start PACs: %d\n", ret);
		return ret;
	}
	st->last_time_ms = ktime_to_ms(ktime_get_boottime());

	ret = devm_add_action_or_reset(dev, cros_ec_pwrmon_stop, st);
	if (ret)
		return ret;

	/* Get Channel Count */
	params->cmd = EC_PWRMON_GET_CHANNEL_COUNT;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0) {
		dev_warn(dev, "Failed to get channel count: %d\n", ret);
		return ret;
	}
	st->num_channels = ((struct ec_response_pwrmon *)st->msg->data)->channel_count;
	if (st->num_channels > PWRMON_MAX_CHANNELS)
		st->num_channels = PWRMON_MAX_CHANNELS;

	st->accumulated_energy = devm_kcalloc(dev, st->num_channels,
					      sizeof(*st->accumulated_energy),
					      GFP_KERNEL);
	st->channel_names = devm_kcalloc(dev, st->num_channels,
					 sizeof(*st->channel_names),
					 GFP_KERNEL);
	if (!st->accumulated_energy || !st->channel_names)
		return -ENOMEM;

	/* Get Dump Info (Names) */
	for (i = 0; i < st->num_channels; i++) {
		params->cmd = EC_PWRMON_DUMP_INFO;
		params->channel_id = i;
		ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
		if (ret < 0) {
			dev_warn(dev, "Failed to get dump info for channel %d: %d\n",
				 i, ret);
			continue;
		}
		dump = &((struct ec_response_pwrmon *)st->msg->data)->dump_info;
		strscpy(st->channel_names[i], dump->channel_name,
			sizeof(st->channel_names[i]));
	}

	indio_dev->name = dev_name(dev);
	indio_dev->info = &cros_ec_pwrmon_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	/* Register MKBP event notifier */
	st->notifier.notifier_call = cros_ec_pwrmon_notify;
	ret = blocking_notifier_chain_register(&st->ec->event_notifier,
					       &st->notifier);
	if (ret < 0) {
		dev_err(dev, "Failed to register notifier: %d\n", ret);
		return ret;
	}

	ret = devm_add_action_or_reset(dev, cros_ec_pwrmon_unregister_notifier,
				       st);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct platform_device_id cros_ec_pwrmon_id[] = {
	{ "cros-ec-pwrmon", 0 },
	{}
};
MODULE_DEVICE_TABLE(platform, cros_ec_pwrmon_id);

static struct platform_driver cros_ec_pwrmon_driver = {
	.driver = {
		.name = "cros-ec-pwrmon",
	},
	.probe = cros_ec_pwrmon_probe,
	.id_table = cros_ec_pwrmon_id,
};
module_platform_driver(cros_ec_pwrmon_driver);

MODULE_AUTHOR("Daniel Balint <dbalint@google.com>");
MODULE_DESCRIPTION("ChromeOS EC Power Monitor Driver");
MODULE_LICENSE("GPL");
