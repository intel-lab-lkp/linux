// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C driver for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/string.h>
#include <linux/uio.h>

#include <linux/usb/lstp.h>

enum lstp_i2c_cmd {
	LSTP_I2C_CMD_BUS_RECOVERY = 0x00,
	LSTP_I2C_CMD_READ = 0x01,
	LSTP_I2C_CMD_WRITE = 0x02,
	LSTP_I2C_CMD_WRITE_READ = 0x04,
	LSTP_I2C_CMD_SMBUS_BLOCK_READ = 0x05
} __packed;

enum lstp_i2c_cmd_flags { LSTP_I2C_CMD_FLAG_NO_STOP = 0x40 };

enum lstp_i2c_smbus_block_read_flags {
	LSTP_I2C_SMBUS_BLOCK_READ_FLAG_PEC = BIT(0),
} __packed;

struct lstp_i2c_read_req {
	u8 addr;
	__le16 rd_len;
} __packed;

struct lstp_i2c_write_req {
	u8 addr;
	u8 wr_data[];
} __packed;

struct lstp_i2c_wr_rd_req {
	u8 addr;
	__le16 rd_len;
	u8 wr_data[]; /* Write length inferred from length in header */
} __packed;

struct lstp_i2c_smbus_block_read_req {
	u8 addr;
	__le16 flags; /* lstp_i2c_smbus_block_read_flags */
	u8 wr_data[]; /* Write length inferred from length in header */
} __packed;

/* Largest block-read write phase: SMBus command byte, block count, maximum-size block. */
#define LSTP_I2C_SMBUS_BLOCK_READ_WR_MAX (2 + I2C_SMBUS_BLOCK_MAX)

enum lstp_i2c_speed {
	LSTP_I2C_SPEED_100KHZ = 0x00,
	LSTP_I2C_SPEED_400KHZ = 0x01,
	LSTP_I2C_SPEED_1000KHZ = 0x02,
	LSTP_I2C_SPEED_3400KHZ = 0x03,
};

struct lstp_i2c_config {
	u8 speed;
	__le16 output_drive_strength; /* in mA */
	__le16 slew_rate; /* in 100 ps (0.1 ns) increments */
	__le16 input_debounce_time; /* in us */
	__le16 glitch_filter_width; /* in ns */
	u8 bus_low_timeout; /* in ms */
	u8 bus_arbitration_retries;
} __packed;

/* Wire offsets: inserting or resizing a field silently shifts every field after it. */
static_assert(sizeof(struct lstp_i2c_config) == 11,
	      "I2C channel config must match the LSTP specification");

/* Shortest blob the channel accepts; every field after speed is optional. */
#define LSTP_I2C_CONFIG_MIN_LEN offsetofend(struct lstp_i2c_config, speed)

static bool lstp_i2c_can_retry(int lstp_status)
{
	return lstp_status == LSTP_BUSY || lstp_status == LSTP_ARB_LOST;
}

static bool lstp_i2c_suppress_error_logs(int ret, int lstp_status)
{
	return ret == -ENODEV || lstp_status == LSTP_NACK || lstp_i2c_can_retry(lstp_status);
}

/* i2c_bus_recovery_info.recover_bus: ask the device to run its own bus recovery. */
static int lstp_i2c_bus_recovery(struct i2c_adapter *adap)
{
	struct lstp_channel *ch = adap->algo_data;
	int lstp_status = LSTP_NO_RESPONSE_STATUS;

	int ret = lstp_request(ch, LSTP_I2C_CMD_BUS_RECOVERY, NULL, 0, NULL, 0, NULL, &lstp_status);

	if (ret && !lstp_i2c_suppress_error_logs(ret, lstp_status))
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Bus recovery failed (%pe)\n", __func__,
				    ch->ch_id, ERR_PTR(ret));
	if (ret && lstp_i2c_can_retry(lstp_status))
		return -EAGAIN;
	return ret;
}

