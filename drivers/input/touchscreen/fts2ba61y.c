// SPDX-License-Identifier: GPL-2.0
// Based loosely on s6sy761.c

#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/input/mt.h>
#include <linux/spi/spi.h>
#include <linux/input/touchscreen.h>
#include <linux/unaligned.h>
#include <linux/regulator/consumer.h>

/* commands */
#define FTS2BA61Y_CMD_SENSE_ON			0x10
#define FTS2BA61Y_CMD_SENSE_OFF			0x11
#define FTS2BA61Y_CMD_READ_PANEL_INFO		0x23
#define FTS2BA61Y_CMD_READ_FW_VER		0x24
#define FTS2BA61Y_CMD_TOUCHTYPE			0x30 /* R/W for get/set */
#define FTS2BA61Y_CMD_CLEAR_EVENTS		0x62
#define FTS2BA61Y_CMD_READ_EVENT		0x87
#define FTS2BA61Y_CMD_CUSTOM_W			0xC0
#define FTS2BA61Y_CMD_CUSTOM_R			0xD1
#define FTS2BA61Y_CMD_REG_W			0xFA
#define FTS2BA61Y_CMD_REG_R			0xFB

/* touch type masks */
#define FTS2BA61Y_MASK_TOUCH			BIT(0)
#define FTS2BA61Y_MASK_HOVER			BIT(1)
#define FTS2BA61Y_MASK_COVER			BIT(2)
#define FTS2BA61Y_MASK_GLOVE			BIT(3)
#define FTS2BA61Y_MASK_STYLUS			BIT(4)
#define FTS2BA61Y_MASK_PALM			BIT(5)
#define FTS2BA61Y_MASK_WET			BIT(6)
#define FTS2BA61Y_TOUCHTYPE_DEFAULT		(FTS2BA61Y_MASK_TOUCH | \
						 FTS2BA61Y_MASK_PALM | \
						 FTS2BA61Y_MASK_WET)

/* event status masks */
#define FTS2BA61Y_MASK_STYPE			GENMASK(5, 2)
#define FTS2BA61Y_MASK_EVENT_ID			GENMASK(1, 0)

/* event coordinate masks */
#define FTS2BA61Y_MASK_TCHSTA			GENMASK(7, 6)
#define FTS2BA61Y_MASK_TID			GENMASK(5, 2)
#define FTS2BA61Y_MASK_X_3_0			GENMASK(7, 4)
#define FTS2BA61Y_MASK_Y_3_0			GENMASK(3, 0)
#define FTS2BA61Y_MASK_Z			GENMASK(5, 0)
#define FTS2BA61Y_MASK_TTYPE_3_2		GENMASK(7, 6)
#define FTS2BA61Y_MASK_TTYPE_1_0		GENMASK(1, 0)
#define FTS2BA61Y_MASK_LEFT_EVENTS		GENMASK(4, 0)

/* event error status */
#define FTS2BA61Y_EVENT_STATUSTYPE_INFO		0x2

/* information report */
#define FTS2BA61Y_INFO_READY_STATUS		0x0

/* event status */
#define FTS2BA61Y_COORDINATE_EVENT		0x0

/* touch types */
#define FTS2BA61Y_TOUCHTYPE_NORMAL		0x0
#define FTS2BA61Y_TOUCHTYPE_HOVER		0x1
#define FTS2BA61Y_TOUCHTYPE_FLIPCOVER		0x2
#define FTS2BA61Y_TOUCHTYPE_GLOVE		0x3
#define FTS2BA61Y_TOUCHTYPE_STYLUS		0x4
#define FTS2BA61Y_TOUCHTYPE_PALM		0x5
#define FTS2BA61Y_TOUCHTYPE_WET			0x6
#define FTS2BA61Y_TOUCHTYPE_PROXIMITY		0x7
#define FTS2BA61Y_TOUCHTYPE_JIG			0x8

#define FTS2BA61Y_COORDINATE_ACTION_NONE	0x0
#define FTS2BA61Y_COORDINATE_ACTION_PRESS	0x1
#define FTS2BA61Y_COORDINATE_ACTION_MOVE	0x2
#define FTS2BA61Y_COORDINATE_ACTION_RELEASE	0x3

