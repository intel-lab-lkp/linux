// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include "osf_core.h"
#include "osf_iio.h"
#include "osf_stream.h"

#define OSF_RESERVED_MSG_FIRST		0x7f00
#define OSF_RESERVED_MSG_LAST		0x7fff
#define OSF_VENDOR_PRIVATE_FIRST	0x8000

void osf_core_init(struct osf_device *osf, struct device *dev)
{
	*osf = (struct osf_device) {
		.dev = dev,
	};
	mutex_init(&osf->latest_lock);
}

void osf_core_unregister_iio(struct osf_device *osf)
{
	for (unsigned int i = 0; i < osf->iio_dev_count; i++)
		osf_iio_unregister_sensor(osf->iio_devs[i].indio_dev);

	osf->iio_dev_count = 0;
}

static struct iio_dev *osf_core_find_iio_dev(struct osf_device *osf,
					     u16 sensor_type, u16 sensor_index)
{
	const struct osf_iio_binding *binding;

	for (unsigned int i = 0; i < osf->iio_dev_count; i++) {
		binding = &osf->iio_devs[i];
		if (binding->sensor_type == sensor_type &&
		    binding->sensor_index == sensor_index)
			return binding->indio_dev;
	}

	return NULL;
}

static struct osf_latest_sample *
osf_core_find_latest_sample(struct osf_device *osf, u16 sensor_type,
			    u16 sensor_index)
{
	struct osf_latest_sample *latest;

	for (unsigned int i = 0; i < osf->latest_sample_count; i++) {
		latest = &osf->latest_samples[i];
		if (latest->sensor_type == sensor_type &&
		    latest->sensor_index == sensor_index)
			return latest;
	}

	if (osf->latest_sample_count >= OSF_MAX_CAPABILITIES)
		return NULL;

	return &osf->latest_samples[osf->latest_sample_count++];
}

static bool
osf_core_capability_supported(const struct osf_capability_entry *entry)
{
	return osf_iio_sensor_supported(entry->sensor_type,
					entry->channel_count) &&
	       entry->sample_format == OSF_SAMPLE_FORMAT_S32 &&
	       !(entry->flags & ~OSF_CAPABILITY_FLAGS_MASK) &&
	       !entry->reserved;
}

static bool osf_core_capability_is_duplicate(const struct osf_capability_cache *cache,
					     u16 index)
{
	const struct osf_capability_entry *entry = &cache->entries[index];

	for (u16 i = 0; i < index; i++) {
		if (cache->entries[i].sensor_type == entry->sensor_type &&
		    cache->entries[i].sensor_index == entry->sensor_index)
			return true;
	}

	return false;
}

static int osf_core_register_capabilities(struct osf_device *osf,
					  const struct osf_capability_cache *cache)
{
	struct iio_dev *indio_dev;
	int ret;

	for (u16 i = 0; i < cache->capability_count; i++) {
		if (osf_core_capability_is_duplicate(cache, i))
			return -EEXIST;
	}

	for (u16 i = 0; i < cache->capability_count; i++) {
		ret = osf_iio_register_sensor(osf->dev, &cache->entries[i],
					      osf, &indio_dev);
		if (ret)
			goto err_unregister;

		osf->iio_devs[osf->iio_dev_count++] = (struct osf_iio_binding) {
			.sensor_type = cache->entries[i].sensor_type,
			.sensor_index = cache->entries[i].sensor_index,
			.indio_dev = indio_dev,
		};
	}

	return 0;

err_unregister:
	osf_core_unregister_iio(osf);

	return ret;
}

