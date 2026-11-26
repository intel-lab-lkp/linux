// SPDX-License-Identifier: GPL-2.0-only
/*
 * ChromeOS EC Power Monitor Driver
 *
 * Copyright (c) 2026 Google Inc.
 */

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define PWRMON_MAX_CHANNELS 64U
#define PWRMON_MKBP_TIMEOUT_MS 1000

struct cros_ec_pwrmon_chan {
	u64 accumulated_energy;
	s64 latched_power;
	bool valid;
	char name[sizeof_field(struct pwrmon_dump_info, channel_name)];
};

struct cros_ec_pwrmon_state {
	struct device *dev;
	struct cros_ec_dev *ec_dev;
	/* Serializes EC host commands and energy accumulation state */
	struct mutex lock;
	/* Protects MKBP event results and collection state */
	spinlock_t event_lock;
	struct notifier_block notifier;
	struct completion comp;
	struct cros_ec_pwrmon_chan *chans;
	unsigned int num_channels;
	unsigned int results_received;
	bool collecting;
	u64 last_time_ms;
	/* Buffer for enabled channel energies plus 64-bit soft timestamp */
	aligned_u64 scan[PWRMON_MAX_CHANNELS + 1];
};

/*
 * MKBP event handler -- called once per channel event from the EC.
 */
static int cros_ec_pwrmon_notify(struct notifier_block *nb,
				 unsigned long queued_during_suspend,
				 void *data)
{
	struct cros_ec_pwrmon_state *st =
		container_of(nb, struct cros_ec_pwrmon_state, notifier);
	struct cros_ec_device *cros_ec = st->ec_dev->ec_dev;
	s64 val;
	u8 ch;

	if (cros_ec->event_data.event_type != EC_MKBP_EVENT_PWRMON)
		return NOTIFY_DONE;

	if (cros_ec->event_size < sizeof(cros_ec->event_data.data.pwrmon_data))
		return NOTIFY_OK;

	ch = cros_ec->event_data.data.pwrmon_data.channel_id;
	val = get_unaligned_le64(&cros_ec->event_data.data.pwrmon_data.value);

	scoped_guard(spinlock, &st->event_lock) {
		if (!st->collecting)
			return NOTIFY_OK;

		if (ch < st->num_channels && !st->chans[ch].valid) {
			st->chans[ch].latched_power = val;
			st->chans[ch].valid = true;
			st->results_received++;
			if (st->results_received == st->num_channels)
				complete(&st->comp);
		}
	}

	return NOTIFY_OK;
}

static int cros_ec_pwrmon_latch(struct iio_dev *indio_dev, s64 *timestamp_ns)
{
	struct cros_ec_pwrmon_state *st = iio_priv(indio_dev);
	struct cros_ec_dev *ec_dev = st->ec_dev;
	struct cros_ec_device *cros_ec = ec_dev->ec_dev;
	const struct ec_params_pwrmon params = {
		.cmd = EC_PWRMON_LATCH,
	};
	unsigned int received;
	u64 current_time_ms;
	u64 duration_ms;
	int ret;

	/* Reset results and prepare for MKBP events */
	scoped_guard(spinlock, &st->event_lock) {
		for (unsigned int i = 0; i < st->num_channels; i++)
			st->chans[i].valid = false;
		st->results_received = 0;
		reinit_completion(&st->comp);
		st->collecting = true;
	}

	/* Latch all PACs -- this also triggers async MKBP delivery. */
	ret = cros_ec_cmd(cros_ec, 0, EC_CMD_PWRMON + ec_dev->cmd_offset,
			  &params, sizeof(params), NULL, 0);
	if (ret < 0) {
		scoped_guard(spinlock, &st->event_lock)
			st->collecting = false;
		dev_warn(st->dev, "Failed to latch PAC accumulators: %d\n", ret);
		return ret;
	}

	current_time_ms = ktime_to_ms(ktime_get_boottime());
	*timestamp_ns = iio_get_time_ns(indio_dev);

	/* Wait for all MKBP events to arrive from the EC */
	ret = wait_for_completion_timeout(&st->comp,
					  msecs_to_jiffies(PWRMON_MKBP_TIMEOUT_MS));

	scoped_guard(spinlock, &st->event_lock) {
		st->collecting = false;
		received = st->results_received;
	}

	duration_ms = current_time_ms - st->last_time_ms;
	st->last_time_ms = current_time_ms;

	if (!ret) {
		dev_warn(st->dev,
			 "Timed out waiting for PWRMON MKBP data (received %u/%u)\n",
			 received, st->num_channels);
		return -ETIMEDOUT;
	}

	for (unsigned int i = 0; i < st->num_channels; i++) {
		/* The EC reports average power in microwatts over the latch window. */
		if (st->chans[i].latched_power <= 0)
			continue;

		st->chans[i].accumulated_energy +=
			div_u64((u64)st->chans[i].latched_power * duration_ms,
				MSEC_PER_SEC);
	}

	return 0;
}