#define FTS2BA61Y_DEV_NAME			"fts2ba61y"
#define FTS2BA61Y_EVENT_BUFF_SIZE		16
#define FTS2BA61Y_PANEL_INFO_SIZE		11
#define FTS2BA61Y_RESET_CMD_SIZE		5
#define FTS2BA61Y_EVENT_COUNT			31
#define MAX_TRANSFER_SIZE			256

enum fts2ba61y_regulators {
	FTS2BA61Y_REGULATOR_VDD,
	FTS2BA61Y_REGULATOR_AVDD,
};

struct fts2ba61y_data {
	struct spi_device *spi;
	struct regulator_bulk_data regulators[2];
	struct input_dev *input_dev;
	struct mutex mutex;
	struct touchscreen_properties prop;

	u8 tx_count;

	unsigned int max_x;
	unsigned int max_y;
};

static int fts2ba61y_write(struct fts2ba61y_data *ts,
			   u8 *reg, int cmd_len, u8 *data, int data_len)
{
	struct spi_message msg;
	struct spi_transfer xfers;
	char *tx_buf;
	int len;
	int ret;

	tx_buf = kzalloc(cmd_len + data_len + 1, GFP_KERNEL);
	if (!tx_buf) {
		ret = -ENOMEM;
		goto out;
	}

	memset(&xfers, 0, sizeof(xfers));
	spi_message_init(&msg);

	memcpy(&tx_buf[0], reg, cmd_len);
	if (data_len && data)
		memcpy(&tx_buf[cmd_len], data, data_len);

	len = cmd_len + data_len;

	/* custom write cmd */
	if (reg[0] != FTS2BA61Y_CMD_REG_W &&
	    reg[0] != FTS2BA61Y_CMD_REG_R) {
		memmove(tx_buf + 1, tx_buf, len);
		tx_buf[0] = FTS2BA61Y_CMD_CUSTOM_W;
		len++;
	}

	xfers.len = len;
	xfers.tx_buf = tx_buf;

	spi_message_add_tail(&xfers, &msg);

	mutex_lock(&ts->mutex);
	ret = spi_sync(ts->spi, &msg);
	if (ret)
		dev_err(&ts->spi->dev, "spi transfer error, %d", ret);
	mutex_unlock(&ts->mutex);

out:
	kfree(tx_buf);
	return ret;
}

static int fts2ba61y_spi_raw_read(struct fts2ba61y_data *ts,
				  u8 *tx_buf, u8 *rx_buf, int len)
{
	struct spi_message msg;
	struct spi_transfer xfer;
	int ret;

	memset(&xfer, 0, sizeof(xfer));
	spi_message_init(&msg);

	xfer.len = len;
	xfer.tx_buf = tx_buf;
	xfer.rx_buf = rx_buf;
	spi_message_add_tail(&xfer, &msg);

	mutex_lock(&ts->mutex);
	ret = spi_sync(ts->spi, &msg);
	if (ret)
		dev_err(&ts->spi->dev, "spi transfer error, %d", ret);
	mutex_unlock(&ts->mutex);

	return ret;
}

/*
 * higher-level wrapper that prepares the buffers for a read.
 */