static int osf_core_handle_sensor_sample(struct osf_device *osf,
					 const struct osf_frame *frame)
{
	struct osf_latest_sample *latest;
	struct osf_sensor_sample sample;
	struct iio_dev *indio_dev;
	s32 values[OSF_MAX_SAMPLE_CHANNELS] = { };
	int ret;

	ret = osf_protocol_decode_sensor_sample(frame, &sample);
	if (ret) {
		dev_warn_ratelimited(osf->dev,
				     "rejecting malformed sensor sample: %d\n",
				     ret);
		return ret;
	}

	indio_dev = osf_core_find_iio_dev(osf, sample.sensor_type,
					  sample.sensor_index);
	if (!indio_dev) {
		dev_dbg_ratelimited(osf->dev,
				    "ignoring sample for unregistered sensor %#x:%u\n",
				    sample.sensor_type, sample.sensor_index);
		return OSF_STREAM_FRAME_IGNORED;
	}

	if (sample.channel_count > OSF_MAX_SAMPLE_CHANNELS) {
		dev_warn_ratelimited(osf->dev,
				     "rejecting sensor sample with %u channels\n",
				     sample.channel_count);
		return -E2BIG;
	}

	for (u16 i = 0; i < sample.channel_count; i++) {
		ret = osf_protocol_sensor_sample_value(&sample, i, &values[i]);
		if (ret) {
			dev_warn_ratelimited(osf->dev,
					     "rejecting malformed sample value: %d\n",
					     ret);
			return ret;
		}
	}

	ret = osf_iio_push_sample(indio_dev, values, sample.channel_count);
	if (ret) {
		dev_err_ratelimited(osf->dev,
				    "failed to push sensor %#x:%u sample: %d\n",
				    sample.sensor_type, sample.sensor_index, ret);
		return ret;
	}

	scoped_guard(mutex, &osf->latest_lock) {
		latest = osf_core_find_latest_sample(osf, sample.sensor_type,
						     sample.sensor_index);
		if (!latest) {
			dev_err_ratelimited(osf->dev,
					    "latest sample cache full for sensor %#x:%u\n",
					    sample.sensor_type,
					    sample.sensor_index);
			return -ENOSPC;
		}

		memcpy(latest->values, values, sizeof(values));
		latest->sensor_type = sample.sensor_type;
		latest->sensor_index = sample.sensor_index;
		latest->channel_count = sample.channel_count;
		latest->sample_format = sample.sample_format;
		latest->scale_nano = sample.scale_nano;
		latest->sequence = frame->sequence;
		latest->timestamp_us = frame->timestamp_us;
		latest->valid = true;
		osf->last_sequence = frame->sequence;
	}

	return OSF_STREAM_FRAME_HANDLED;
}

static int osf_core_handle_device_status(struct osf_device *osf,
					 const struct osf_frame *frame)
{
	struct osf_device_status status;
	int ret;

	ret = osf_protocol_decode_device_status(frame, &status);
	if (ret) {
		dev_warn_ratelimited(osf->dev,
				     "rejecting malformed device status: %d\n",
				     ret);
		return ret;
	}

	osf->status_cache = (struct osf_status_cache) {
		.uptime_s = status.uptime_s,
		.status_flags = status.status_flags,
		.error_flags = status.error_flags,
		.dropped_frames = status.dropped_frames,
		.sequence = frame->sequence,
		.valid = true,
	};
	osf->last_sequence = frame->sequence;

	return OSF_STREAM_FRAME_HANDLED;
}

static int osf_core_handle_capability_report(struct osf_device *osf,
					     const struct osf_frame *frame)
{
	struct osf_capability_cache cache = { };
	struct osf_capability_report report;
	int frame_result;
	int ret;

	ret = osf_protocol_decode_capability_report(frame, &report);
	if (ret) {
		dev_warn_ratelimited(osf->dev,
				     "rejecting malformed capability report: %d\n",
				     ret);
		return ret;
	}

	if (osf->capability_cache.valid) {
		dev_dbg_ratelimited(osf->dev,
				    "ignoring repeated capability report\n");
		osf->last_sequence = frame->sequence;
		return OSF_STREAM_FRAME_IGNORED;
	}

	for (u16 i = 0; i < report.capability_count; i++) {
		struct osf_capability_entry entry;

		ret = osf_protocol_decode_capability_entry(&report, i, &entry);
		if (ret) {
			dev_warn_ratelimited(osf->dev,
					     "rejecting malformed capability entry: %d\n",
					     ret);
			return ret;
		}

		if (!osf_core_capability_supported(&entry))
			continue;

		if (cache.capability_count >= OSF_MAX_CAPABILITIES) {
			dev_warn_ratelimited(osf->dev,
					     "too many supported capabilities\n");
			return -E2BIG;
		}

		cache.entries[cache.capability_count++] = entry;
	}

	cache.sequence = frame->sequence;
	cache.valid = true;

	frame_result = OSF_STREAM_FRAME_IGNORED;
	if (cache.capability_count) {
		ret = osf_core_register_capabilities(osf, &cache);
		if (ret) {
			if (ret == -EEXIST)
				dev_warn_ratelimited(osf->dev,
						     "rejecting duplicate capability\n");
			else
				dev_err_ratelimited(osf->dev,
						    "failed to register capabilities: %d\n",
						    ret);
			return ret;
		}
		frame_result = OSF_STREAM_FRAME_HANDLED;
	} else {
		dev_dbg_ratelimited(osf->dev,
				    "ignoring report without supported capabilities\n");
	}

	osf->capability_cache = cache;
	osf->last_sequence = frame->sequence;

	return frame_result;
}

