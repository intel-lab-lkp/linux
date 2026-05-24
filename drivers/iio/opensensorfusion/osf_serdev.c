// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "osf_core.h"
#include "osf_stream.h"

#define OSF_SERDEV_BAUD		115200

struct osf_serdev {
	struct serdev_device *serdev;
	struct osf_device osf;
	struct osf_stream stream;
};

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
	osf_core_init(&osf_uart->osf, &serdev->dev);
	osf_stream_init(&osf_uart->stream, &osf_uart->osf);

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

	return 0;
}

static void osf_serdev_remove(struct serdev_device *serdev)
{
	struct osf_serdev *osf_uart = serdev_device_get_drvdata(serdev);

	serdev_device_close(serdev);
	osf_stream_reset(&osf_uart->stream);
	osf_core_unregister_iio(&osf_uart->osf);
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

module_serdev_device_driver(osf_serdev_driver);

MODULE_DESCRIPTION("Open Sensor Fusion IIO driver");
MODULE_LICENSE("GPL");
