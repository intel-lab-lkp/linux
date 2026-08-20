// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device-id/of.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
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

static int osf_serdev_receive_frame(void *context, const u8 *buf, size_t len)
{
	struct osf_device *osf = context;

	return osf_core_receive_frame(osf, buf, len);
}

static size_t osf_serdev_receive_buf(struct serdev_device *serdev,
				     const u8 *buf, size_t count)
{
	struct osf_serdev *osf_uart = serdev_device_get_drvdata(serdev);
	const struct osf_stream_stats *stats;
	u64 authenticated_before;
	int ret;

	authenticated_before = osf_uart->stream.stats.authenticated_frames;
	ret = osf_stream_receive_bytes(&osf_uart->stream, buf, count);
	stats = &osf_uart->stream.stats;

	if (ret || stats->authenticated_frames != authenticated_before)
		dev_dbg_ratelimited(&serdev->dev,
				    "rx count=%zu authenticated=%llu handled=%llu ignored=%llu rejected=%llu bad_magic=%llu bad_crc=%llu dropped=%llu ret=%d\n",
				    count, stats->authenticated_frames,
				    stats->handled_frames, stats->ignored_frames,
				    stats->rejected_frames,
				    stats->bad_magic_resyncs,
				    stats->bad_crc_frames,
				    stats->dropped_bytes, ret);

	return count;
}

static const struct serdev_device_ops osf_serdev_ops = {
	.receive_buf = osf_serdev_receive_buf,
};

static int osf_serdev_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct osf_serdev *osf_uart;
	unsigned int baudrate;
	int ret;

	osf_uart = devm_kzalloc(dev, sizeof(*osf_uart), GFP_KERNEL);
	if (!osf_uart)
		return -ENOMEM;

	osf_uart->serdev = serdev;
	osf_core_init(&osf_uart->osf, dev);
	osf_stream_init(&osf_uart->stream, osf_serdev_receive_frame,
			&osf_uart->osf);

	serdev_device_set_drvdata(serdev, osf_uart);
	serdev_device_set_client_ops(serdev, &osf_serdev_ops);

	ret = devm_regulator_get_enable(dev, "vcc");
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable vcc regulator\n");

	ret = serdev_device_open(serdev);
	if (ret)
		return ret;

	baudrate = serdev_device_set_baudrate(serdev, OSF_SERDEV_BAUD);
	if (baudrate != OSF_SERDEV_BAUD)
		dev_warn(dev, "requested %u baud, controller set %u\n",
			 OSF_SERDEV_BAUD, baudrate);

	serdev_device_set_flow_control(serdev, false);

	return 0;
}

static void osf_serdev_remove(struct serdev_device *serdev)
{
	struct osf_serdev *osf_uart = serdev_device_get_drvdata(serdev);

	/* Stop the RX producer before unregistering IIO consumers. */
	serdev_device_close(serdev);
	osf_stream_reset(&osf_uart->stream);
	osf_core_unregister_iio(&osf_uart->osf);
}

static const struct of_device_id osf_serdev_of_match[] = {
	{ .compatible = "opensensorfusion,osf" },
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