static int lstp_i2c_validate_msg(struct i2c_msg *msg)
{
	if (!msg->buf && msg->len > 0)
		return -EINVAL;

	if (msg->flags & I2C_M_TEN)
		return -EAFNOSUPPORT; /* 10-bit address not supported */

	if (msg->addr > 0x7F)
		return -EINVAL; /* Only 7-bit address supported */

	if ((msg->flags & I2C_M_RECV_LEN) && (!(msg->flags & I2C_M_RD) || msg->len < 1))
		return -EINVAL;

	return 0;
}

/* One READ transaction; @no_stop omits STOP so a further message can follow. */
static int lstp_i2c_read(struct i2c_adapter *adap, struct i2c_msg *msg, bool no_stop)
{
	u8 cmd = LSTP_I2C_CMD_READ;
	struct lstp_channel *ch = adap->algo_data;
	struct lstp_i2c_read_req req = {
		.addr = msg->addr,
		.rd_len = cpu_to_le16(msg->len),
	};
	int lstp_status = LSTP_NO_RESPONSE_STATUS;

	int ret = lstp_i2c_validate_msg(msg);

	if (ret) {
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Invalid message to addr=0x%02x (%pe)\n",
				    __func__, ch->ch_id, msg->addr, ERR_PTR(ret));
		return ret;
	}

	if (msg->len > ch->max_rx_payload) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Read request message to addr=0x%02x too long (%u bytes, max %zu)\n",
				    __func__, ch->ch_id, msg->addr, msg->len, ch->max_rx_payload);
		return -EINVAL;
	}

	dev_dbg(ch->dev, "%s: ch_%d: addr=0x%02x, len=%d\n", __func__, ch->ch_id, msg->addr,
		msg->len);

	if (no_stop)
		cmd |= LSTP_I2C_CMD_FLAG_NO_STOP;

	ret = lstp_request(ch, cmd, &req, sizeof(req), msg->buf, msg->len, NULL, &lstp_status);
	if (ret && !lstp_i2c_suppress_error_logs(ret, lstp_status))
		dev_err_ratelimited(ch->dev, "%s: ch_%d: READ to addr=0x%02x failed (%pe)\n",
				    __func__, ch->ch_id, msg->addr, ERR_PTR(ret));
	if (ret && lstp_i2c_can_retry(lstp_status))
		return -EAGAIN;
	return ret;
}

/* One WRITE transaction; @no_stop omits STOP so a further message can follow. */
static int lstp_i2c_write(struct i2c_adapter *adap, struct i2c_msg *msg, bool no_stop)
{
	u8 cmd = LSTP_I2C_CMD_WRITE;
	struct lstp_channel *ch = adap->algo_data;
	struct lstp_i2c_write_req req_head = { .addr = msg->addr };
	struct kvec vecs[2] = {
		{ .iov_base = &req_head, .iov_len = sizeof(req_head) },
		{ .iov_base = msg->buf, .iov_len = msg->len },
	};
	int lstp_status = LSTP_NO_RESPONSE_STATUS;

	int ret = lstp_i2c_validate_msg(msg);

	if (ret) {
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Invalid message to addr=0x%02x (%pe)\n",
				    __func__, ch->ch_id, msg->addr, ERR_PTR(ret));
		return ret;
	}

	if (sizeof(req_head) + msg->len > ch->max_tx_payload) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Write message to addr=0x%02x too long (%u bytes, max %zu)\n",
				    __func__, ch->ch_id, msg->addr, msg->len,
				    ch->max_tx_payload - sizeof(req_head));
		return -EINVAL;
	}

	if (msg->len > 0)
		dev_dbg(ch->dev, "%s: ch_%d: addr=0x%02x, len=%d, data=0x%*ph\n", __func__,
			ch->ch_id, msg->addr, msg->len, msg->len, msg->buf);
	else
		dev_dbg(ch->dev, "%s: ch_%d: addr=0x%02x, len=%d\n", __func__, ch->ch_id, msg->addr,
			msg->len);

	if (no_stop)
		cmd |= LSTP_I2C_CMD_FLAG_NO_STOP;

	ret = lstp_requestv(ch, cmd, vecs, ARRAY_SIZE(vecs), NULL, 0, NULL, &lstp_status);
	if (ret && !lstp_i2c_suppress_error_logs(ret, lstp_status))
		dev_err_ratelimited(ch->dev, "%s: ch_%d: WRITE to addr=0x%02x failed (%pe)\n",
				    __func__, ch->ch_id, msg->addr, ERR_PTR(ret));
	if (ret && lstp_i2c_can_retry(lstp_status))
		return -EAGAIN;
	return ret;
}

