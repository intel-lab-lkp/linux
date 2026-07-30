// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 NXP
 */

#include <linux/types.h>

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>

#include "ele_base_msg.h"
#include "ele_common.h"

#define FW_DBG_DUMP_FIXED_STR		"ELE"

int ele_uapi_allowed_base_cmd(struct se_if_priv *priv,
			      struct se_msg_hdr *header)
{
	switch (header->command) {
	case ELE_PING_REQ: return 0;
	case ELE_DEBUG_DUMP_REQ: return 0;
	case ELE_OEM_AUTH_CONTAINER_REQ: return 0;
	case ELE_OEM_VERIFY_IMAGE_REQ: return 0;
	case ELE_OEM_REL_CONTAINER_REQ: return 0;
	case ELE_FW_LIFE_CYCLE_REQ: return 0;
	case ELE_READ_FUSE_REQ: return 0;
	case ELE_GET_FW_VERS_REQ: return 0;
	case ELE_RETURN_LIFE_CYCLE_REQ: return 0;
	case ELE_GET_EVENT_REQ: return 0;
	case ELE_COMMIT_REQ: return 0;
	case ELE_GEN_KEY_BLOB_REQ: return 0;
	case ELE_GET_FW_STATUS_REQ: return 0;
	case ELE_XIP_DECRYPT_REQ: return 0;
	case ELE_WRITE_FUSE: return 0;
	case ELE_GET_INFO_REQ: return 0;
	case ELE_DEV_ATTEST_REQ: return 0;
	case ELE_WRITE_SHADOW_FUSE_REQ: return 0;
	case ELE_READ_SHADOW_FUSE_REQ: return 0;
	default:
		return -EACCES;
	}
}

static void ele_get_info_cleanup(struct se_if_priv *priv, u32 *buf, dma_addr_t d_addr,
				 size_t size)
{
	/* For the case when priv->mem_pool != NULL:
	 *
	 *   If this probe-time transaction timed out, the firmware may
	 *   still write into the SRAM buffer after this function returns.
	 *   Do not release it back to the pool while the firmware-busy
	 *   circuit breaker still marks this context as owning an
	 *   outstanding transaction. The buffer is reclaimed with the
	 *   device on unbind; leaking this fixed-size probe buffer is
	 *   preferable to letting the firmware corrupt reused pool memory.
	 *   This mirrors the guard already applied on the shared-memory
	 *   cleanup path below.
	 */

	if (priv->mem_pool) {
		if (se_is_fw_busy_ctx(priv->priv_dev_ctx))
			return;
		se_cleanup_mem_pool_buf(priv->priv_dev_ctx, true);
	} else {
		se_dev_ctx_shared_mem_cleanup(priv->priv_dev_ctx);
	}
}

int ele_get_info(struct se_if_priv *priv, struct ele_dev_info *s_info)
{
	dma_addr_t get_info_addr = 0;
	void *get_info_data = NULL;
	u32 get_info_len;
	int ret = 0;

	if (!priv)
		return -EINVAL;

	guard(mutex)(&priv->priv_dev_ctx->fops_lock);
	memset(s_info, 0x0, sizeof(*s_info));

	struct se_api_msg *tx_msg __free(kfree) =
		kzalloc(ELE_GET_INFO_REQ_MSG_SZ, GFP_KERNEL);
	if (!tx_msg)
		return -ENOMEM;

	struct se_api_msg *rx_msg __free(kfree) =
		kzalloc(ELE_GET_INFO_RSP_MSG_SZ, GFP_KERNEL);
	if (!rx_msg)
		return -ENOMEM;

	get_info_len = ELE_GET_INFO_BUFF_SZ;
	if (priv->mem_pool) {
		ret = se_get_mem_pool_buf(priv->priv_dev_ctx, &get_info_data,
					  &get_info_addr, get_info_len);
		if (ret) {
			dev_err(priv->dev, "Failed[0x%x] to alloc from gen_pool.\n", ret);
			return -ENOMEM;
		}
	} else {
		ret = get_shared_mem_slot(priv->priv_dev_ctx,
					  &get_info_len, &get_info_addr,
					  &get_info_data);
		if (ret) {
			dev_err(priv->dev, "Failed to allocate buffer.\n");
			return -ENOMEM;
		}
	}

	se_fill_cmd_msg_hdr(priv, (struct se_msg_hdr *)&tx_msg->header,
			    ELE_GET_INFO_REQ, ELE_GET_INFO_REQ_MSG_SZ, true);

	tx_msg->data[0] = upper_32_bits(get_info_addr);
	tx_msg->data[1] = lower_32_bits(get_info_addr);
	tx_msg->data[2] = sizeof(*s_info);

	ret = ele_msg_send_rcv(priv->priv_dev_ctx, tx_msg, ELE_GET_INFO_REQ_MSG_SZ,
			       rx_msg, ELE_GET_INFO_RSP_MSG_SZ);
	if (ret < 0) {
		ele_get_info_cleanup(priv, get_info_data, get_info_addr, get_info_len);
		return ret;
	}

	ret = se_val_rsp_hdr_n_status(priv, rx_msg, ELE_GET_INFO_REQ,
				      ELE_GET_INFO_RSP_MSG_SZ, true);
	if (ret < 0) {
		ele_get_info_cleanup(priv, get_info_data, get_info_addr, get_info_len);
		return ret;
	}

	memcpy(s_info, get_info_data, sizeof(*s_info));

	ele_get_info_cleanup(priv, get_info_data, get_info_addr, get_info_len);

	return ret;
}

