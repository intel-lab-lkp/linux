// SPDX-License-Identifier: GPL-2.0
/*
 * ACD10 CO2 gas sensor driver
 *
 * Copyright (c) 2026 Akhilesh Patil <akhilesh.patil@linux.dev>
 *
 * Datasheet:
 * https://www.es.co.th/Schemetic/PDF/ACD10.PDF
 *
 * TODO additional features:
 *	1) single-point calibration feature.
 *	2) Sensor preheat wait feature.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/serdev.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define ACD10_SERIAL_BAUDRATE		1200
#define ACD10_SERDEV_TIMEOUT		msecs_to_jiffies(500)
#define ACD10_FRAME_START		0xFE
#define ACD10_FRAME_FIXED_CODE		0xA6
#define ACD10_FRAME_DATA_START		4U
#define ACD10_FRAME_DATA_LEN_IDX	2U
#define ACD10_FRAME_DATA_LEN_MAX	11U
#define ACD10_FRAME_CMD_CODE_IDX	3U

struct acd10_state {
	struct serdev_device *serdev;

	/* Lock to acquire during CMD/RESP sequence */
	struct mutex lock;
	struct completion buf_ready;

	/* Single buffer to hold tx and rx data */
	u8 buf[16];
	u8 buf_idx;
	u8 rx_expected_len;

	u8 sw_rev[16];
	u8 inst_num[16];
};

enum acd10_cmds {
	ACD10_CMD_READ_CO2 = 0,
	ACD10_CMD_CAL,
	ACD10_CMD_READ_SWREV,
	ACD10_CMD_READ_INS_NUM,
};

struct acd10_cmd_info {
	u8 cmd_code;
	u8 tx_frame_len;
	u8 tx_data_len;
	u8 rx_frame_len;
};

static const struct acd10_cmd_info cmd_info[] = {
	[ACD10_CMD_READ_CO2] = {
		.cmd_code = 0x01,
		.tx_frame_len = 5,
		.tx_data_len = 0,
		.rx_frame_len = 9,
	},

	[ACD10_CMD_CAL] = {
		.cmd_code = 0x03,
		.tx_frame_len = 7,
		.tx_data_len = 2,
		.rx_frame_len = 5,
	},

	[ACD10_CMD_READ_SWREV] = {
		.cmd_code = 0x1E,
		.tx_frame_len = 5,
		.tx_data_len = 0,
		/*
		 * Note: datasheet says sw rev response is D1-D11 with D11 reserved.
		 * However, sensor actually sends only D1-D10 (10 bytes).
		 * Hence, rx_frame_len is 15 and not 16.
		 */
		.rx_frame_len = 15,
	},

	[ACD10_CMD_READ_INS_NUM] = {
		.cmd_code = 0x1F,
		.tx_frame_len = 5,
		.tx_data_len = 0,
		.rx_frame_len = 15,
	}
};

static u8 acd10_get_checksum(u8 *buf, u8 len)
{
	u8 checksum = 0;

	for (u8 i = 1; i < len - 1; i += 1)
		checksum += buf[i];

	return checksum;
}

/*
 * Do transfer based on command and fill drv_state->buf with received data.
 * Caller of this function should use mutex drv_state->lock.
 */
static int acd10_serdev_transfer(struct iio_dev *indio_dev, enum acd10_cmds cmd)
{
	struct acd10_state *drv_state = iio_priv(indio_dev);
	struct serdev_device *serdev = drv_state->serdev;
	struct device *dev = &indio_dev->dev;
	int ret;

	/* Prepare transmit buffer along with checksum */
	drv_state->buf[0] = ACD10_FRAME_START;
	drv_state->buf[1] = ACD10_FRAME_FIXED_CODE;
	drv_state->buf[ACD10_FRAME_DATA_LEN_IDX] = cmd_info[cmd].tx_data_len;
	drv_state->buf[ACD10_FRAME_CMD_CODE_IDX] = cmd_info[cmd].cmd_code;
	drv_state->buf[cmd_info[cmd].tx_frame_len - 1] =
		acd10_get_checksum(drv_state->buf, cmd_info[cmd].tx_frame_len);
	drv_state->buf_idx = 0;
	drv_state->rx_expected_len = cmd_info[cmd].rx_frame_len;
	reinit_completion(&drv_state->buf_ready);

	/* Transmit the buffer over serdev */
	ret = serdev_device_write(serdev, drv_state->buf, cmd_info[cmd].tx_frame_len,
				  ACD10_SERDEV_TIMEOUT);
	if (ret < 0)
		return ret;
	if (ret != cmd_info[cmd].tx_frame_len)
		return -EIO;

	/* Read the received bytes in buffer */
	ret = wait_for_completion_interruptible_timeout(&drv_state->buf_ready,
							ACD10_SERDEV_TIMEOUT);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;

	/* validate frame header */
	if (drv_state->buf[0] != ACD10_FRAME_START ||
	    drv_state->buf[1] != ACD10_FRAME_FIXED_CODE) {
		dev_err(dev, "RX frame invalid\n");
		return -EIO;
	}

	/* Maximum data len can be max 11 */
	if (drv_state->buf[ACD10_FRAME_DATA_LEN_IDX] > ACD10_FRAME_DATA_LEN_MAX)
		return -EIO;

	/* Validate command echo */
	if (drv_state->buf[ACD10_FRAME_CMD_CODE_IDX] != cmd_info[cmd].cmd_code)
		return -EIO;

	/* Validate checksum */
	if (drv_state->buf[cmd_info[cmd].rx_frame_len - 1] !=
	    acd10_get_checksum(drv_state->buf, cmd_info[cmd].rx_frame_len)) {
		dev_err(dev, "Checksum error\n");
		return -EIO;
	}

	return 0;
}