/* Write then read at one address, separated by a repeated START, as a single request. */
static int lstp_i2c_write_read(struct i2c_adapter *adap, struct i2c_msg *wr_msg,
			       struct i2c_msg *rd_msg, bool no_stop)
{
	u8 cmd = LSTP_I2C_CMD_WRITE_READ;
	struct lstp_channel *ch = adap->algo_data;
	struct lstp_i2c_wr_rd_req req_head = {
		.addr = wr_msg->addr,
		.rd_len = cpu_to_le16(rd_msg->len),
	};
	struct kvec vecs[2] = {
		{ .iov_base = &req_head, .iov_len = sizeof(req_head) },
		{ .iov_base = wr_msg->buf, .iov_len = wr_msg->len },
	};
	int lstp_status = LSTP_NO_RESPONSE_STATUS;

	int ret = lstp_i2c_validate_msg(wr_msg);

	if (ret) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Invalid write message to addr=0x%02x (%pe)\n",
				    __func__, ch->ch_id, wr_msg->addr, ERR_PTR(ret));
		return ret;
	}

	if (sizeof(req_head) + wr_msg->len > ch->max_tx_payload) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Write message to addr=0x%02x too long (%u bytes, max %zu)\n",
				    __func__, ch->ch_id, wr_msg->addr, wr_msg->len,
				    ch->max_tx_payload - sizeof(req_head));
		return -EINVAL;
	}

	ret = lstp_i2c_validate_msg(rd_msg);
	if (ret) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Invalid read message to addr=0x%02x (%pe)\n",
				    __func__, ch->ch_id, rd_msg->addr, ERR_PTR(ret));
		return ret;
	}

	if (rd_msg->len > ch->max_rx_payload) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Read message to addr=0x%02x too long (%u bytes, max %zu)\n",
				    __func__, ch->ch_id, rd_msg->addr, rd_msg->len,
				    ch->max_rx_payload);
		return -EINVAL;
	}

	if (wr_msg->addr != rd_msg->addr) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Write/read addr mismatch (wr=0x%02x, rd=0x%02x)\n",
				    __func__, ch->ch_id, wr_msg->addr, rd_msg->addr);
		return -EINVAL;
	}

	if (no_stop)
		cmd |= LSTP_I2C_CMD_FLAG_NO_STOP;

	ret = lstp_requestv(ch, cmd, vecs, ARRAY_SIZE(vecs), rd_msg->buf, rd_msg->len, NULL,
			    &lstp_status);
	if (ret && !lstp_i2c_suppress_error_logs(ret, lstp_status))
		dev_err_ratelimited(ch->dev, "%s: ch_%d: WRITE_READ to addr=0x%02x failed (%pe)\n",
				    __func__, ch->ch_id, wr_msg->addr, ERR_PTR(ret));
	if (ret && lstp_i2c_can_retry(lstp_status))
		return -EAGAIN;
	return ret;
}

/*
 * SMBus block read with an optional write phase, which must address the same target.
 * @rd_msg->len is updated to the bytes received.
 */