static int fts2ba61y_read(struct fts2ba61y_data *ts,
			  u8 reg[], int tx_len, u8 buf[], int rx_len)
{
	char *tx_buf, *rx_buf;
	int ret, mem_len;
	u16 reg_val;

	if (tx_len > 3)
		mem_len = rx_len + 1 + tx_len;
	else
		mem_len = rx_len + 4;

	tx_buf = kzalloc(mem_len, GFP_KERNEL);
	rx_buf = kzalloc(mem_len, GFP_KERNEL);
	if (!tx_buf || !rx_buf) {
		ret = -ENOMEM;
		goto out;
	}

	switch (reg[0]) {
	case FTS2BA61Y_CMD_READ_EVENT:
	case FTS2BA61Y_CMD_REG_W:
	case FTS2BA61Y_CMD_REG_R:
		memcpy(tx_buf, reg, tx_len);
		break;

	default:
		tx_buf[0] = FTS2BA61Y_CMD_CUSTOM_R;

		if (tx_len == 1)
			reg_val = 0;
		else if (tx_len == 2)
			reg_val = reg[0];
		else if (tx_len == 3)
			reg_val = reg[0] | (reg[1] << 8);
		else {
			ret = -EINVAL;
			goto out;
		}

		tx_len = 3;
		put_unaligned_be16(reg_val, &tx_buf[1]);

		ret = fts2ba61y_write(ts, reg, 1, NULL, 0);
		if (ret < 0)
			goto out;
		break;
	}

	ret = fts2ba61y_spi_raw_read(ts, tx_buf, rx_buf, rx_len + 1 + tx_len);
	if (ret < 0)
		goto out;

	memcpy(buf, &rx_buf[1 + tx_len], rx_len);

out:
	kfree(tx_buf);
	kfree(rx_buf);
	return ret;
}

static int fts2ba61y_wait_for_ready(struct fts2ba61y_data *ts)
{
	u8 buffer[FTS2BA61Y_EVENT_BUFF_SIZE];
	u8 cmd = FTS2BA61Y_CMD_READ_EVENT;
	u8 status_id, stype;
	int ret;

	for (int retries = 5; retries > 0; retries--) {
		ret = fts2ba61y_read(ts, &cmd, 1, buffer, FTS2BA61Y_EVENT_BUFF_SIZE);

		stype = FIELD_GET(FTS2BA61Y_MASK_STYPE, buffer[0]);
		status_id = buffer[1];

		if (stype == FTS2BA61Y_EVENT_STATUSTYPE_INFO &&
		    status_id == FTS2BA61Y_INFO_READY_STATUS) {
			ret = 0;
			break;
		} else
			ret = -ENODEV;

		msleep(20);
	}

	return ret;
}

static int fts2ba61y_reset(struct fts2ba61y_data *ts)
{
	u8 cmd = FTS2BA61Y_CMD_REG_W;
	/* the following sequence is undocumented */
	u8 reset[FTS2BA61Y_RESET_CMD_SIZE] = { 0x20, 0x00,
					       0x00, 0x24, 0x81 };
	int ret;

	disable_irq(ts->spi->irq);

	ret = fts2ba61y_write(ts, &cmd, 1, &reset[0], FTS2BA61Y_RESET_CMD_SIZE);
	if (ret)
		return ret;
	msleep(30);

	ret = fts2ba61y_wait_for_ready(ts);
	if (ret)
		return ret;

	enable_irq(ts->spi->irq);

	return 0;
}

static int fts2ba61y_set_channels(struct fts2ba61y_data *ts)
{
	int ret;
	u8 cmd = FTS2BA61Y_CMD_READ_PANEL_INFO;
	u8 data[FTS2BA61Y_PANEL_INFO_SIZE];

	ret = fts2ba61y_read(ts, &cmd, 1, data, FTS2BA61Y_PANEL_INFO_SIZE);
	if (ret)
		return ret;

	ts->max_x = get_unaligned_be16(data);
	ts->max_y = get_unaligned_be16(data + 2);

	/* if no tx channels defined, at least keep one */
	ts->tx_count = max_t(u8, data[8], 1);

	return 0;
}

static int fts2ba61y_set_touch_func(struct fts2ba61y_data *ts)
{
	u8 cmd = FTS2BA61Y_CMD_TOUCHTYPE;
	u16 touchtype = cpu_to_le16(FTS2BA61Y_TOUCHTYPE_DEFAULT);

	return fts2ba61y_write(ts, &cmd, 1, (u8 *)&touchtype, 2);
}

static int fts2ba61y_hw_init(struct fts2ba61y_data *ts)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ts->regulators),
								ts->regulators);
	if (ret)
		return ret;

	msleep(140);

	ret = fts2ba61y_reset(ts);
	if (ret)
		return ret;

	ret = fts2ba61y_set_channels(ts);
	if (ret)
		return ret;

	return fts2ba61y_set_touch_func(ts);
}