int ele_fetch_soc_info(struct se_if_priv *priv, void *data)
{
	return ele_get_info(priv, (struct ele_dev_info *)data);
}

int ele_ping(struct se_if_priv *priv)
{
	int ret = 0;

	if (!priv)
		return -EINVAL;

	struct se_api_msg *tx_msg __free(kfree) = kzalloc(ELE_PING_REQ_SZ,
							  GFP_KERNEL);
	if (!tx_msg)
		return -ENOMEM;

	struct se_api_msg *rx_msg __free(kfree) = kzalloc(ELE_PING_RSP_SZ,
							  GFP_KERNEL);
	if (!rx_msg)
		return -ENOMEM;

	se_fill_cmd_msg_hdr(priv, (struct se_msg_hdr *)&tx_msg->header,
			    ELE_PING_REQ, ELE_PING_REQ_SZ, true);

	ret = ele_msg_send_rcv(priv->priv_dev_ctx, tx_msg, ELE_PING_REQ_SZ,
			       rx_msg, ELE_PING_RSP_SZ);
	if (ret < 0)
		return ret;

	ret = se_val_rsp_hdr_n_status(priv, rx_msg, ELE_PING_REQ,
				      ELE_PING_RSP_SZ, true);

	return ret;
}

int ele_service_swap(struct se_if_priv *priv,
		     dma_addr_t addr,
		     u32 addr_size, u16 flag)
{
	int ret = 0;

	if (!priv)
		return -EINVAL;

	if (upper_32_bits(addr)) {
		dev_err(priv->dev,
			"ELE service-swap address exceeds 32-bit range: %pad\n",
			&addr);
		return -ERANGE;
	}

	struct se_api_msg *tx_msg __free(kfree)	=
		kzalloc(ELE_SERVICE_SWAP_REQ_MSG_SZ, GFP_KERNEL);
	if (!tx_msg)
		return -ENOMEM;

	struct se_api_msg *rx_msg __free(kfree) =
		kzalloc(ELE_SERVICE_SWAP_RSP_MSG_SZ, GFP_KERNEL);
	if (!rx_msg)
		return -ENOMEM;

	se_fill_cmd_msg_hdr(priv, (struct se_msg_hdr *)&tx_msg->header,
			    ELE_SERVICE_SWAP_REQ, ELE_SERVICE_SWAP_REQ_MSG_SZ, true);

	tx_msg->data[0] = flag;
	tx_msg->data[1] = addr_size;
	tx_msg->data[2] = ELE_NONE_VAL;
	tx_msg->data[3] = lower_32_bits(addr);
	ret = se_update_msg_chksum((u32 *)&tx_msg[0], ELE_SERVICE_SWAP_REQ_MSG_SZ);
	if (ret)
		return -EINVAL;

	ret = ele_msg_send_rcv(priv->priv_dev_ctx, tx_msg, ELE_SERVICE_SWAP_REQ_MSG_SZ,
			       rx_msg, ELE_SERVICE_SWAP_RSP_MSG_SZ);
	if (ret < 0)
		return ret;

	ret = se_val_rsp_hdr_n_status(priv, rx_msg, ELE_SERVICE_SWAP_REQ,
				      ELE_SERVICE_SWAP_RSP_MSG_SZ, true);
	if (ret)
		return ret;

	if (flag == ELE_IMEM_EXPORT)
		ret = rx_msg->data[1];
	else
		ret = 0;

	return ret;
}

