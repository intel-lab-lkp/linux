// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/minmax.h>
#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include "dp_comm.h"
#include "dp_reg.h"
#include "dp_aux.h"

#define DP_MIN_PULSE_NUM 0x9

static void dp_aux_reset(const struct dp_dev *dp)
{
	dp_write_bits(dp->base + DP_DPTX_RST_CTRL, DP_CFG_AUX_RST_N, 0x0);
	usleep_range(10, 15);
	dp_write_bits(dp->base + DP_DPTX_RST_CTRL, DP_CFG_AUX_RST_N, 0x1);
}

static void dp_aux_read_data(struct dp_dev *dp, u8 *buf, u8 size)
{
	u32 reg_num;
	u32 value;
	u32 num;
	u8 i, j;

	reg_num = round_up(size, AUX_4_BYTE) / AUX_4_BYTE;
	for (i = 0; i < reg_num; i++) {
		/* number of bytes read from a single register */
		num = min(size - i * AUX_4_BYTE, AUX_4_BYTE);
		value = readl(dp->base + DP_AUX_RD_DATA0 + i * AUX_4_BYTE);
		/* convert the 32-bit value of the register to the buffer. */
		for (j = 0; j < num; j++)
			buf[i * AUX_4_BYTE + j] = value >> (j * AUX_8_BIT);
	}
}

static void dp_aux_write_data(struct dp_dev *dp, u8 *buf, u8 size)
{
	u32 reg_num;
	u32 value;
	u8 i, j;
	u32 num;

	reg_num = round_up(size, AUX_4_BYTE) / AUX_4_BYTE;
	for (i = 0; i < reg_num; i++) {
		/* number of bytes written to a single register */
		num = min_t(u8, size - i * AUX_4_BYTE, AUX_4_BYTE);
		value = 0;
		/* obtain the 32-bit value written to a single register. */
		for (j = 0; j < num; j++)
			value |= buf[i * AUX_4_BYTE + j] << (j * AUX_8_BIT);
		/* writing data to a single register */
		writel(value, dp->base + DP_AUX_WR_DATA0 + i * AUX_4_BYTE);
	}
}

static u32 dp_aux_build_cmd(const struct drm_dp_aux_msg *msg)
{
	u32 aux_cmd = msg->request;

	if (msg->size)
		aux_cmd |= (msg->size - 1) << AUX_CMD_REQ_LEN_S;
	else
		aux_cmd |= 1 << AUX_CMD_I2C_ADDR_ONLY_S;

	aux_cmd |= msg->address << AUX_CMD_ADDR_S;

	return aux_cmd;
}

/* ret >= 0 ,ret is size; ret < 0, ret is err code */
static int dp_aux_parse_xfer(struct dp_dev *dp, struct drm_dp_aux_msg *msg)
{
	u32 buf_data_cnt;
	u32 aux_status;
	int ret = 0;

	aux_status = readl(dp->base + DP_AUX_STATUS);
	msg->reply = FIELD_GET(DP_CFG_AUX_STATUS, aux_status);

	if (aux_status & DP_CFG_AUX_TIMEOUT)
		return -ETIMEDOUT;

	/* only address */
	if (!msg->size)
		return 0;

	if (msg->reply != DP_AUX_NATIVE_REPLY_ACK)
		return 0;

	buf_data_cnt = FIELD_GET(DP_CFG_AUX_READY_DATA_BYTE, aux_status);

	switch (msg->request) {
	case DP_AUX_NATIVE_WRITE:
		ret = msg->size;
		break;
	case DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT:
		if (buf_data_cnt == AUX_I2C_WRITE_SUCCESS)
			ret = msg->size;
		else if (buf_data_cnt == AUX_I2C_WRITE_PARTIAL_SUCCESS)
			ret = FIELD_GET(DP_CFG_AUX, aux_status);
		break;
	case DP_AUX_NATIVE_READ:
	case DP_AUX_I2C_READ | DP_AUX_I2C_MOT:
		buf_data_cnt--;
		/* only the successful part of data is read */
		if (buf_data_cnt != msg->size) {
			ret = -EBUSY;
		} else { /* all data is successfully read */
			dp_aux_read_data(dp, msg->buffer, msg->size);
			ret = msg->size;
		}
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

/* ret >= 0 ,ret is size; ret < 0, ret is err code */
static ssize_t dp_aux_xfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	struct dp_dev *dp = container_of(aux, struct dp_dev, aux);
	u32 aux_cmd;
	int ret;
	u32 val; /* val will be assigned at the beginning of readl_poll_timeout function */

	writel(0, dp->base + DP_AUX_WR_DATA0);
	writel(0, dp->base + DP_AUX_WR_DATA1);
	writel(0, dp->base + DP_AUX_WR_DATA2);
	writel(0, dp->base + DP_AUX_WR_DATA3);

	dp_aux_write_data(dp, msg->buffer, msg->size);

	aux_cmd = dp_aux_build_cmd(msg);
	writel(aux_cmd, dp->base + DP_AUX_CMD_ADDR);

	/* enable aux transfer */
	dp_write_bits(dp->base + DP_AUX_REQ, DP_CFG_AUX_REQ, 0x1);
	ret = readl_poll_timeout(dp->base + DP_AUX_REQ, val, !(val & DP_CFG_AUX_REQ), 50, 5000);
	if (ret) {
		dp_aux_reset(dp);
		return ret;
	}

	return dp_aux_parse_xfer(dp, msg);
}

void dp_aux_init(struct dp_dev *dp)
{
	dp_write_bits(dp->base + DP_AUX_REQ, DP_CFG_AUX_SYNC_LEN_SEL, 0x0);
	dp_write_bits(dp->base + DP_AUX_REQ, DP_CFG_AUX_TIMER_TIMEOUT, 0x1);
	dp_write_bits(dp->base + DP_AUX_REQ, DP_CFG_AUX_MIN_PULSE_NUM, DP_MIN_PULSE_NUM);

	dp->aux.transfer = dp_aux_xfer;
	dp->aux.is_remote = 0;
	drm_dp_aux_init(&dp->aux);
}