static int fts2ba61y_get_event(struct fts2ba61y_data *ts, u8 *data, int *n_events)
{
	int ret;
	u8 cmd = FTS2BA61Y_CMD_READ_EVENT;

	ret = fts2ba61y_read(ts, &cmd, 1, data, FTS2BA61Y_EVENT_BUFF_SIZE);
	if (ret < 0)
		return ret;

	if (!data[0]) {
		*n_events = 0;
		return 0;
	}

	*n_events = FIELD_GET(FTS2BA61Y_MASK_LEFT_EVENTS, data[7]);
	if (unlikely(*n_events >= FTS2BA61Y_EVENT_COUNT)) {
		cmd = FTS2BA61Y_CMD_CLEAR_EVENTS;
		fts2ba61y_write(ts, &cmd, 1, NULL, 0);
		*n_events = 0;
		return -EINVAL;
	}

	if (*n_events > 0) {
		ret = fts2ba61y_read(ts, &cmd, 1,
				     &data[1 * FTS2BA61Y_EVENT_BUFF_SIZE],
				     FTS2BA61Y_EVENT_BUFF_SIZE * (*n_events));
		if (ret)
			return ret;
	}

	return 0;
}

static void fts2ba61y_report_coordinates(struct fts2ba61y_data *ts,
					 u8 *event, u8 tid)
{
	u8 major = event[4];
	u8 minor = event[5];
	u8 z = FIELD_GET(FTS2BA61Y_MASK_Z, event[6]);

	u16 x = (event[1] << 4) |
		FIELD_GET(FTS2BA61Y_MASK_X_3_0, event[3]);
	u16 y = (event[2] << 4) |
		FIELD_GET(FTS2BA61Y_MASK_Y_3_0, event[3]);
	u16 ttype = (FIELD_GET(FTS2BA61Y_MASK_TTYPE_3_2, event[6]) << 2) |
		    (FIELD_GET(FTS2BA61Y_MASK_TTYPE_1_0, event[7]) << 0);

	if (ttype != FTS2BA61Y_TOUCHTYPE_NORMAL &&
	    ttype != FTS2BA61Y_TOUCHTYPE_PALM &&
	    ttype != FTS2BA61Y_TOUCHTYPE_WET &&
	    ttype != FTS2BA61Y_TOUCHTYPE_GLOVE)
		return;

	input_mt_slot(ts->input_dev, tid);
	input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, major);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MINOR, minor);
	input_report_abs(ts->input_dev, ABS_MT_PRESSURE, z);

	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);
}

static void fts2ba61y_report_release(struct fts2ba61y_data *ts, u8 tid)
{
	input_mt_slot(ts->input_dev, tid);
	input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);

	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);
}

static void fts2ba61y_handle_coordinates(struct fts2ba61y_data *ts, u8 *event)
{
	u8 t_id = FIELD_GET(FTS2BA61Y_MASK_TID, event[0]);
	u8 action = FIELD_GET(FTS2BA61Y_MASK_TCHSTA, event[0]);

	if (t_id > ts->tx_count)
		return;

	switch (action) {
	case FTS2BA61Y_COORDINATE_ACTION_PRESS:
	case FTS2BA61Y_COORDINATE_ACTION_MOVE:
		fts2ba61y_report_coordinates(ts, event, t_id);
		break;

	case FTS2BA61Y_COORDINATE_ACTION_RELEASE:
		fts2ba61y_report_release(ts, t_id);
		break;
	}
}

static irqreturn_t fts2ba61y_irq_handler(int irq, void *handle)
{
	struct fts2ba61y_data *ts = handle;
	u8 buffer[FTS2BA61Y_EVENT_COUNT * FTS2BA61Y_EVENT_BUFF_SIZE];
	u8 *event;
	u8 event_id;
	int n_events = 0;
	int ret;

	usleep(1);

	ret = fts2ba61y_get_event(ts, buffer, &n_events);
	if (ret < 0) {
		dev_dbg(&ts->spi->dev, "failed to get event: %d", ret);
		return IRQ_HANDLED;
	}

	for (int i = 0; i <= n_events; i++) {
		event = &buffer[i * FTS2BA61Y_EVENT_BUFF_SIZE];
		event_id = FIELD_GET(FTS2BA61Y_MASK_EVENT_ID, event[0]);

		if (event_id == FTS2BA61Y_COORDINATE_EVENT)
			fts2ba61y_handle_coordinates(ts, event);
	}

	return IRQ_HANDLED;
}