static int lstp_i2c_smbus_block_read(struct i2c_adapter *adap, struct i2c_msg *wr_msg,
				     struct i2c_msg *rd_msg, bool no_stop)
{
	u8 cmd = LSTP_I2C_CMD_SMBUS_BLOCK_READ;
	struct lstp_channel *ch = adap->algo_data;
	DEFINE_RAW_FLEX(struct lstp_i2c_smbus_block_read_req, req, wr_data,
			LSTP_I2C_SMBUS_BLOCK_READ_WR_MAX);
	u16 wr_len = wr_msg ? wr_msg->len : 0;
	u8 *wr_buf = wr_msg ? wr_msg->buf : NULL;
	bool pec = rd_msg->flags & I2C_CLIENT_PEC;
	/* The target frames the block with a count byte, plus a trailing PEC byte if asked for. */
	u8 framing = pec ? 2 : 1;
	u16 rx_buf_len = I2C_SMBUS_BLOCK_MAX + framing;
	size_t pkt_len = 0;
	int lstp_status = LSTP_NO_RESPONSE_STATUS;

	/* Probe enforces bulk_tx_size >= LSTP_USB_EP_MIN_SIZE; anchor the largest write phase. */
	BUILD_BUG_ON(sizeof(struct lstp_header) +
			     struct_size(req, wr_data, LSTP_I2C_SMBUS_BLOCK_READ_WR_MAX) >
		     LSTP_USB_EP_MIN_SIZE);

	int ret = lstp_i2c_validate_msg(rd_msg);

	if (ret) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Invalid read message to addr=0x%02x (%pe)\n",
				    __func__, ch->ch_id, rd_msg->addr, ERR_PTR(ret));
		return ret;
	}

	/*
	 * An I2C_M_RECV_LEN read reserves rd_msg->len bytes on top of I2C_SMBUS_BLOCK_MAX, so the
	 * framing has to fit in there for a maximum-size block to still land inside the buffer.
	 */
	if (rd_msg->len < framing) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Read from addr=0x%02x reserves %u bytes, needs %u for framing\n",
				    __func__, ch->ch_id, rd_msg->addr, rd_msg->len, framing);
		return -EINVAL;
	}

	if (wr_msg) {
		ret = lstp_i2c_validate_msg(wr_msg);
		if (ret) {
			dev_err_ratelimited(ch->dev,
					    "%s: ch_%d: Invalid write message to addr=0x%02x (%pe)\n",
					    __func__, ch->ch_id, wr_msg->addr, ERR_PTR(ret));
			return ret;
		}

		if (wr_msg->addr != rd_msg->addr) {
			dev_err_ratelimited(ch->dev,
					    "%s: ch_%d: Write/read addr mismatch (wr=0x%02x, rd=0x%02x)\n",
					    __func__, ch->ch_id, wr_msg->addr, rd_msg->addr);
			return -EINVAL;
		}
	}

	if (wr_len > LSTP_I2C_SMBUS_BLOCK_READ_WR_MAX) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: SMBus write phase to addr=0x%02x too long (%u bytes, max %u)\n",
				    __func__, ch->ch_id, rd_msg->addr, wr_len,
				    LSTP_I2C_SMBUS_BLOCK_READ_WR_MAX);
		return -EINVAL;
	}

	if (rx_buf_len > ch->max_rx_payload) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: Read message to addr=0x%02x too long (%u bytes, max %zu)\n",
				    __func__, ch->ch_id, rd_msg->addr, rx_buf_len,
				    ch->max_rx_payload);
		return -EINVAL;
	}

	*req = (struct lstp_i2c_smbus_block_read_req){
		.addr = rd_msg->addr,
		.flags = cpu_to_le16(pec ? LSTP_I2C_SMBUS_BLOCK_READ_FLAG_PEC : 0),
	};
	if (wr_len)
		memcpy(req->wr_data, wr_buf, wr_len);

	if (no_stop)
		cmd |= LSTP_I2C_CMD_FLAG_NO_STOP;

	ret = lstp_request(ch, cmd, req, struct_size(req, wr_data, wr_len), rd_msg->buf, rx_buf_len,
			   &pkt_len, &lstp_status);
	if (ret) {
		if (!lstp_i2c_suppress_error_logs(ret, lstp_status))
			dev_err_ratelimited(ch->dev,
					    "%s: ch_%d: SMBUS_BLOCK_READ to addr=0x%02x failed (%pe)\n",
					    __func__, ch->ch_id, rd_msg->addr, ERR_PTR(ret));
		if (lstp_i2c_can_retry(lstp_status))
			return -EAGAIN;
		return ret;
	}

	if (pkt_len == 0) {
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Empty response packet\n", __func__,
				    ch->ch_id);
		return -EIO;
	}

	u8 block_len = rd_msg->buf[0];

	if (block_len > I2C_SMBUS_BLOCK_MAX) {
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Invalid block length %u\n", __func__,
				    ch->ch_id, block_len);
		return -EPROTO;
	}

	u16 rx_len = block_len + framing;

	if (pkt_len < rx_len) {
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Invalid response length (%zu)\n", __func__,
				    ch->ch_id, pkt_len);
		return -EIO;
	}

	rd_msg->len = rx_len;
	return 0;
}