int ele_fw_authenticate(struct se_if_priv *priv, dma_addr_t contnr_addr,
			dma_addr_t img_addr)
{
	int ret = 0;

	if (!priv)
		return -EINVAL;

	if (upper_32_bits(contnr_addr) || upper_32_bits(img_addr)) {
		dev_err(priv->dev, "Wrong address: %pap %pap\n", &contnr_addr, &img_addr);
		return -EINVAL;
	}

	struct se_api_msg *tx_msg __free(kfree)	=
		kzalloc(ELE_FW_AUTH_REQ_SZ, GFP_KERNEL);
	if (!tx_msg)
		return -ENOMEM;

	struct se_api_msg *rx_msg __free(kfree) =
		kzalloc(ELE_FW_AUTH_RSP_MSG_SZ, GFP_KERNEL);
	if (!rx_msg)
		return -ENOMEM;

	se_fill_cmd_msg_hdr(priv, (struct se_msg_hdr *)&tx_msg->header,
			    ELE_FW_AUTH_REQ, ELE_FW_AUTH_REQ_SZ, true);

	tx_msg->data[0] = lower_32_bits(contnr_addr);
	tx_msg->data[1] = 0;
	tx_msg->data[2] = lower_32_bits(img_addr);

	ret = ele_msg_send_rcv(priv->priv_dev_ctx, tx_msg, ELE_FW_AUTH_REQ_SZ, rx_msg,
			       ELE_FW_AUTH_RSP_MSG_SZ);
	if (ret < 0)
		return ret;

	ret = se_val_rsp_hdr_n_status(priv, rx_msg, ELE_FW_AUTH_REQ,
				      ELE_FW_AUTH_RSP_MSG_SZ, true);

	return ret;
}

int ele_debug_dump(struct se_if_priv *priv)
{
	bool keep_logging;
	int msg_ex_cnt;
	int ret = 0;
	int i;

	if (!priv)
		return -EINVAL;

	struct se_api_msg *tx_msg __free(kfree) = kzalloc(ELE_DEBUG_DUMP_REQ_SZ,
							  GFP_KERNEL);
	if (!tx_msg)
		return -ENOMEM;

	struct se_api_msg *rx_msg __free(kfree)	= kzalloc(ELE_DEBUG_DUMP_RSP_SZ,
							  GFP_KERNEL);
	if (!rx_msg)
		return -ENOMEM;

	se_fill_cmd_msg_hdr(priv, &tx_msg->header, ELE_DEBUG_DUMP_REQ,
			    ELE_DEBUG_DUMP_REQ_SZ, true);

	msg_ex_cnt = 0;
	do {
		memset(rx_msg, 0x0, ELE_DEBUG_DUMP_RSP_SZ);

		ret = ele_msg_send_rcv(priv->priv_dev_ctx, tx_msg, ELE_DEBUG_DUMP_REQ_SZ,
				       rx_msg, ELE_DEBUG_DUMP_RSP_SZ);
		if (ret < 0)
			return ret;

		ret = se_val_rsp_hdr_n_status(priv, rx_msg, ELE_DEBUG_DUMP_REQ,
					      ELE_DEBUG_DUMP_RSP_SZ, true);
		if (ret) {
			dev_err(priv->dev, "Dump_Debug_Buffer Error: %x.", ret);
			break;
		}
		keep_logging = (rx_msg->header.size >= (ELE_DEBUG_DUMP_RSP_SZ >> 2) &&
				msg_ex_cnt < ELE_MAX_DBG_DMP_PKT);

		rx_msg->header.size -= 2;

		if (rx_msg->header.size > 2)
			rx_msg->header.size--;

		for (i = 0; i < rx_msg->header.size; i += 2)
			dev_info(priv->dev, "%s%02x_%02x: 0x%08x 0x%08x",
				 FW_DBG_DUMP_FIXED_STR,	msg_ex_cnt, i,
				 rx_msg->data[i + 1], rx_msg->data[i + 2]);

		msg_ex_cnt++;
	} while (keep_logging);

	return ret;
}
