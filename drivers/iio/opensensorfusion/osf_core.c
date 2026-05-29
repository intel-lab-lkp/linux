// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include "osf_core.h"
#include "osf_protocol.h"

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
}

static int osf_core_validate_sensor_sample(const struct osf_frame *frame)
{
	struct osf_sensor_sample sample;

	return osf_protocol_decode_sensor_sample(frame, &sample);
}

static int osf_core_validate_device_status(const struct osf_frame *frame)
{
	struct osf_device_status status;
	int ret;

	ret = osf_protocol_decode_device_status(frame, &status);
	if (ret)
		return ret;

	if (status.reserved)
		return -EPROTO;

	return 0;
}

static int osf_core_validate_capability_report(const struct osf_frame *frame)
{
	struct osf_capability_entry entry;
	struct osf_capability_report report;
	unsigned int i;
	int ret;

	ret = osf_protocol_decode_capability_report(frame, &report);
	if (ret)
		return ret;

	for (i = 0; i < report.capability_count; i++) {
		ret = osf_protocol_decode_capability_entry(&report, i, &entry);
		if (ret)
			return ret;
	}

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
		ret = osf_core_validate_sensor_sample(&frame);
		break;
	case OSF_MSG_DEVICE_STATUS:
		ret = osf_core_validate_device_status(&frame);
		break;
	case OSF_MSG_CAPABILITY_REPORT:
		ret = osf_core_validate_capability_report(&frame);
		break;
	default:
		if (frame.message_type >= OSF_RESERVED_MSG_FIRST &&
		    frame.message_type <= OSF_RESERVED_MSG_LAST)
			ret = 0;
		else if (frame.message_type >= OSF_VENDOR_PRIVATE_FIRST)
			ret = 0;
		else
			ret = -EOPNOTSUPP;
		break;
	}

	if (!ret)
		osf->last_sequence = frame.sequence;

	return ret;
}
