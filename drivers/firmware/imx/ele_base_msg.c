// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2024 NXP
 */

#include <linux/types.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>

#include "ele_base_msg.h"
#include "ele_common.h"

int ele_get_info(struct device *dev, struct soc_info *s_info)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct se_api_msg *tx_msg __free(kfree);
	struct se_api_msg *rx_msg __free(kfree);
	phys_addr_t get_info_addr;
	u32 *get_info_data;
	u32 status;
	int ret;

	if (!priv || !s_info)
		goto exit;

	memset(s_info, 0x0, sizeof(*s_info));

	if (priv->mem_pool_name)
		get_info_data = get_phy_buf_mem_pool(dev,
						     priv->mem_pool_name,
						     &get_info_addr,
						     ELE_GET_INFO_BUFF_SZ);
	else
		get_info_data = dmam_alloc_coherent(dev,
						    ELE_GET_INFO_BUFF_SZ,
						    &get_info_addr,
						    GFP_KERNEL);
	if (!get_info_data) {
		ret = -ENOMEM;
		dev_err(dev,
			"%s: Failed to allocate get_info_addr.\n",
			__func__);
		goto exit;
	}

	tx_msg = kzalloc(ELE_GET_INFO_REQ_MSG_SZ << 2, GFP_KERNEL);
	if (!tx_msg) {
		ret = -ENOMEM;
		return ret;
	}

	rx_msg = kzalloc(ELE_GET_INFO_RSP_MSG_SZ << 2, GFP_KERNEL);
	if (!rx_msg) {
		ret = -ENOMEM;
		return ret;
	}

	ret = plat_fill_cmd_msg_hdr(priv,
				    (struct se_msg_hdr *)&tx_msg->header,
				    ELE_GET_INFO_REQ,
				    ELE_GET_INFO_REQ_MSG_SZ,
				    true);
	if (ret)
		goto exit;

	tx_msg->data[0] = upper_32_bits(get_info_addr);
	tx_msg->data[1] = lower_32_bits(get_info_addr);
	tx_msg->data[2] = ELE_GET_INFO_READ_SZ;
	priv->rx_msg = rx_msg;
	ret = imx_ele_msg_send_rcv(priv, tx_msg);
	if (ret < 0)
		goto exit;

	ret  = validate_rsp_hdr(priv,
				priv->rx_msg->header,
				ELE_GET_INFO_REQ,
				ELE_GET_INFO_RSP_MSG_SZ,
				true);
	if (ret)
		goto exit;

	status = RES_STATUS(priv->rx_msg->data[0]);
	if (status != priv->success_tag) {
		dev_err(dev, "Command Id[%d], Response Failure = 0x%x",
			ELE_GET_INFO_REQ, status);
		ret = -1;
	}

	s_info->imem_state = (get_info_data[ELE_IMEM_STATE_WORD]
				& ELE_IMEM_STATE_MASK) >> 16;
	s_info->major_ver = (get_info_data[GET_INFO_SOC_INFO_WORD_OFFSET]
				& SOC_VER_MASK) >> 24;
	s_info->minor_ver = ((get_info_data[GET_INFO_SOC_INFO_WORD_OFFSET]
				& SOC_VER_MASK) >> 16) & 0xFF;
	s_info->soc_rev = (get_info_data[GET_INFO_SOC_INFO_WORD_OFFSET]
				& SOC_VER_MASK) >> 16;
	s_info->soc_id = get_info_data[GET_INFO_SOC_INFO_WORD_OFFSET]
				& SOC_ID_MASK;
	s_info->serial_num
		= (u64)get_info_data[GET_INFO_SL_NUM_MSB_WORD_OFF] << 32
			| get_info_data[GET_INFO_SL_NUM_LSB_WORD_OFF];
exit:
	if (get_info_addr) {
		if (priv->mem_pool_name)
			free_phybuf_mem_pool(dev, priv->mem_pool_name,
					     get_info_data, ELE_GET_INFO_BUFF_SZ);
		else
			dmam_free_coherent(dev,
					   ELE_GET_INFO_BUFF_SZ,
					   get_info_data,
					   get_info_addr);
	}

	return ret;
}