static int fts2ba61y_input_open(struct input_dev *dev)
{
	struct fts2ba61y_data *ts = input_get_drvdata(dev);
	u8 cmd = FTS2BA61Y_CMD_SENSE_ON;

	return fts2ba61y_write(ts, &cmd, 1, NULL, 0);
}

static void fts2ba61y_input_close(struct input_dev *dev)
{
	struct fts2ba61y_data *ts = input_get_drvdata(dev);
	int ret;
	u8 cmd = FTS2BA61Y_CMD_SENSE_OFF;

	ret = fts2ba61y_write(ts, &cmd, 1, NULL, 0);
	if (ret)
		dev_err(&ts->spi->dev, "failed to turn off sensing\n");
}

static void fts2ba61y_power_off(void *data)
{
	struct fts2ba61y_data *ts = data;

	disable_irq(ts->spi->irq);
	regulator_bulk_disable(ARRAY_SIZE(ts->regulators),
						   ts->regulators);
}

static int fts2ba61y_probe(struct spi_device *spi) {
	struct fts2ba61y_data *ts;
	struct input_dev *input_dev;
	int error;

	ts = devm_kzalloc(&spi->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->spi = spi;
	mutex_init(&ts->mutex);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;

	error = spi_setup(spi);
	if (error)
		return error;

	ts->regulators[FTS2BA61Y_REGULATOR_VDD].supply = "vdd";
	ts->regulators[FTS2BA61Y_REGULATOR_AVDD].supply = "avdd";
	error = devm_regulator_bulk_get(&spi->dev,
									ARRAY_SIZE(ts->regulators),
									ts->regulators);
	if (error)
		return error;

	error = fts2ba61y_hw_init(ts);
	if (error)
		return error;

	error = devm_add_action_or_reset(&ts->spi->dev, fts2ba61y_power_off, ts);
	if (error)
		return error;

	input_dev = devm_input_allocate_device(&spi->dev);
	if (!input_dev)
		return -ENOMEM;

	ts->input_dev = input_dev;

	input_dev->name = FTS2BA61Y_DEV_NAME;
	input_dev->id.bustype = BUS_SPI;
	input_dev->open = fts2ba61y_input_open;
	input_dev->close = fts2ba61y_input_close;

	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, ts->max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, ts->max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);

	touchscreen_parse_properties(input_dev, true, &ts->prop);

	spi_set_drvdata(spi, ts);
	input_set_drvdata(input_dev, ts);

	error = input_mt_init_slots(input_dev, ts->tx_count, INPUT_MT_DIRECT);
	if (error)
		return error;

	error = input_register_device(input_dev);
	if (error)
		return error;

	error = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
					  fts2ba61y_irq_handler,
					  IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					  "fts2ba61y_irq", ts);
	return error;
}

#ifdef CONFIG_OF
static const struct of_device_id spi_touchscreen_dt_ids[] = {
	{ .compatible = "st,fts2ba61y" },
	{},
};
MODULE_DEVICE_TABLE(of, spi_touchscreen_dt_ids);
#endif

static const struct spi_device_id fts2ba61y_spi_ids[] = {
	{ "fts2ba61y" },
	{ },
};
MODULE_DEVICE_TABLE(spi, fts2ba61y_spi_ids);

static struct spi_driver spi_touchscreen_driver = {
	.driver = {
		.name = FTS2BA61Y_DEV_NAME,
		.of_match_table = of_match_ptr(spi_touchscreen_dt_ids),
	},
	.probe = fts2ba61y_probe,
	.id_table = fts2ba61y_spi_ids,
};

module_spi_driver(spi_touchscreen_driver);

MODULE_AUTHOR("Ivaylo Ivanov <ivo.ivanov.ivanov1@gmail.com>");
MODULE_DESCRIPTION("ST-Microelectronics FTS2BA61Y Touch Screen");
MODULE_LICENSE("GPL");