static int acd10_get_sw_rev(struct iio_dev *indio_dev)
{
	struct acd10_state *drv_state = iio_priv(indio_dev);
	struct device *dev = &indio_dev->dev;
	int ret;

	ret = acd10_serdev_transfer(indio_dev, ACD10_CMD_READ_SWREV);
	if (ret)
		return ret;

	memcpy(drv_state->sw_rev, &drv_state->buf[ACD10_FRAME_DATA_START], 10);
	drv_state->sw_rev[10] = '\0';

	dev_info(dev, "Software revision: %s\n", drv_state->sw_rev);

	return 0;
}

static int acd10_get_instrument_num(struct iio_dev *indio_dev)
{
	struct acd10_state *drv_state = iio_priv(indio_dev);
	struct device *dev = &indio_dev->dev;
	int ret;

	ret = acd10_serdev_transfer(indio_dev, ACD10_CMD_READ_INS_NUM);
	if (ret)
		return ret;

	memcpy(drv_state->inst_num, &drv_state->buf[ACD10_FRAME_DATA_START], 10);
	drv_state->inst_num[10] = '\0';

	dev_info(dev, "Instrument number: %s\n", drv_state->inst_num);

	return 0;
}

static size_t acd10_receive_buf(struct serdev_device *serdev,
				const u8 *data, size_t len)
{
	struct iio_dev *indio_dev = serdev_device_get_drvdata(serdev);
	struct acd10_state *drv_state = iio_priv(indio_dev);
	size_t consume = len;
	int i = 0;

	while (drv_state->rx_expected_len && (consume > 0)) {
		drv_state->buf[drv_state->buf_idx++] = data[i++];
		consume -= 1;
		drv_state->rx_expected_len -= 1;
	}

	if (drv_state->rx_expected_len == 0) {
		drv_state->buf_idx = 0;
		complete(&drv_state->buf_ready);
	}

	return len;
}

static int acd10_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan,
			  int *val, int *val2, long mask)
{
	struct acd10_state *drv_state = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:

		/* Read CO2 sensor value*/
		scoped_guard(mutex, &drv_state->lock) {
			ret = acd10_serdev_transfer(indio_dev, ACD10_CMD_READ_CO2);
			if (ret)
				return ret;

			*val = get_unaligned_be16(&drv_state->buf[ACD10_FRAME_DATA_START]);
		}

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 100;
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static const struct iio_info acd10_info = {
	.read_raw = &acd10_read_raw,
};

static const struct iio_chan_spec acd10_channels[] = {
	{
		.type = IIO_CONCENTRATION,
		.channel2 = IIO_MOD_CO2,
		.modified = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

static const struct serdev_device_ops acd10_ser_ops = {
	.receive_buf = &acd10_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

static int acd10_probe(struct serdev_device *serdev)
{
	int ret;
	struct device *dev = &serdev->dev;
	struct iio_dev *indio_dev;
	struct acd10_state *drv_state;
	unsigned int baudrate;

	indio_dev = devm_iio_device_alloc(dev, sizeof(struct acd10_state));
	if (!indio_dev)
		return -ENOMEM;

	indio_dev->name = "acd10";
	indio_dev->channels = acd10_channels;
	indio_dev->num_channels = ARRAY_SIZE(acd10_channels);
	indio_dev->info = &acd10_info;

	serdev_device_set_drvdata(serdev, indio_dev);

	drv_state = iio_priv(indio_dev);
	drv_state->serdev = serdev;

	init_completion(&drv_state->buf_ready);
	ret = devm_mutex_init(dev, &drv_state->lock);
	if (ret)
		return ret;

	serdev_device_set_client_ops(serdev, &acd10_ser_ops);
	ret = devm_serdev_device_open(dev, serdev);
	if (ret) {
		dev_err(dev, "ACD10: serial device open failed\n");
		return ret;
	}

	baudrate = serdev_device_set_baudrate(serdev, ACD10_SERIAL_BAUDRATE);
	dev_info(dev, "baud rate set to: %u\n", baudrate);
	if (baudrate != ACD10_SERIAL_BAUDRATE) {
		dev_err(dev, "baud rate expected: %u, got: %u\n",
			ACD10_SERIAL_BAUDRATE, baudrate);
		return -EIO;
	}
	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret)
		return ret;

	ret = acd10_get_sw_rev(indio_dev);
	if (ret)
		return ret;

	ret = acd10_get_instrument_num(indio_dev);
	if (ret)
		return ret;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id acd10_of_match_table[] = {
	{ .compatible = "aosong,acd10", },
	{ }
};

static struct serdev_device_driver acd10_driver = {
	.driver = {
		.name = "acd10",
		.of_match_table = acd10_of_match_table,
	},
	.probe = acd10_probe,
};
module_serdev_device_driver(acd10_driver);

MODULE_AUTHOR("Akhilesh Patil");
MODULE_DESCRIPTION("ACD10 CO2 gas sensor driver over serdev interface");
MODULE_LICENSE("GPL");
