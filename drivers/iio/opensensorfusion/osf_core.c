// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "osf_core.h"
#include "osf_serdev.h"

#define OSF_RESERVED_MSG_FIRST		0x7f00
#define OSF_RESERVED_MSG_LAST		0x7fff
#define OSF_VENDOR_PRIVATE_FIRST	0x8000

static struct osf_latest_sample *
osf_core_find_latest_sample(struct osf_device *osf, u16 sensor_type,
			    u16 sensor_index, bool allocate)
{
	struct osf_latest_sample *free_slot = NULL;
	struct osf_latest_sample *latest;
	unsigned int i;

	for (i = 0; i < OSF_MAX_LATEST_SAMPLES; i++) {
		latest = &osf->latest_samples[i];
		if (!latest->valid) {
			if (!free_slot)
				free_slot = latest;
			continue;
		}

		if (latest->sensor_type == sensor_type &&
		    latest->sensor_index == sensor_index)
			return latest;
	}

	return allocate ? free_slot : NULL;
}

static void osf_core_store_latest_sample(struct osf_latest_sample *latest,
					 const struct osf_sensor_sample *sample,
					 const s32 *values,
					 const struct osf_frame *frame)
{
	unsigned int i;

	for (i = 0; i < sample->channel_count; i++)
		latest->values[i] = values[i];

	for (; i < OSF_MAX_SAMPLE_CHANNELS; i++)
		latest->values[i] = 0;

	latest->sensor_type = sample->sensor_type;
	latest->sensor_index = sample->sensor_index;
	latest->channel_count = sample->channel_count;
	latest->sample_format = sample->sample_format;
	latest->scale_nano = sample->scale_nano;
	latest->sequence = frame->sequence;
	latest->timestamp_us = frame->timestamp_us;
	latest->valid = true;
}

static int osf_core_handle_sensor_sample(struct osf_device *osf,
					 const struct osf_frame *frame)
{
	osf_sample_callback_t callback;
	void *callback_context;
	struct osf_latest_sample *latest;
	struct osf_sample_event event = { };
	struct osf_sensor_sample sample;
	s32 values[OSF_MAX_SAMPLE_CHANNELS] = { };
	unsigned long flags;
	unsigned int i;
	int ret;

	ret = osf_protocol_decode_sensor_sample(frame, &sample);
	if (ret)
		return ret;

	if (sample.channel_count > OSF_MAX_SAMPLE_CHANNELS)
		return -E2BIG;

	for (i = 0; i < sample.channel_count; i++) {
		ret = osf_protocol_sensor_sample_value(&sample, i, &values[i]);
		if (ret)
			return ret;
	}

	event.sensor_type = sample.sensor_type;
	event.sensor_index = sample.sensor_index;
	event.channel_count = sample.channel_count;
	event.sample_format = sample.sample_format;
	event.scale_nano = sample.scale_nano;
	event.sequence = frame->sequence;
	event.timestamp_us = frame->timestamp_us;
	event.host_timestamp_ns = ktime_get_ns();
	for (i = 0; i < sample.channel_count; i++)
		event.values[i] = values[i];

	spin_lock_irqsave(&osf->lock, flags);
	latest = osf_core_find_latest_sample(osf, sample.sensor_type,
					     sample.sensor_index, true);
	if (!latest) {
		spin_unlock_irqrestore(&osf->lock, flags);
		return -ENOSPC;
	}

	osf_core_store_latest_sample(latest, &sample, values, frame);
	osf_core_store_latest_sample(&osf->latest_sample, &sample, values,
				     frame);
	osf->last_sequence = frame->sequence;
	callback = osf->sample_callback;
	callback_context = osf->sample_callback_context;
	spin_unlock_irqrestore(&osf->lock, flags);

	if (callback)
		callback(callback_context, &event);

	return 0;
}

static int osf_core_handle_device_status(struct osf_device *osf,
					 const struct osf_frame *frame)
{
	struct osf_status_cache cache = { };
	struct osf_device_status status;
	unsigned long flags;
	int ret;

	ret = osf_protocol_decode_device_status(frame, &status);
	if (ret)
		return ret;

	if (status.reserved)
		return -EPROTO;

	cache.uptime_s = status.uptime_s;
	cache.status_flags = status.status_flags;
	cache.error_flags = status.error_flags;
	cache.dropped_frames = status.dropped_frames;
	cache.sequence = frame->sequence;
	cache.valid = true;
	spin_lock_irqsave(&osf->lock, flags);
	osf->status_cache = cache;
	osf->last_sequence = frame->sequence;
	spin_unlock_irqrestore(&osf->lock, flags);

	return 0;
}