int osf_core_receive_frame(struct osf_device *osf, const u8 *buf, size_t len)
{
	struct osf_frame frame;
	size_t frame_len;
	int ret;

	ret = osf_protocol_decode_frame(buf, len, &frame, &frame_len);
	if (ret)
		return ret;

	if (frame_len != len)
		return -EMSGSIZE;

	if (frame.protocol_major != OSF_PROTOCOL_MAJOR) {
		dev_dbg_ratelimited(osf->dev,
				    "ignoring unsupported protocol major %u\n",
				    frame.protocol_major);
		return OSF_STREAM_FRAME_IGNORED;
	}

	if (frame.reserved) {
		dev_dbg_ratelimited(osf->dev,
				    "ignoring frame with reserved field %#x\n",
				    frame.reserved);
		return OSF_STREAM_FRAME_IGNORED;
	}

	switch (frame.message_type) {
	case OSF_MSG_SENSOR_SAMPLE:
		ret = osf_core_handle_sensor_sample(osf, &frame);
		break;
	case OSF_MSG_DEVICE_STATUS:
		ret = osf_core_handle_device_status(osf, &frame);
		break;
	case OSF_MSG_CAPABILITY_REPORT:
		ret = osf_core_handle_capability_report(osf, &frame);
		break;
	default:
		if (frame.message_type >= OSF_RESERVED_MSG_FIRST &&
		    frame.message_type <= OSF_RESERVED_MSG_LAST) {
			dev_dbg_ratelimited(osf->dev,
					    "ignoring reserved message type %#x\n",
					    frame.message_type);
			return OSF_STREAM_FRAME_IGNORED;
		}
		if (frame.message_type >= OSF_VENDOR_PRIVATE_FIRST) {
			dev_dbg_ratelimited(osf->dev,
					    "ignoring vendor message type %#x\n",
					    frame.message_type);
			return OSF_STREAM_FRAME_IGNORED;
		}

		dev_dbg_ratelimited(osf->dev,
				    "ignoring unsupported message type %#x\n",
				    frame.message_type);
		return OSF_STREAM_FRAME_IGNORED;
	}

	/*
	 * Handler failures are authenticated application rejections. Keep the
	 * trusted frame boundary and let the stream consume the complete frame.
	 */
	if (ret < 0)
		return OSF_STREAM_FRAME_REJECTED;

	return ret;
}

int osf_core_read_latest_sample(struct osf_device *osf, u16 sensor_type,
				u16 sensor_index, u16 channel,
				s32 *value)
{
	const struct osf_latest_sample *latest;

	if (!osf || !value)
		return -EINVAL;

	guard(mutex)(&osf->latest_lock);
	for (unsigned int i = 0; i < osf->latest_sample_count; i++) {
		latest = &osf->latest_samples[i];
		if (latest->sensor_type != sensor_type ||
		    latest->sensor_index != sensor_index)
			continue;

		if (!latest->valid || channel >= latest->channel_count)
			break;

		*value = latest->values[channel];
		return 0;
	}

	return -ENODATA;
}