int ele_ping(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct se_api_msg *tx_msg __free(kfree);
	struct se_api_msg *rx_msg __free(kfree);
	u32 status;
	int ret;

	tx_msg = kzalloc(ELE_PING_REQ_SZ << 2, GFP_KERNEL);
	if (!tx_msg) {
		ret = -ENOMEM;
		return ret;
	}

	rx_msg = kzalloc(ELE_PING_RSP_SZ << 2, GFP_KERNEL);
	if (!rx_msg) {
		ret = -ENOMEM;
		return ret;
	}

	ret = plat_fill_cmd_msg_hdr(priv,
				    (struct se_msg_hdr *)&tx_msg->header,
				    ELE_PING_REQ, ELE_PING_REQ_SZ,
				    true);
	if (ret) {
		dev_err(dev, "Error: plat_fill_cmd_msg_hdr failed.\n");
		goto exit;
	}

	priv->rx_msg = rx_msg;
	ret = imx_ele_msg_send_rcv(priv, tx_msg);
	if (ret)
		goto exit;

	ret  = validate_rsp_hdr(priv,
				priv->rx_msg->header,
				ELE_PING_REQ,
				ELE_PING_RSP_SZ,
				true);
	if (ret)
		goto exit;

	status = RES_STATUS(priv->rx_msg->data[0]);
	if (status != priv->success_tag) {
		dev_err(dev, "Command Id[%d], Response Failure = 0x%x",
			ELE_PING_REQ, status);
		ret = -1;
	}
exit:
	return ret;
}

int ele_service_swap(struct device *dev,
		     phys_addr_t addr,
		     u32 addr_size, u16 flag)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct se_api_msg *tx_msg __free(kfree);
	struct se_api_msg *rx_msg __free(kfree);
	u32 status;
	int ret;

	tx_msg = kzalloc(ELE_SERVICE_SWAP_REQ_MSG_SZ << 2, GFP_KERNEL);
	if (!tx_msg) {
		ret = -ENOMEM;
		return ret;
	}

	rx_msg = kzalloc(ELE_SERVICE_SWAP_RSP_MSG_SZ << 2, GFP_KERNEL);
	if (!rx_msg) {
		ret = -ENOMEM;
		return ret;
	}

	ret = plat_fill_cmd_msg_hdr(priv,
				    (struct se_msg_hdr *)&tx_msg->header,
				    ELE_SERVICE_SWAP_REQ,
				    ELE_SERVICE_SWAP_REQ_MSG_SZ,
				    true);
	if (ret)
		goto exit;

	tx_msg->data[0] = flag;
	tx_msg->data[1] = addr_size;
	tx_msg->data[2] = ELE_NONE_VAL;
	tx_msg->data[3] = lower_32_bits(addr);
	tx_msg->data[4] = plat_add_msg_crc((uint32_t *)&tx_msg[0],
						 ELE_SERVICE_SWAP_REQ_MSG_SZ);
	priv->rx_msg = rx_msg;
	ret = imx_ele_msg_send_rcv(priv, tx_msg);
	if (ret < 0)
		goto exit;

	ret  = validate_rsp_hdr(priv,
				priv->rx_msg->header,
				ELE_SERVICE_SWAP_REQ,
				ELE_SERVICE_SWAP_RSP_MSG_SZ,
				true);
	if (ret)
		goto exit;

	status = RES_STATUS(priv->rx_msg->data[0]);
	if (status != priv->success_tag) {
		dev_err(dev, "Command Id[%d], Response Failure = 0x%x",
			ELE_SERVICE_SWAP_REQ, status);
		ret = -1;
	} else {
		if (flag == ELE_IMEM_EXPORT)
			ret = priv->rx_msg->data[1];
		else
			ret = 0;
	}
exit:

	return ret;
}

int ele_fw_authenticate(struct device *dev, phys_addr_t addr)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct se_api_msg *tx_msg __free(kfree);
	struct se_api_msg *rx_msg __free(kfree);
	u32 status;
	int ret;

	tx_msg = kzalloc(ELE_FW_AUTH_REQ_SZ << 2, GFP_KERNEL);
	if (!tx_msg) {
		ret = -ENOMEM;
		return ret;
	}

	rx_msg = kzalloc(ELE_FW_AUTH_RSP_MSG_SZ << 2, GFP_KERNEL);
	if (!rx_msg) {
		ret = -ENOMEM;
		return ret;
	}
	ret = plat_fill_cmd_msg_hdr(priv,
				    (struct se_msg_hdr *)&tx_msg->header,
				    ELE_FW_AUTH_REQ,
				    ELE_FW_AUTH_REQ_SZ,
				    true);
	if (ret)
		goto exit;

	tx_msg->data[0] = addr;
	tx_msg->data[1] = 0x0;
	tx_msg->data[2] = addr;

	priv->rx_msg = rx_msg;
	ret = imx_ele_msg_send_rcv(priv, tx_msg);
	if (ret < 0)
		goto exit;

	ret  = validate_rsp_hdr(priv,
				priv->rx_msg->header,
				ELE_FW_AUTH_REQ,
				ELE_FW_AUTH_RSP_MSG_SZ,
				true);
	if (ret)
		goto exit;

	status = RES_STATUS(priv->rx_msg->data[0]);
	if (status != priv->success_tag) {
		dev_err(dev, "Command Id[%d], Response Failure = 0x%x",
			ELE_FW_AUTH_REQ, status);
		ret = -1;
	}
exit:

	return ret;
}
