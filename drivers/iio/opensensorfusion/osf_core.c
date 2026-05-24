// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include "osf_core.h"
#include "osf_iio.h"

#define OSF_RESERVED_MSG_FIRST		0x7f00
#define OSF_RESERVED_MSG_LAST		0x7fff
#define OSF_VENDOR_PRIVATE_FIRST	0x8000

void osf_core_init(struct osf_device *osf, struct device *dev)
{
	memset(osf, 0, sizeof(*osf));
	osf->dev = dev;
}

void osf_core_unregister_iio(struct osf_device *osf)
{
	unsigned int i;

	for (i = 0; i < osf->iio_dev_count; i++)
		osf_iio_unregister_sensor(osf->iio_devs[i].indio_dev);

	osf->iio_dev_count = 0;
}

static struct iio_dev *osf_core_find_iio_dev(struct osf_device *osf,
					     u16 sensor_type, u16 sensor_index)
{
	const struct osf_iio_binding *binding;
	unsigned int i;

	for (i = 0; i < osf->iio_dev_count; i++) {
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
	unsigned int i;

	for (i = 0; i < osf->latest_sample_count; i++) {
		latest = &osf->latest_samples[i];
		if (latest->sensor_type == sensor_type &&
		    latest->sensor_index == sensor_index)
			return latest;
	}

	if (osf->latest_sample_count >= OSF_MAX_CAPABILITIES)
		return NULL;

	return &osf->latest_samples[osf->latest_sample_count++];
}

static bool osf_core_capability_duplicate(const struct osf_capability_cache *cache,
					  unsigned int index)
{
	const struct osf_capability_entry *entry = &cache->entries[index];
	unsigned int i;

	for (i = 0; i < index; i++) {
		if (!osf_iio_sensor_supported(cache->entries[i].sensor_type,
					      cache->entries[i].channel_count))
			continue;

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
	unsigned int i;
	int ret;

	if (osf->capability_cache.valid)
		return 0;

	for (i = 0; i < cache->capability_count; i++) {
		if (!osf_iio_sensor_supported(cache->entries[i].sensor_type,
					      cache->entries[i].channel_count))
			continue;

		if (osf_core_capability_duplicate(cache, i))
			return -EEXIST;
	}

	for (i = 0; i < cache->capability_count; i++) {
		if (!osf_iio_sensor_supported(cache->entries[i].sensor_type,
					      cache->entries[i].channel_count))
			continue;

		ret = osf_iio_register_sensor(osf->dev, &cache->entries[i],
					      osf, &indio_dev);
		if (ret)
			goto err_unregister;

		osf->iio_devs[osf->iio_dev_count].sensor_type =
			cache->entries[i].sensor_type;
		osf->iio_devs[osf->iio_dev_count].sensor_index =
			cache->entries[i].sensor_index;
		osf->iio_devs[osf->iio_dev_count].indio_dev = indio_dev;
		osf->iio_dev_count++;
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
	unsigned int i;
	int ret;

	ret = osf_protocol_decode_sensor_sample(frame, &sample);
	if (ret)
		return ret;

	if (sample.channel_count > OSF_MAX_SAMPLE_CHANNELS)
		return -E2BIG;

	latest = osf_core_find_latest_sample(osf, sample.sensor_type,
					     sample.sensor_index);
	if (!latest)
		return -E2BIG;

	for (i = 0; i < sample.channel_count; i++) {
		ret = osf_protocol_sensor_sample_value(&sample, i,
						       &latest->values[i]);
		if (ret)
			return ret;
	}

	for (; i < OSF_MAX_SAMPLE_CHANNELS; i++)
		latest->values[i] = 0;

	latest->sensor_type = sample.sensor_type;
	latest->sensor_index = sample.sensor_index;
	latest->channel_count = sample.channel_count;
	latest->sample_format = sample.sample_format;
	latest->scale_nano = sample.scale_nano;
	latest->sequence = frame->sequence;
	latest->timestamp_us = frame->timestamp_us;
	latest->valid = true;
	osf->last_sequence = frame->sequence;

	indio_dev = osf_core_find_iio_dev(osf, sample.sensor_type,
					  sample.sensor_index);
	if (indio_dev) {
		ret = osf_iio_push_sample(indio_dev, latest->values,
					  latest->channel_count);
		if (ret)
			return ret;
	}

	return 0;
}

static int osf_core_handle_device_status(struct osf_device *osf,
					 const struct osf_frame *frame)
{
	struct osf_status_cache cache = { };
	struct osf_device_status status;
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
	osf->status_cache = cache;
	osf->last_sequence = frame->sequence;

	return 0;
}

static int osf_core_handle_capability_report(struct osf_device *osf,
					     const struct osf_frame *frame)
{
	struct osf_capability_cache cache = { };
	struct osf_capability_report report;
	unsigned int i;
	int ret;

	ret = osf_protocol_decode_capability_report(frame, &report);
	if (ret)
		return ret;

	if (report.capability_count > OSF_MAX_CAPABILITIES)
		return -E2BIG;

	if (osf->capability_cache.valid) {
		osf->last_sequence = frame->sequence;
		return 0;
	}

	for (i = 0; i < report.capability_count; i++) {
		ret = osf_protocol_decode_capability_entry(&report, i,
							   &cache.entries[i]);
		if (ret)
			return ret;
	}

	cache.capability_count = report.capability_count;
	cache.sequence = frame->sequence;
	cache.valid = true;

	ret = osf_core_register_capabilities(osf, &cache);
	if (ret)
		return ret;

	osf->capability_cache = cache;
	osf->last_sequence = frame->sequence;

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
	unsigned int i;

	if (!osf || !value)
		return -EINVAL;

	for (i = 0; i < osf->latest_sample_count; i++) {
		latest = &osf->latest_samples[i];
		if (latest->sensor_type == sensor_type &&
		    latest->sensor_index == sensor_index)
			goto found;
	}

	return -ENODATA;

found:
	if (!latest->valid)
		return -ENODATA;
	if (channel >= latest->channel_count)
		return -ENODATA;

	*value = latest->values[channel];

	return 0;
}