static irqreturn_t cros_ec_pwrmon_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct cros_ec_pwrmon_state *st = iio_priv(indio_dev);
	s64 timestamp_ns;

	scoped_guard(mutex, &st->lock) {
		unsigned int bit, j = 0;

		if (!cros_ec_pwrmon_latch(indio_dev, &timestamp_ns)) {
			iio_for_each_active_channel(indio_dev, bit)
				st->scan[j++] = st->chans[bit].accumulated_energy;

			iio_push_to_buffers_with_ts(indio_dev, st->scan,
						    sizeof(st->scan),
						    timestamp_ns);
		}
	}

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int cros_ec_pwrmon_read_raw(struct iio_dev *indio_dev,
				   struct iio_chan_spec const *chan,
				   int *val, int *val2, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		/* Raw accumulated energy is in microjoules (1e-6 J). */
		*val = 0;
		*val2 = 1;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int cros_ec_pwrmon_read_label(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan,
				     char *label)
{
	struct cros_ec_pwrmon_state *st = iio_priv(indio_dev);

	if (!st->chans[chan->channel].name[0])
		return -EINVAL;

	return sysfs_emit(label, "%s\n", st->chans[chan->channel].name);
}

static const struct iio_info cros_ec_pwrmon_info = {
	.read_raw = cros_ec_pwrmon_read_raw,
	.read_label = cros_ec_pwrmon_read_label,
};

static void cros_ec_pwrmon_stop(void *data)
{
	struct cros_ec_pwrmon_state *st = data;
	struct cros_ec_dev *ec_dev = st->ec_dev;
	struct cros_ec_device *cros_ec = ec_dev->ec_dev;
	const struct ec_params_pwrmon params = {
		.cmd = EC_PWRMON_STOP,
	};
	int ret;

	guard(mutex)(&st->lock);

	ret = cros_ec_cmd(cros_ec, 0, EC_CMD_PWRMON + ec_dev->cmd_offset,
			  &params, sizeof(params), NULL, 0);
	if (ret < 0)
		dev_warn(st->dev, "Failed to stop PACs: %d\n", ret);
}

static int cros_ec_pwrmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_dev *ec_dev = dev_get_drvdata(dev->parent);
	struct cros_ec_device *cros_ec = ec_dev->ec_dev;
	struct ec_params_pwrmon params = {};
	struct ec_response_pwrmon resp;
	struct cros_ec_pwrmon_state *st;
	struct iio_chan_spec *iio_chans;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->dev = dev;
	st->ec_dev = ec_dev;

	spin_lock_init(&st->event_lock);
	init_completion(&st->comp);

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	/* Start power accumulators */
	params.cmd = EC_PWRMON_START;
	ret = cros_ec_cmd(cros_ec, 0, EC_CMD_PWRMON + ec_dev->cmd_offset,
			  &params, sizeof(params), NULL, 0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to start PACs\n");
	st->last_time_ms = ktime_to_ms(ktime_get_boottime());

	ret = devm_add_action_or_reset(dev, cros_ec_pwrmon_stop, st);
	if (ret)
		return ret;

	/* Get Channel Count */
	params.cmd = EC_PWRMON_GET_CHANNEL_COUNT;
	ret = cros_ec_cmd(cros_ec, 0, EC_CMD_PWRMON + ec_dev->cmd_offset,
			  &params, sizeof(params), &resp, sizeof(resp));
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get channel count\n");

	st->num_channels = min(resp.channel_count, PWRMON_MAX_CHANNELS);
	if (!st->num_channels)
		return dev_err_probe(dev, -ENODEV, "No power monitor channels\n");

	st->chans = devm_kcalloc(dev, st->num_channels, sizeof(*st->chans),
				 GFP_KERNEL);
	if (!st->chans)
		return -ENOMEM;

	iio_chans = devm_kcalloc(dev, st->num_channels + 1, sizeof(*iio_chans),
				 GFP_KERNEL);
	if (!iio_chans)
		return -ENOMEM;

	/* Get Dump Info (Names) */
	for (unsigned int i = 0; i < st->num_channels; i++) {
		iio_chans[i].type = IIO_ENERGY;
		iio_chans[i].indexed = 1;
		iio_chans[i].channel = i;
		iio_chans[i].info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE);
		iio_chans[i].scan_index = i;
		iio_chans[i].scan_type.sign = 'u';
		iio_chans[i].scan_type.realbits = 64;
		iio_chans[i].scan_type.storagebits = 64;
		iio_chans[i].scan_type.endianness = IIO_CPU;

		params.cmd = EC_PWRMON_DUMP_INFO;
		params.channel_id = i;
		ret = cros_ec_cmd(cros_ec, 0,
				  EC_CMD_PWRMON + ec_dev->cmd_offset,
				  &params, sizeof(params), &resp, sizeof(resp));
		if (ret < 0) {
			dev_warn(dev, "Failed to get dump info for channel %u: %d\n",
				 i, ret);
			continue;
		}
		strscpy(st->chans[i].name, resp.dump_info.channel_name,
			sizeof(st->chans[i].name));
	}
	iio_chans[st->num_channels] = IIO_CHAN_SOFT_TIMESTAMP(st->num_channels);

	indio_dev->name = "cros-ec-pwrmon";
	indio_dev->info = &cros_ec_pwrmon_info;
	indio_dev->channels = iio_chans;
	indio_dev->num_channels = st->num_channels + 1;

	ret = iio_device_set_clock(indio_dev, CLOCK_BOOTTIME);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set timestamp clock\n");

	/* Register MKBP event notifier */
	st->notifier.notifier_call = cros_ec_pwrmon_notify;
	ret = devm_blocking_notifier_chain_register(dev,
						    &cros_ec->event_notifier,
						    &st->notifier);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register notifier\n");

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev, NULL,
					      cros_ec_pwrmon_trigger_handler,
					      NULL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to setup triggered buffer\n");

	return devm_iio_device_register(dev, indio_dev);
}

static const struct platform_device_id cros_ec_pwrmon_id[] = {
	{ .name = "cros-ec-pwrmon" },
	{ }
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
