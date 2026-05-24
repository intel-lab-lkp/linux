// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/units.h>

#include "osf_core.h"
#include "osf_iio.h"

struct osf_iio_sensor_spec {
	u16 sensor_type;
	u16 channel_count;
	const char *name;
	const struct iio_chan_spec *channels;
	unsigned int num_channels;
	const unsigned long *available_scan_masks;
};

struct osf_iio_state {
	const struct osf_iio_sensor_spec *spec;
	struct iio_buffer *buffer;
	u32 scale_nano;
	u16 sensor_index;
	struct osf_device *osf;
};

#define OSF_SCAN_TYPE_S32						\
	{								\
		.sign = 's',						\
		.realbits = 32,					\
		.storagebits = 32,					\
		.endianness = IIO_CPU,					\
	}

#define OSF_MOD_CHAN(_type, _mod, _idx)				\
	{								\
		.type = (_type),					\
		.modified = 1,					\
		.channel2 = (_mod),					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.scan_index = (_idx),					\
		.scan_type = OSF_SCAN_TYPE_S32,			\
	}

#define OSF_CHAN(_type, _idx)					\
	{								\
		.type = (_type),					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.scan_index = (_idx),					\
		.scan_type = OSF_SCAN_TYPE_S32,			\
	}

static const struct iio_chan_spec osf_accel_channels[] = {
	OSF_MOD_CHAN(IIO_ACCEL, IIO_MOD_X, 0),
	OSF_MOD_CHAN(IIO_ACCEL, IIO_MOD_Y, 1),
	OSF_MOD_CHAN(IIO_ACCEL, IIO_MOD_Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};

static const struct iio_chan_spec osf_gyro_channels[] = {
	OSF_MOD_CHAN(IIO_ANGL_VEL, IIO_MOD_X, 0),
	OSF_MOD_CHAN(IIO_ANGL_VEL, IIO_MOD_Y, 1),
	OSF_MOD_CHAN(IIO_ANGL_VEL, IIO_MOD_Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};

static const struct iio_chan_spec osf_mag_channels[] = {
	OSF_MOD_CHAN(IIO_MAGN, IIO_MOD_X, 0),
	OSF_MOD_CHAN(IIO_MAGN, IIO_MOD_Y, 1),
	OSF_MOD_CHAN(IIO_MAGN, IIO_MOD_Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};

static const struct iio_chan_spec osf_temp_channels[] = {
	OSF_CHAN(IIO_TEMP, 0),
	IIO_CHAN_SOFT_TIMESTAMP(1),
};

static const unsigned long osf_3axis_available_scan_masks[] = {
	GENMASK(2, 0),
	0,
};

static const unsigned long osf_temp_available_scan_masks[] = {
	BIT(0),
	0,
};

static const struct osf_iio_sensor_spec osf_iio_sensor_specs[] = {
	{
		.sensor_type = OSF_SENSOR_ACCELEROMETER,
		.channel_count = 3,
		.name = "osf-accel",
		.channels = osf_accel_channels,
		.num_channels = ARRAY_SIZE(osf_accel_channels),
		.available_scan_masks = osf_3axis_available_scan_masks,
	},
	{
		.sensor_type = OSF_SENSOR_GYROSCOPE,
		.channel_count = 3,
		.name = "osf-gyro",
		.channels = osf_gyro_channels,
		.num_channels = ARRAY_SIZE(osf_gyro_channels),
		.available_scan_masks = osf_3axis_available_scan_masks,
	},
	{
		.sensor_type = OSF_SENSOR_MAGNETOMETER,
		.channel_count = 3,
		.name = "osf-magn",
		.channels = osf_mag_channels,
		.num_channels = ARRAY_SIZE(osf_mag_channels),
		.available_scan_masks = osf_3axis_available_scan_masks,
	},
	{
		.sensor_type = OSF_SENSOR_TEMPERATURE,
		.channel_count = 1,
		.name = "osf-temp",
		.channels = osf_temp_channels,
		.num_channels = ARRAY_SIZE(osf_temp_channels),
		.available_scan_masks = osf_temp_available_scan_masks,
	},
};

static const struct osf_iio_sensor_spec *
osf_iio_find_sensor_spec(u16 sensor_type, u16 channel_count)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(osf_iio_sensor_specs); i++) {
		if (osf_iio_sensor_specs[i].sensor_type == sensor_type &&
		    osf_iio_sensor_specs[i].channel_count == channel_count)
			return &osf_iio_sensor_specs[i];
	}

	return NULL;
}

bool osf_iio_sensor_supported(u16 sensor_type, u16 channel_count)
{
	return !!osf_iio_find_sensor_spec(sensor_type, channel_count);
}

const char *osf_iio_sensor_name(u16 sensor_type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(osf_iio_sensor_specs); i++) {
		if (osf_iio_sensor_specs[i].sensor_type == sensor_type)
			return osf_iio_sensor_specs[i].name;
	}

	return NULL;
}

