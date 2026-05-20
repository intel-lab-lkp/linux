// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "osf_core.h"
#include "osf_iio.h"
#include "osf_serdev.h"
#include "osf_stream.h"

#define OSF_SERDEV_BAUD		115200
#define OSF_SERDEV_BUFFER_QUEUE_LEN	32

struct osf_serdev_iio {
	u16 sensor_type;
	u16 sensor_index;
	struct iio_dev *indio_dev;
};

struct osf_serdev {
	struct serdev_device *serdev;
	struct osf_device osf;
	struct osf_stream stream;
	struct work_struct iio_register_work;
	struct work_struct buffer_push_work;
	/* Serializes IIO device table access and registration work. */
	struct mutex iio_lock;
	/* Protects sample events queued from the receive path. */
	spinlock_t sample_lock;
	struct osf_serdev_iio iio[OSF_MAX_CAPABILITIES];
	unsigned int iio_count;
	struct osf_sample_event sample_queue[OSF_SERDEV_BUFFER_QUEUE_LEN];
	unsigned int sample_queue_head;
	unsigned int sample_queue_tail;
	unsigned int sample_queue_count;
	u64 sample_queue_drops;
	atomic64_t capability_sequence_scheduled;
};

static struct iio_dev *osf_serdev_find_iio(struct osf_serdev *osf_uart,
					   const struct osf_sample_event *event)
{
	unsigned int i;

	for (i = 0; i < osf_uart->iio_count; i++) {
		if (osf_uart->iio[i].sensor_type == event->sensor_type &&
		    osf_uart->iio[i].sensor_index == event->sensor_index)
			return osf_uart->iio[i].indio_dev;
	}

	return NULL;
}

static bool osf_serdev_iio_registered(struct osf_serdev *osf_uart,
				      const struct osf_capability_entry *entry)
{
	unsigned int i;

	for (i = 0; i < osf_uart->iio_count; i++) {
		if (osf_uart->iio[i].sensor_type == entry->sensor_type &&
		    osf_uart->iio[i].sensor_index == entry->sensor_index)
			return true;
	}

	return false;
}

static int osf_serdev_register_iio(struct osf_serdev *osf_uart,
				   const struct osf_capability_entry *entry)
{
	struct device *dev = &osf_uart->serdev->dev;
	struct iio_dev *indio_dev;
	int ret;

	if (osf_serdev_iio_registered(osf_uart, entry))
		return 0;

	if (!osf_iio_sensor_supported(entry->sensor_type, entry->channel_count) ||
	    entry->sample_format != OSF_SAMPLE_FORMAT_S32) {
		dev_dbg(dev,
			"ignoring unsupported capability sensor=%u index=%u channels=%u format=%u\n",
			entry->sensor_type, entry->sensor_index,
			entry->channel_count, entry->sample_format);
		return 0;
	}

	if (osf_uart->iio_count >= ARRAY_SIZE(osf_uart->iio)) {
		dev_warn(dev, "IIO registration table full, ignoring sensor=%u index=%u\n",
			 entry->sensor_type, entry->sensor_index);
		return 0;
	}

	ret = osf_iio_register_sensor(dev, entry, &osf_uart->osf, &indio_dev);
	if (ret)
		return ret;

	osf_uart->iio[osf_uart->iio_count].sensor_type = entry->sensor_type;
	osf_uart->iio[osf_uart->iio_count].sensor_index = entry->sensor_index;
	osf_uart->iio[osf_uart->iio_count].indio_dev = indio_dev;
	osf_uart->iio_count++;

	return 1;
}

