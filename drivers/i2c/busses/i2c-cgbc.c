// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Congatec Board Controller I2C busses driver
 *
 * Copyright (C) 2024 Bootlin
 * Author: Thomas Richard <thomas.richard@bootlin.com>
 */

#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/mfd/cgbc.h>
#include <linux/module.h>
#include <linux/platform_data/i2c-cgbc.h>
#include <linux/platform_device.h>

#define CGBC_I2C_CMD_START	0x40
#define CGBC_I2C_CMD_STAT	0x48
#define CGBC_I2C_CMD_DATA	0x50
#define CGBC_I2C_CMD_SPEED	0x58

#define CGBC_I2C_STAT_IDL	0x00
#define CGBC_I2C_STAT_DAT	0x01
#define CGBC_I2C_STAT_BUSY	0x02

#define CGBC_I2C_START	0x80
#define CGBC_I2C_STOP	0x40

#define CGBC_I2C_LAST_ACK  0x80    /* send ACK on last read byte */

/*
 * Reference code defines 1kHz as min freq and 6.1MHz as max freq.
 * But in practice, the board controller limits the frequency to 1MHz, and the
 * 1kHz is not functional (minimal working freq is 50kHz).
 * So use these values as limits.
 */
#define CGBC_I2C_FREQ_MIN_HZ	50000	/* 50 kHz */
#define CGBC_I2C_FREQ_MAX_HZ	1000000 /* 1 MHz */

#define CGBC_I2C_FREQ_UNIT_1KHZ		0x40
#define CGBC_I2C_FREQ_UNIT_10KHZ	0x80
#define CGBC_I2C_FREQ_UNIT_100KHZ	0xC0

#define CGBC_I2C_FREQ_UNIT_MASK		0xC0
#define CGBC_I2C_FREQ_VALUE_MASK	0x3F

#define CGBC_I2C_READ_MAX_LEN	31
#define CGBC_I2C_WRITE_MAX_LEN	32

#define CGBC_I2C_CMD_HEADER_SIZE	4
#define CGBC_I2C_CMD_SIZE		(CGBC_I2C_CMD_HEADER_SIZE + CGBC_I2C_WRITE_MAX_LEN)

enum cgbc_i2c_state {
	CGBC_I2C_STATE_DONE = 0,
	CGBC_I2C_STATE_INIT,
	CGBC_I2C_STATE_START,
	CGBC_I2C_STATE_READ,
	CGBC_I2C_STATE_WRITE,
	CGBC_I2C_STATE_ERROR,
};

struct cgbc_i2c_data {
	struct device		*dev;
	struct cgbc_device_data *cgbc;
	struct i2c_adapter      adap;
	struct i2c_msg		*msg;
	int			nmsgs;
	int			pos;
	enum cgbc_i2c_state	state;
	u8			bus_id;
	unsigned long		read_maxtime_us;
};

struct cgbc_i2c_transfer {
	u8 bus_id;
	bool start;
	bool stop;
	bool last_ack;
	u8 read;
	u8 write;
	u8 addr;
	u8 data[CGBC_I2C_WRITE_MAX_LEN];
};

static u8 cgbc_i2c_freq_to_reg(unsigned int bus_frequency)
{
	u8 reg;

	if (bus_frequency <= 10000)
		reg = CGBC_I2C_FREQ_UNIT_1KHZ | (bus_frequency / 1000);
	else if (bus_frequency <= 100000)
		reg = CGBC_I2C_FREQ_UNIT_10KHZ | (bus_frequency / 10000);
	else
		reg = CGBC_I2C_FREQ_UNIT_100KHZ | (bus_frequency / 100000);

	return reg;
}

static unsigned int cgbc_i2c_reg_to_freq(u8 reg)
{
	unsigned int freq = reg & CGBC_I2C_FREQ_VALUE_MASK;
	u8 unit = reg & CGBC_I2C_FREQ_UNIT_MASK;

	if (unit == CGBC_I2C_FREQ_UNIT_100KHZ)
		return freq * 100000;
	else if (unit == CGBC_I2C_FREQ_UNIT_10KHZ)
		return freq * 10000;
	else
		return freq * 1000;
}

static int cgbc_i2c_get_status(struct i2c_adapter *adap)
{
	struct cgbc_i2c_data *i2c = i2c_get_adapdata(adap);
	struct cgbc_device_data *cgbc = i2c->cgbc;
	u8 cmd = CGBC_I2C_CMD_STAT | i2c->bus_id;
	u8 status;
	int ret;

	ret = cgbc_command(cgbc, &cmd, sizeof(cmd), NULL, 0, &status);
	if (ret)
		return ret;

	return status;
}