static bool lstp_i2c_is_combined_pair(const struct i2c_msg *wr_msg, const struct i2c_msg *rd_msg)
{
	return !(wr_msg->flags & I2C_M_RD) && (rd_msg->flags & I2C_M_RD) &&
	       wr_msg->addr == rd_msg->addr;
}

/*
 * i2c_algorithm.xfer: one device request per message, except a write/read pair to the same
 * target, which folds into one combined request. Every request but the last omits STOP, so
 * the whole message array stays a single bus transaction.
 */
static int lstp_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct lstp_channel *ch = adap->algo_data;

	if (!msgs || num <= 0)
		return -EINVAL;

	dev_dbg(ch->dev, "%s: ch_%d: I2C transfer with %d message(s)\n", __func__, ch->ch_id, num);

	for (int i = 0; i < num; i++) {
		struct i2c_msg *wr_msg = NULL;
		struct i2c_msg *msg = &msgs[i];

		if (i + 1 < num && lstp_i2c_is_combined_pair(msg, &msgs[i + 1])) {
			wr_msg = msg;
			msg = &msgs[++i];
		}

		bool recv_len = msg->flags & I2C_M_RECV_LEN;
		bool read = msg->flags & I2C_M_RD;
		bool no_stop = i != num - 1;
		int ret;

		if (!read)
			ret = lstp_i2c_write(adap, msg, no_stop);
		else if (read && recv_len)
			ret = lstp_i2c_smbus_block_read(adap, wr_msg, msg, no_stop);
		else if (read && !recv_len && wr_msg)
			ret = lstp_i2c_write_read(adap, wr_msg, msg, no_stop);
		else
			ret = lstp_i2c_read(adap, msg, no_stop);

		if (ret)
			return ret;
	}

	return num;
}

static u32 lstp_i2c_functionality(struct i2c_adapter *adap)
{
	/* TODO: This needs to be discoverable, also need separate commands for 10-bit address */
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL_ALL;
}

static struct i2c_bus_recovery_info lstp_i2c_recovery_info = {
	.recover_bus = lstp_i2c_bus_recovery,
};

static const struct i2c_algorithm lstp_i2c_algorithm = {
	.xfer = lstp_i2c_xfer,
	.functionality = lstp_i2c_functionality,
};

#define LSTP_I2C_DEFAULT_RETRIES 3

/*
 * The I2C core re-issues only an -EAGAIN transfer and bounds the whole retry loop, not each
 * attempt, by adap->timeout, so the timeout spans every attempt. A device reporting
 * bus_arbitration_retries retries LSTP_BUSY and LSTP_ARB_LOST itself, so the host does not
 * and ignores "nvidia,retries". @adap's firmware node must already be set.
 */