static void osf_serdev_iio_register_work(struct work_struct *work)
{
	struct osf_serdev *osf_uart =
		container_of(work, struct osf_serdev, iio_register_work);
	struct device *dev = &osf_uart->serdev->dev;
	struct osf_capability_cache cache;
	unsigned int registered = 0;
	unsigned int i;
	int ret;

	if (!osf_core_copy_capability_cache(&osf_uart->osf, &cache))
		return;

	mutex_lock(&osf_uart->iio_lock);
	for (i = 0; i < cache.capability_count; i++) {
		ret = osf_serdev_register_iio(osf_uart, &cache.entries[i]);
		if (ret) {
			if (ret > 0) {
				registered++;
				continue;
			}

			dev_err(dev,
				"failed to register IIO sensor=%u index=%u: %d\n",
				cache.entries[i].sensor_type,
				cache.entries[i].sensor_index, ret);
		}
	}
	mutex_unlock(&osf_uart->iio_lock);

	if (registered)
		dev_info(dev,
			 "registered %u Open Sensor Fusion IIO devices from capability report seq=%llu\n",
			 registered, (unsigned long long)cache.sequence);
}

static bool osf_serdev_dequeue_sample(struct osf_serdev *osf_uart,
				      struct osf_sample_event *event)
{
	unsigned long flags;
	bool valid = false;

	spin_lock_irqsave(&osf_uart->sample_lock, flags);
	if (osf_uart->sample_queue_count) {
		*event = osf_uart->sample_queue[osf_uart->sample_queue_tail];
		osf_uart->sample_queue_tail =
			(osf_uart->sample_queue_tail + 1) %
			ARRAY_SIZE(osf_uart->sample_queue);
		osf_uart->sample_queue_count--;
		valid = true;
	}
	spin_unlock_irqrestore(&osf_uart->sample_lock, flags);

	return valid;
}

static void osf_serdev_buffer_push_work(struct work_struct *work)
{
	struct osf_serdev *osf_uart =
		container_of(work, struct osf_serdev, buffer_push_work);
	struct device *dev = &osf_uart->serdev->dev;
	struct osf_sample_event event;
	struct iio_dev *indio_dev;
	int ret;

	while (osf_serdev_dequeue_sample(osf_uart, &event)) {
		mutex_lock(&osf_uart->iio_lock);
		indio_dev = osf_serdev_find_iio(osf_uart, &event);
		if (indio_dev) {
			ret = osf_iio_push_sample(indio_dev, &event);
			if (ret)
				dev_dbg_ratelimited(dev,
						    "failed to push IIO buffer sample sensor=%u index=%u ret=%d\n",
						    event.sensor_type,
						    event.sensor_index, ret);
		}
		mutex_unlock(&osf_uart->iio_lock);
	}
}

static void osf_serdev_sample_ready(void *context,
				    const struct osf_sample_event *event)
{
	struct osf_serdev *osf_uart = context;
	unsigned long flags;
	u64 drops = 0;
	bool queued = false;

	if (!osf_uart || !event)
		return;

	spin_lock_irqsave(&osf_uart->sample_lock, flags);
	if (osf_uart->sample_queue_count < ARRAY_SIZE(osf_uart->sample_queue)) {
		osf_uart->sample_queue[osf_uart->sample_queue_head] = *event;
		osf_uart->sample_queue_head =
			(osf_uart->sample_queue_head + 1) %
			ARRAY_SIZE(osf_uart->sample_queue);
		osf_uart->sample_queue_count++;
		queued = true;
	} else {
		osf_uart->sample_queue_drops++;
		drops = osf_uart->sample_queue_drops;
	}
	spin_unlock_irqrestore(&osf_uart->sample_lock, flags);

	if (queued) {
		schedule_work(&osf_uart->buffer_push_work);
		return;
	}

	dev_dbg_ratelimited(&osf_uart->serdev->dev,
			    "dropping IIO buffer sample sensor=%u index=%u drops=%llu\n",
			    event->sensor_type, event->sensor_index,
			    (unsigned long long)drops);
}

static void osf_serdev_schedule_iio_registration(struct osf_serdev *osf_uart)
{
	s64 scheduled_seq;
	u64 sequence;

	if (!osf_core_capability_sequence(&osf_uart->osf, &sequence))
		return;

	scheduled_seq = atomic64_read(&osf_uart->capability_sequence_scheduled);
	if (scheduled_seq == (s64)sequence)
		return;

	atomic64_set(&osf_uart->capability_sequence_scheduled, (s64)sequence);
	schedule_work(&osf_uart->iio_register_work);
}

