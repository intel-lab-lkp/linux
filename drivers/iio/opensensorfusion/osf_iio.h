/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OSF_IIO_H
#define _OSF_IIO_H

#include <linux/types.h>

#include "osf_protocol.h"

struct device;
struct iio_dev;
struct osf_sample_event;

int osf_iio_register_sensor(struct device *dev,
			    const struct osf_capability_entry *entry,
			    void *driver_data, struct iio_dev **indio_dev);
int osf_iio_push_sample(struct iio_dev *indio_dev,
			const struct osf_sample_event *event);
bool osf_iio_sensor_supported(u16 sensor_type, u16 channel_count);
const char *osf_iio_sensor_name(u16 sensor_type);

#endif