static void lstp_i2c_configure_retry_policy(struct lstp_channel *ch, struct i2c_adapter *adap,
					    const struct lstp_i2c_config *cfg)
{
	u32 retries = LSTP_I2C_DEFAULT_RETRIES;
	u32 configured_retries;
	bool have_configured;

	have_configured =
		!device_property_read_u32(&adap->dev, "nvidia,retries", &configured_retries);

	if (cfg->bus_arbitration_retries) {
		if (have_configured && configured_retries)
			dev_warn(ch->dev,
				 "%s: ch_%d: Ignoring retries=%u; device retries the bus %u time(s) itself\n",
				 __func__, ch->ch_id, configured_retries,
				 cfg->bus_arbitration_retries);
		retries = 0;
	} else if (have_configured) {
		if (configured_retries <= U8_MAX)
			retries = configured_retries;
		else
			dev_warn(ch->dev, "%s: ch_%d: Ignoring retries value %u; maximum is %u\n",
				 __func__, ch->ch_id, configured_retries, U8_MAX);
	}

	adap->retries = retries;
	adap->timeout = msecs_to_jiffies((retries + 1) * LSTP_USB_RESPONSE_TIMEOUT_MS);
}

/* The I2C core takes the bus number from an "i2c" alias on adap->dev's firmware node. */
static int lstp_i2c_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(auxdev);
	const struct lstp_channel_init_info *info = dev_get_platdata(&auxdev->dev);

	if (!info)
		return -ENODEV;

	if (info->config_len < LSTP_I2C_CONFIG_MIN_LEN) {
		dev_err(ch->dev, "%s: ch_%d: I2C config too short: %zu < %zu\n", __func__,
			ch->ch_id, info->config_len, LSTP_I2C_CONFIG_MIN_LEN);
		return -EINVAL;
	}

	/* Full-size copy, so field reads need no length checks: unreported fields read as zero. */
	struct lstp_i2c_config cfg;

	memcpy_and_pad(&cfg, sizeof(cfg), info->config, info->config_len, 0);

	struct i2c_adapter *adap = devm_kzalloc(ch->dev, sizeof(*adap), GFP_KERNEL);

	if (!adap)
		return -ENOMEM;

	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_HWMON;
	adap->algo = &lstp_i2c_algorithm;
	adap->algo_data = ch;
	adap->dev.parent = ch->dev;
	adap->bus_recovery_info = &lstp_i2c_recovery_info;
	device_set_node(&adap->dev, ch->fwnode);
	lstp_i2c_configure_retry_policy(ch, adap, &cfg);
	strscpy(adap->name, ch->display_name, sizeof(adap->name));
	/* TODO: save i2c speed from config */

	int ret = devm_i2c_add_adapter(ch->dev, adap);

	if (ret) {
		dev_err(ch->dev, "%s: ch_%d: Could not register I2C adapter (%pe)\n", __func__,
			ch->ch_id, ERR_PTR(ret));
		return ret;
	}

	dev_info(ch->dev, "%s: ch_%d: Started as %s\n", __func__, ch->ch_id, adap->name);
	return 0;
}

static const struct auxiliary_device_id lstp_i2c_id_table[] = { { .name = "lstp.i2c" }, {} };
MODULE_DEVICE_TABLE(auxiliary, lstp_i2c_id_table);

static struct lstp_auxiliary_driver lstp_i2c_drv = {
	.adrv = {
		.name = "i2c",
		.probe = lstp_i2c_probe,
		.id_table = lstp_i2c_id_table,
	},
};
module_lstp_auxiliary_driver(lstp_i2c_drv);

MODULE_IMPORT_NS("LSTP");
MODULE_DESCRIPTION("LSTP I2C driver");
MODULE_LICENSE("GPL");