static size_t osf_serdev_receive_buf(struct serdev_device *serdev,
				     const u8 *buf, size_t count)
{
	struct osf_serdev *osf_uart = serdev_device_get_drvdata(serdev);
	const struct osf_stream_stats *stats;
	u64 valid_before;
	int ret;

	valid_before = osf_uart->stream.stats.valid_frames;
	ret = osf_stream_receive_bytes(&osf_uart->stream, buf, count);
	stats = &osf_uart->stream.stats;
	osf_serdev_schedule_iio_registration(osf_uart);

	if (ret || stats->valid_frames != valid_before)
		dev_dbg_ratelimited(&serdev->dev,
				    "rx count=%zu valid=%llu bad_magic=%llu bad_crc=%llu partial=%llu dropped=%llu ret=%d\n",
				    count,
				    (unsigned long long)stats->valid_frames,
				    (unsigned long long)stats->bad_magic_resyncs,
				    (unsigned long long)stats->bad_crc_frames,
				    (unsigned long long)stats->partial_frames,
				    (unsigned long long)stats->dropped_bytes,
				    ret);

	return count;
}

static const struct serdev_device_ops osf_serdev_ops = {
	.receive_buf = osf_serdev_receive_buf,
};

static int osf_serdev_probe(struct serdev_device *serdev)
{
	struct osf_serdev *osf_uart;
	unsigned int baudrate;
	int ret;

	osf_uart = devm_kzalloc(&serdev->dev, sizeof(*osf_uart), GFP_KERNEL);
	if (!osf_uart)
		return -ENOMEM;

	osf_uart->serdev = serdev;
	osf_core_init_device(&osf_uart->osf);
	osf_stream_init(&osf_uart->stream, &osf_uart->osf);
	INIT_WORK(&osf_uart->iio_register_work, osf_serdev_iio_register_work);
	INIT_WORK(&osf_uart->buffer_push_work, osf_serdev_buffer_push_work);
	mutex_init(&osf_uart->iio_lock);
	spin_lock_init(&osf_uart->sample_lock);
	atomic64_set(&osf_uart->capability_sequence_scheduled, -1);
	osf_core_set_sample_callback(&osf_uart->osf, osf_serdev_sample_ready,
				     osf_uart);

	serdev_device_set_drvdata(serdev, osf_uart);
	serdev_device_set_client_ops(serdev, &osf_serdev_ops);

	ret = serdev_device_open(serdev);
	if (ret)
		return ret;

	baudrate = serdev_device_set_baudrate(serdev, OSF_SERDEV_BAUD);
	if (baudrate != OSF_SERDEV_BAUD)
		dev_warn(&serdev->dev, "requested %u baud, controller set %u\n",
			 OSF_SERDEV_BAUD, baudrate);

	serdev_device_set_flow_control(serdev, false);

	dev_info(&serdev->dev, "Open Sensor Fusion UART opened at %u baud\n",
		 OSF_SERDEV_BAUD);

	return 0;
}

static void osf_serdev_remove(struct serdev_device *serdev)
{
	struct osf_serdev *osf_uart = serdev_device_get_drvdata(serdev);

	osf_core_set_sample_callback(&osf_uart->osf, NULL, NULL);
	serdev_device_close(serdev);
	cancel_work_sync(&osf_uart->iio_register_work);
	cancel_work_sync(&osf_uart->buffer_push_work);
	osf_stream_reset(&osf_uart->stream);
}

static const struct of_device_id osf_serdev_of_match[] = {
	{ .compatible = "opensensorfusion,osf-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, osf_serdev_of_match);

static struct serdev_device_driver osf_serdev_driver = {
	.probe = osf_serdev_probe,
	.remove = osf_serdev_remove,
	.driver = {
		.name = "open-sensor-fusion-uart",
		.of_match_table = osf_serdev_of_match,
	},
};

int osf_serdev_register_driver(void)
{
	return serdev_device_driver_register(&osf_serdev_driver);
}

void osf_serdev_unregister_driver(void)
{
	serdev_device_driver_unregister(&osf_serdev_driver);
}
