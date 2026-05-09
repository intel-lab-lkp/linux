// SPDX-License-Identifier: GPL-2.0
/*
 * NXP HMS (Heterogeneous Multi-SoC) DSA Switch SPI Transport Layer
 *
 * Copyright 2025-2026 NXP
 */

#include <linux/spi/spi.h>
#include "hms_switch.h"

int hms_xfer_cmd(struct hms_private *priv,
		 enum hms_spi_rw_mode rw, enum hms_cmd cmd,
		 void *param, size_t param_len,
		 void *resp, size_t resp_len,
		 struct ptp_system_timestamp *ptp_sts)
{
	struct hms_cmd_hdr hdr = {0};
	struct spi_device *spi = priv->spidev;
	struct spi_transfer hdr_xfer, resp_xfer;
	int rc;

	if (!IS_ALIGNED(resp_len, HMS_SPI_MSG_WORD_BYTES)) {
		dev_err(&spi->dev, "hms cmd %d data size should be a multiple of 4: %zu\n",
			cmd, resp_len);
		return -EINVAL;
	}

	if (resp_len > priv->max_xfer_len) {
		dev_err(&spi->dev, "hms cmd %d data size is too large\n",
			cmd);
		return -EINVAL;
	}

	if (param_len > HMS_SPI_MSG_PARAM_SIZE) {
		dev_err(&spi->dev, "hms cmd %d param size is too large\n",
			cmd);
		return -EINVAL;
	}

	hdr.cmd = (rw << HMS_CMD_DIR_SHIFT) |
		  ((resp_len / HMS_SPI_MSG_WORD_BYTES) << HMS_CMD_LEN_SHIFT) |
		  cmd;
	if (param)
		memcpy(hdr.param, param, param_len);

	memset(&hdr_xfer, 0, sizeof(hdr_xfer));
	hdr_xfer.tx_buf = &hdr;
	hdr_xfer.len = HMS_SPI_MSG_HEADER_SIZE;
	hdr_xfer.ptp_sts_word_pre = hdr_xfer.len - 1;
	hdr_xfer.ptp_sts_word_post = hdr_xfer.len - 1;
	hdr_xfer.ptp_sts = ptp_sts;

	mutex_lock(&priv->spi_lock);

	rc = spi_sync_transfer(spi, &hdr_xfer, 1);
	if (rc < 0) {
		dev_err(&spi->dev, "hms cmd %d SPI transfer failed: %d\n",
			cmd, rc);
		mutex_unlock(&priv->spi_lock);
		return rc;
	}

	usleep_range(HMS_SPI_MSG_RESPONSE_TIME,
		     HMS_SPI_MSG_RESPONSE_TIME + 100);

	if (!resp) {
		mutex_unlock(&priv->spi_lock);
		return 0;
	}

	/* Populate the transfer's data buffer */
	memset(&resp_xfer, 0, sizeof(resp_xfer));
	if (rw == SPI_READ)
		resp_xfer.rx_buf = resp;
	else
		resp_xfer.tx_buf = resp;
	resp_xfer.len = resp_len;

	resp_xfer.ptp_sts_word_pre = resp_xfer.len - 1;
	resp_xfer.ptp_sts_word_post = resp_xfer.len - 1;
	resp_xfer.ptp_sts = ptp_sts;

	rc = spi_sync_transfer(spi, &resp_xfer, 1);

	mutex_unlock(&priv->spi_lock);

	if (rc < 0) {
		dev_err(&spi->dev, "hms cmd %d SPI transfer failed: %d\n",
			cmd, rc);
		return rc;
	}

	return 0;
}

int hms_xfer_set_cmd(struct hms_private *priv,
		     enum hms_cmd cmd,
		     void *param, size_t param_len)
{
	return hms_xfer_cmd(priv, SPI_WRITE, cmd,
			    param, param_len,
			    NULL, 0, NULL);
}

int hms_xfer_get_cmd(struct hms_private *priv,
		     enum hms_cmd cmd, u32 id,
		     void *resp, size_t resp_len)
{
	struct hms_cmd_read_param param;

	param.id = id;

	return hms_xfer_cmd(priv, SPI_READ, cmd,
			    &param, sizeof(param),
			    resp, resp_len, NULL);
}