static int osf_iio_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan, int *val,
			    int *val2, long mask)
{
	struct osf_iio_state *state = iio_priv(indio_dev);
	s32 raw;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!state->osf)
			return -ENODATA;

		if (chan->scan_index < 0)
			return -EINVAL;

		ret = osf_core_read_latest_sample(state->osf,
						  state->spec->sensor_type,
						  state->sensor_index,
						  chan->scan_index, &raw);
		if (ret)
			return ret;

		*val = raw;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = state->scale_nano / NANO;
		*val2 = state->scale_nano % NANO;
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info osf_iio_info = {
	.read_raw = osf_iio_read_raw,
};

int osf_iio_register_sensor(struct device *dev,
			    const struct osf_capability_entry *entry,
			    struct osf_device *osf, struct iio_dev **indio_dev)
{
	const struct osf_iio_sensor_spec *spec;
	struct osf_iio_state *state;
	struct iio_dev *iio_dev;
	int ret;

	spec = osf_iio_find_sensor_spec(entry->sensor_type,
					entry->channel_count);
	if (!spec)
		return -EOPNOTSUPP;

	if (entry->sample_format != OSF_SAMPLE_FORMAT_S32)
		return -EOPNOTSUPP;

	iio_dev = iio_device_alloc(dev, sizeof(*state));
	if (!iio_dev)
		return -ENOMEM;

	state = iio_priv(iio_dev);
	state->spec = spec;
	state->scale_nano = entry->scale_nano;
	state->sensor_index = entry->sensor_index;
	state->osf = osf;

	iio_dev->name = spec->name;
	iio_dev->info = &osf_iio_info;
	iio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_SOFTWARE;
	iio_dev->channels = spec->channels;
	iio_dev->num_channels = spec->num_channels;
	iio_dev->available_scan_masks = spec->available_scan_masks;

	state->buffer = iio_kfifo_allocate();
	if (!state->buffer) {
		ret = -ENOMEM;
		goto err_free_iio;
	}

	ret = iio_device_attach_buffer(iio_dev, state->buffer);
	if (ret)
		goto err_free_buffer;

	ret = iio_device_register(iio_dev);
	if (ret)
		goto err_free_buffer;

	if (indio_dev)
		*indio_dev = iio_dev;

	return 0;

err_free_buffer:
	iio_kfifo_free(state->buffer);
err_free_iio:
	iio_device_free(iio_dev);

	return ret;
}

void osf_iio_unregister_sensor(struct iio_dev *indio_dev)
{
	struct osf_iio_state *state;

	if (!indio_dev)
		return;

	state = iio_priv(indio_dev);
	iio_device_unregister(indio_dev);
	iio_kfifo_free(state->buffer);
	iio_device_free(indio_dev);
}

int osf_iio_push_sample(struct iio_dev *indio_dev, const s32 *values,
			unsigned int channel_count)
{
	s64 timestamp;

	if (!iio_buffer_enabled(indio_dev))
		return 0;

	timestamp = iio_get_time_ns(indio_dev);

	return iio_push_to_buffers_with_ts_unaligned(indio_dev, values,
						     channel_count * sizeof(*values),
						     timestamp);
}