static int cgbc_i2c_set_frequency(struct i2c_adapter *adap,
				  unsigned int bus_frequency)
{
	struct cgbc_i2c_data *i2c = i2c_get_adapdata(adap);
	struct cgbc_device_data *cgbc = i2c->cgbc;
	u8 cmd[2], data;

	if (bus_frequency > CGBC_I2C_FREQ_MAX_HZ ||
	    bus_frequency < CGBC_I2C_FREQ_MIN_HZ) {
		dev_info(i2c->dev, "invalid frequency %u, using default\n", bus_frequency);
		bus_frequency = I2C_MAX_STANDARD_MODE_FREQ;
	}

	cmd[0] = CGBC_I2C_CMD_SPEED | i2c->bus_id;
	cmd[1] = cgbc_i2c_freq_to_reg(bus_frequency);

	return cgbc_command(cgbc, &cmd, sizeof(cmd), &data, 1, NULL);
}

static int cgbc_i2c_get_frequency(struct i2c_adapter *adap)
{
	struct cgbc_i2c_data *i2c = i2c_get_adapdata(adap);
	struct cgbc_device_data *cgbc = i2c->cgbc;
	u8 cmd[2] = { CGBC_I2C_CMD_SPEED | i2c->bus_id };
	unsigned int bus_frequency;
	u8 data;
	int ret;

	ret = cgbc_command(cgbc, &cmd, sizeof(cmd), &data, 1, NULL);
	if (ret)
		return ret;

	bus_frequency = cgbc_i2c_reg_to_freq(data);

	dev_dbg(i2c->dev, "%s is running at %d Hz\n", adap->name, bus_frequency);

	/*
	 * The read_maxtime_us variable represents the maximum time to wait
	 * for data during a read operation. The maximum amount of data that
	 * can be read by a command is CGBC_I2C_READ_MAX_LEN.
	 * Therefore, calculate the max time to properly size the timeout.
	 */
	i2c->read_maxtime_us = (BITS_PER_BYTE + 1) * CGBC_I2C_READ_MAX_LEN
		* USEC_PER_SEC / bus_frequency;

	return 0;
}

static unsigned int cgbc_i2c_xfer_to_cmd(struct cgbc_i2c_transfer xfer, u8 *cmd)
{
	int i = 0;

	cmd[i++] = CGBC_I2C_CMD_START | xfer.bus_id;

	cmd[i] = (xfer.start) ? CGBC_I2C_START : 0x00;
	if (xfer.stop)
		cmd[i] |= CGBC_I2C_STOP;
	cmd[i++] |= (xfer.start) ? xfer.write + 1 : xfer.write;

	cmd[i++] = (xfer.last_ack) ? (xfer.read | CGBC_I2C_LAST_ACK) : xfer.read;

	if (xfer.start)
		cmd[i++] = xfer.addr;

	if (xfer.write > 0)
		memcpy(&cmd[i], &xfer.data, xfer.write);

	return i + xfer.write;
}

static int cgbc_i2c_xfer_msg(struct i2c_adapter *adap)
{
	struct cgbc_i2c_data *i2c = i2c_get_adapdata(adap);
	struct cgbc_device_data *cgbc = i2c->cgbc;
	struct i2c_msg *msg = i2c->msg;
	u8 cmd[CGBC_I2C_CMD_SIZE];
	int ret, max_len, len, i;
	unsigned int cmd_len;
	u8 cmd_data;

	struct cgbc_i2c_transfer xfer = {
		.bus_id = i2c->bus_id,
		.addr = i2c_8bit_addr_from_msg(msg),
	};

	if (i2c->state == CGBC_I2C_STATE_DONE)
		return 0;

	ret = cgbc_i2c_get_status(adap);

	if (ret == CGBC_I2C_STAT_BUSY)
		return -EBUSY;
	else if (ret < 0)
		goto err;

	if (i2c->state == CGBC_I2C_STATE_INIT ||
	    (i2c->state == CGBC_I2C_STATE_WRITE && msg->flags & I2C_M_RD))
		xfer.start = true;

	i2c->state = (msg->flags & I2C_M_RD) ? CGBC_I2C_STATE_READ : CGBC_I2C_STATE_WRITE;

	max_len = (i2c->state == CGBC_I2C_STATE_READ) ?
		CGBC_I2C_READ_MAX_LEN : CGBC_I2C_WRITE_MAX_LEN;

	if (msg->len - i2c->pos > max_len) {
		len = max_len;
	} else {
		len = msg->len - i2c->pos;

		if (i2c->nmsgs == 1)
			xfer.stop = true;
	}

	if (i2c->state == CGBC_I2C_STATE_WRITE) {
		xfer.write = len;
		xfer.read = 0;

		for (i = 0; i < len; i++)
			xfer.data[i] = msg->buf[i2c->pos + i];

		cmd_len = cgbc_i2c_xfer_to_cmd(xfer, &cmd[0]);

		ret = cgbc_command(cgbc, &cmd, cmd_len, NULL, 0, NULL);
		if (ret)
			goto err;
	} else if (i2c->state == CGBC_I2C_STATE_READ) {
		xfer.write = 0;
		xfer.read = len;

		if (i2c->nmsgs > 1 || msg->len - i2c->pos > max_len)
			xfer.read |= CGBC_I2C_LAST_ACK;

		cmd_len = cgbc_i2c_xfer_to_cmd(xfer, &cmd[0]);
		ret = cgbc_command(cgbc, &cmd, cmd_len, NULL, 0, NULL);
		if (ret)
			goto err;

		ret = read_poll_timeout(cgbc_i2c_get_status, ret,
					ret != CGBC_I2C_STAT_BUSY, 0,
					2 * i2c->read_maxtime_us, false, adap);
		if (ret < 0)
			goto err;

		cmd_data = CGBC_I2C_CMD_DATA | i2c->bus_id;
		ret = cgbc_command(cgbc, &cmd_data, sizeof(cmd_data),
				   msg->buf + i2c->pos, len, NULL);
		if (ret)
			goto err;
	}

	if (len == (msg->len - i2c->pos)) {
		i2c->msg++;
		i2c->nmsgs--;
		i2c->pos = 0;
	} else {
		i2c->pos += len;
	}

	if (i2c->nmsgs == 0)
		i2c->state = CGBC_I2C_STATE_DONE;

	return 0;

err:
	i2c->state = CGBC_I2C_STATE_ERROR;
	return ret;
}

