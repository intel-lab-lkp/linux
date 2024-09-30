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

static void dp_aux_reset(const struct hibmc_dp_aux *aux)
{
	dp_write_bits(aux->addr + DP_DPTX_RST_CTRL, DP_CFG_AUX_RST_N, 0x0);
	usleep_range(10, 15);
	dp_write_bits(aux->addr + DP_DPTX_RST_CTRL, DP_CFG_AUX_RST_N, 0x1);
}

static void dp_aux_read_data(struct hibmc_dp_aux *aux, u8 *buf, u8 size)
{
	u32 reg_num;
	u32 value;
	u32 num;
	u8 i, j;

	reg_num = round_up(size, AUX_4_BYTE) / AUX_4_BYTE;
	for (i = 0; i < reg_num; i++) {
		/* number of bytes read from a single register */
		num = min(size - i * AUX_4_BYTE, AUX_4_BYTE);
		value = readl(aux->addr + DP_AUX_RD_DATA0 + i * AUX_4_BYTE);
		/* convert the 32-bit value of the register to the buffer. */
		for (j = 0; j < num; j++)
			buf[i * AUX_4_BYTE + j] = value >> (j * AUX_8_BIT);
	}
}

static void dp_aux_write_data(struct hibmc_dp_aux *aux, u8 *buf, u8 size)
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
		writel(value, aux->addr + DP_AUX_WR_DATA0 + i * AUX_4_BYTE);
	}
}

static u32 dp_aux_build_cmd(const struct hibmc_dp_aux_msg *msg)
{
	u32 aux_cmd = msg->request << AUX_CMD_REQ_TYPE_S;

	if (msg->size)
		aux_cmd |= (msg->size - 1) << AUX_CMD_REQ_LEN_S;
	else
		aux_cmd |= 1 << AUX_CMD_I2C_ADDR_ONLY_S;

	aux_cmd |= msg->address << AUX_CMD_ADDR_S;

	return aux_cmd;
}

/* ret >= 0 ,ret is size; ret < 0, ret is err code */
static int dp_aux_parse_xfer(struct hibmc_dp_aux *aux, struct hibmc_dp_aux_msg *msg)
{
	u32 buf_data_cnt;
	u32 aux_status;
	int ret = 0;

	aux_status = readl(aux->addr + DP_AUX_STATUS);
	msg->reply = (aux_status & DP_CFG_AUX_STATUS) >> DP_CFG_AUX_STATUS_S;

	if (aux_status & DP_CFG_AUX_TIMEOUT)
		return -ETIMEDOUT;

	/* only address */
	if (!msg->size)
		return 0;

	if (msg->reply != DP_AUX_ACK)
		return 0;

	buf_data_cnt = (aux_status & DP_CFG_AUX_READY_DATA_BYTE) >> AUX_READY_DATA_BYTE_S;

	switch (msg->request) {
	case DP_NATIVE_W:
		ret = msg->size;
		break;
	case DP_I2C_MOT_W:
		if (buf_data_cnt == AUX_I2C_WRITE_SUCCESS)
			ret = msg->size;
		else if (buf_data_cnt == AUX_I2C_WRITE_PARTIAL_SUCCESS)
			ret = (aux_status & DP_CFG_AUX) >> DP_CFG_AUX_S;
		break;
	case DP_NATIVE_R:
	case DP_I2C_MOT_R:
		buf_data_cnt--;
		/* only the successful part of data is read */
		if (buf_data_cnt != msg->size) {
			ret = -EBUSY;
		} else { /* all data is successfully read */
			dp_aux_read_data(aux, msg->buf, msg->size);
			ret = msg->size;
		}
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

/* ret >= 0 ,ret is size; ret < 0, ret is err code */
static int dp_aux_xfer(struct hibmc_dp_aux *aux, struct hibmc_dp_aux_msg *msg)
{
	u32 aux_cmd;
	int ret;
	u32 val; /* val will be assigned at the beginning of readl_poll_timeout function */

	aux_cmd = dp_aux_build_cmd(msg);
	writel(aux_cmd, aux->addr + DP_AUX_CMD_ADDR);

	/* enable aux transfer */
	dp_write_bits(aux->addr + DP_AUX_REQ, DP_CFG_AUX_REQ, 0x1);
	ret = readl_poll_timeout(aux->addr + DP_AUX_REQ, val, !(val & DP_CFG_AUX_REQ), 50, 5000);
	if (ret) {
		dp_aux_reset(aux);
		return ret;
	}

	return dp_aux_parse_xfer(aux, msg);
}

/* ret >= 0 ,ret is size; ret < 0, ret is err code */
static int dp_aux_rw(struct hibmc_dp_aux *aux, u32 address, u8 *buffer, u8 request, u8 size)
{
	struct hibmc_dp_aux_msg msg;
	u32 retry;
	int ret;

	msg.address = address;
	msg.request = request;
	msg.buf = buffer;
	msg.size = size;

	mutex_lock(&aux->lock);

	writel(0, aux->addr + DP_AUX_WR_DATA0);
	writel(0, aux->addr + DP_AUX_WR_DATA1);
	writel(0, aux->addr + DP_AUX_WR_DATA2);
	writel(0, aux->addr + DP_AUX_WR_DATA3);

	dp_aux_write_data(aux, buffer, size);

	for (retry = 0; retry < AUX_RW_MAX_RETRY; retry++) {
		ret = dp_aux_xfer(aux, &msg);
		if (ret < 0) {
			if (ret == -EBUSY) {
				usleep_range(450, 500);
				continue;
			} else if (ret == -ETIMEDOUT) {
				continue;
			} else {
				goto exit;
			}
		}
		switch (msg.reply & DP_AUX_NATIVE_REPLY_MASK) {
		case DP_AUX_ACK:
			goto exit;
		case DP_AUX_NACK:
		case DP_AUX_DEFER:
			usleep_range(450, 500);
			continue;
		default:
			ret = -EINVAL;
			goto exit;
		}
	}

exit:
	mutex_unlock(&aux->lock);

	return ret;
}

int dp_aux_write(struct hibmc_dp_dev *dp, u32 address, u8 *buffer, u8 size)
{
	int ret;

	ret = dp_aux_rw(&dp->aux, address, buffer, DP_NATIVE_W, size);
	if (ret != size) {
		drm_err(dp->dev, "dp aux dpcd write failed, address:0x%x, size:%u, ret:%d!\n",
			address, size, ret);
		if (ret < 0)
			return ret;
		else
			return -EFAULT;
	}

	return 0;
}

int dp_aux_read(struct hibmc_dp_dev *dp, u32 address, u8 *buffer, u8 size)
{
	int ret;

	ret = dp_aux_rw(&dp->aux, address, buffer, DP_NATIVE_R, size);
	if (ret != size) {
		drm_err(dp->dev, "dp aux dpcd read failed, address:0x%x, size:%u, ret:%d!\n",
			address, size, ret);
		if (ret < 0)
			return ret;
		else
			return -EFAULT;
	}

	return 0;
}