static int osf_core_handle_capability_report(struct osf_device *osf,
					     const struct osf_frame *frame)
{
	struct osf_capability_cache cache = { };
	struct osf_capability_report report;
	unsigned long flags;
	unsigned int i;
	int ret;

	ret = osf_protocol_decode_capability_report(frame, &report);
	if (ret)
		return ret;

	if (report.capability_count > OSF_MAX_CAPABILITIES)
		return -E2BIG;

	for (i = 0; i < report.capability_count; i++) {
		ret = osf_protocol_decode_capability_entry(&report, i,
							   &cache.entries[i]);
		if (ret)
			return ret;
	}

	cache.capability_count = report.capability_count;
	cache.sequence = frame->sequence;
	cache.valid = true;
	spin_lock_irqsave(&osf->lock, flags);
	osf->capability_cache = cache;
	osf->last_sequence = frame->sequence;
	spin_unlock_irqrestore(&osf->lock, flags);

	return 0;
}

int osf_core_receive_frame(struct osf_device *osf, const u8 *buf, size_t len)
{
	struct osf_frame frame;
	size_t frame_len;
	int ret;

	if (!osf || !buf)
		return -EINVAL;

	ret = osf_protocol_decode_frame(buf, len, &frame, &frame_len);
	if (ret)
		return ret;

	if (frame_len != len)
		return -EMSGSIZE;

	switch (frame.message_type) {
	case OSF_MSG_SENSOR_SAMPLE:
		return osf_core_handle_sensor_sample(osf, &frame);
	case OSF_MSG_DEVICE_STATUS:
		return osf_core_handle_device_status(osf, &frame);
	case OSF_MSG_CAPABILITY_REPORT:
		return osf_core_handle_capability_report(osf, &frame);
	default:
		if (frame.message_type >= OSF_RESERVED_MSG_FIRST &&
		    frame.message_type <= OSF_RESERVED_MSG_LAST)
			return 0;
		if (frame.message_type >= OSF_VENDOR_PRIVATE_FIRST)
			return 0;
		return -EOPNOTSUPP;
	}
}

int osf_core_read_latest_sample(struct osf_device *osf, u16 sensor_type,
				u16 sensor_index, unsigned int channel,
				s32 *value)
{
	const struct osf_latest_sample *latest;
	unsigned long flags;
	int ret = 0;

	if (!osf || !value)
		return -EINVAL;

	spin_lock_irqsave(&osf->lock, flags);
	latest = osf_core_find_latest_sample(osf, sensor_type,
					     sensor_index, false);
	if (!latest)
		latest = &osf->latest_sample;

	if (!latest->valid) {
		ret = -ENODATA;
		goto out_unlock;
	}

	if (latest->sensor_type != sensor_type ||
	    latest->sensor_index != sensor_index) {
		ret = -ENODATA;
		goto out_unlock;
	}

	if (channel >= latest->channel_count) {
		ret = -ENODATA;
		goto out_unlock;
	}

	*value = latest->values[channel];

out_unlock:
	spin_unlock_irqrestore(&osf->lock, flags);

	return ret;
}

void osf_core_init_device(struct osf_device *osf)
{
	if (!osf)
		return;

	spin_lock_init(&osf->lock);
	osf->sample_callback = NULL;
	osf->sample_callback_context = NULL;
}

bool osf_core_copy_capability_cache(struct osf_device *osf,
				    struct osf_capability_cache *cache)
{
	unsigned long flags;
	bool valid;

	if (!osf || !cache)
		return false;

	spin_lock_irqsave(&osf->lock, flags);
	valid = osf->capability_cache.valid;
	if (valid)
		*cache = osf->capability_cache;
	spin_unlock_irqrestore(&osf->lock, flags);

	return valid;
}

bool osf_core_capability_sequence(struct osf_device *osf, u64 *sequence)
{
	unsigned long flags;
	bool valid;

	if (!osf || !sequence)
		return false;

	spin_lock_irqsave(&osf->lock, flags);
	valid = osf->capability_cache.valid;
	if (valid)
		*sequence = osf->capability_cache.sequence;
	spin_unlock_irqrestore(&osf->lock, flags);

	return valid;
}

void osf_core_set_sample_callback(struct osf_device *osf,
				  osf_sample_callback_t callback,
				  void *context)
{
	unsigned long flags;

	if (!osf)
		return;

	spin_lock_irqsave(&osf->lock, flags);
	osf->sample_callback = callback;
	osf->sample_callback_context = context;
	spin_unlock_irqrestore(&osf->lock, flags);
}

static int __init osf_core_init(void)
{
	return osf_serdev_register_driver();
}

static void __exit osf_core_exit(void)
{
	osf_serdev_unregister_driver();
}

module_init(osf_core_init);
module_exit(osf_core_exit);

MODULE_DESCRIPTION("Open Sensor Fusion IIO skeleton");
MODULE_LICENSE("GPL");