static int cgbc_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			 int num)
{
	struct cgbc_i2c_data *i2c = i2c_get_adapdata(adap);
	unsigned long timeout = jiffies + HZ;
	int ret;

	i2c->state = CGBC_I2C_STATE_INIT;
	i2c->msg = msgs;
	i2c->nmsgs = num;
	i2c->pos = 0;

	while (time_before(jiffies, timeout)) {
		ret = cgbc_i2c_xfer_msg(adap);
		if (i2c->state == CGBC_I2C_STATE_DONE)
			return num;

		if (i2c->state == CGBC_I2C_STATE_ERROR)
			return ret;

		if (ret == 0)
			timeout = jiffies + HZ;
	}

	i2c->state = CGBC_I2C_STATE_ERROR;
	return -ETIMEDOUT;
}

static u32 cgbc_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~(I2C_FUNC_SMBUS_QUICK));
}

static const struct i2c_algorithm cgbc_i2c_algorithm = {
	.xfer = cgbc_i2c_xfer,
	.functionality = cgbc_i2c_func,
};

static const struct i2c_adapter cgbc_i2c_adapter = {
	.owner		= THIS_MODULE,
	.class		= I2C_CLASS_DEPRECATED,
	.algo		= &cgbc_i2c_algorithm,
	.nr		= -1,
};

static int cgbc_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cgbc_device_data *cgbc = dev_get_drvdata(dev->parent);
	struct cgbc_i2c_platform_data *pdata;
	struct cgbc_i2c_data *i2c;
	int ret, i;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	pdata = dev_get_platdata(dev);
	if (!pdata)
		return dev_err_probe(dev, -ENODEV, "missing platform_data\n");

	i2c->cgbc = cgbc;
	i2c->dev = dev;
	i2c->adap = cgbc_i2c_adapter;
	i2c->adap.dev.parent = i2c->dev;
	i2c->bus_id = pdata->cgbc_bus_id;
	strscpy(i2c->adap.name, pdata->name, sizeof(i2c->adap.name));
	i2c_set_adapdata(&i2c->adap, i2c);
	platform_set_drvdata(pdev, i2c);

	if (!pdata->fixed_freq) {
		ret = cgbc_i2c_set_frequency(&i2c->adap, I2C_MAX_STANDARD_MODE_FREQ);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to set I2C bus frequency");
	}

	ret = cgbc_i2c_get_frequency(&i2c->adap);
	if (ret)
		return dev_err_probe(i2c->dev, ret, "Failed to get I2C bus frequency");

	ret = i2c_add_numbered_adapter(&i2c->adap);
	if (ret)
		return ret;

	/* Add known devices to the bus */
	for (i = 0; i < pdata->nb_devices; i++)
		i2c_new_client_device(&i2c->adap, pdata->devices + i);

	return 0;
}

static void cgbc_i2c_remove(struct platform_device *pdev)
{
	struct cgbc_i2c_data *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
}

static struct platform_driver cgbc_i2c_driver = {
	.driver = {
		.name = "cgbc-i2c",
	},
	.probe		= cgbc_i2c_probe,
	.remove		= cgbc_i2c_remove,
};

module_platform_driver(cgbc_i2c_driver);

MODULE_DESCRIPTION("Congatec Board Controller I2C Driver");
MODULE_AUTHOR("Thomas Richard <thomas.richard@bootlin.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:cgbc_i2c");
