// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Broadcom Corporation
 */

#include <linux/auxiliary_bus.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/fwctl.h>
#include <linux/bnxt/hsi.h>
#include <linux/bnxt/ulp.h>
#include <uapi/fwctl/fwctl.h>
#include <uapi/fwctl/bnxt.h>

struct bnxtctl_uctx {
	struct fwctl_uctx uctx;
	u32 uctx_caps;
};

struct bnxtctl_dev {
	struct fwctl_device fwctl;
	struct bnxt_aux_priv *aux_priv;
};

DEFINE_FREE(bnxtctl, struct bnxtctl_dev *, if (_T) fwctl_put(&_T->fwctl))

static int bnxtctl_open_uctx(struct fwctl_uctx *uctx)
{
	struct bnxtctl_uctx *bnxtctl_uctx =
		container_of(uctx, struct bnxtctl_uctx, uctx);

	bnxtctl_uctx->uctx_caps = BIT(FWCTL_BNXT_INLINE_COMMANDS) |
				  BIT(FWCTL_BNXT_QUERY_COMMANDS) |
				  BIT(FWCTL_BNXT_SEND_COMMANDS) |
				  BIT(FWCTL_BNXT_DMA_COMMANDS);
	return 0;
}

static void bnxtctl_close_uctx(struct fwctl_uctx *uctx)
{
}

static void *bnxtctl_info(struct fwctl_uctx *uctx, size_t *length)
{
	struct bnxtctl_uctx *bnxtctl_uctx =
		container_of(uctx, struct bnxtctl_uctx, uctx);
	struct fwctl_info_bnxt *info;

	info = kzalloc_obj(*info);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->uctx_caps = bnxtctl_uctx->uctx_caps;

	*length = sizeof(*info);
	return info;
}

struct bnxtctl_dma_field {
	size_t offset;	/* offsetof(hwrm_xxx_input, field_name) */
	u8     dir;
};

struct bnxtctl_cmd_dma_desc {
	u16                      req_type;
	u8                       num_fields;
	u8                       scope_min;
	struct bnxtctl_dma_field fields[FWCTL_BNXT_MAX_BUFS];
};

/*
 * Per-command DMA buffer descriptor table for HWRM commands that
 * carry __le64 DMA address fields in their input
 */
static const struct bnxtctl_cmd_dma_desc bnxtctl_dma_cmds[] = {
	{ HWRM_NVM_SET_VARIABLE, 1, FWCTL_RPC_CONFIGURATION,
	  {{ offsetof(struct hwrm_nvm_set_variable_input, src_data_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE }} },
	{ HWRM_NVM_GET_VARIABLE, 1, FWCTL_RPC_CONFIGURATION,
	  {{ offsetof(struct hwrm_nvm_get_variable_input, dest_data_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_NVM_READ, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_nvm_read_input, host_dest_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_NVM_GET_DIR_ENTRIES, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_nvm_get_dir_entries_input, host_dest_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_NVM_WRITE, 1, FWCTL_RPC_DEBUG_WRITE,
	  {{ offsetof(struct hwrm_nvm_write_input, host_src_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE }} },
	{ HWRM_NVM_MODIFY, 1, FWCTL_RPC_DEBUG_WRITE,
	  {{ offsetof(struct hwrm_nvm_modify_input, host_src_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE }} },
	{ HWRM_NVM_RAW_WRITE_BLK, 1, FWCTL_RPC_DEBUG_WRITE_FULL,
	  {{ offsetof(struct hwrm_nvm_raw_write_blk_input, host_src_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE }} },
	{ HWRM_NVM_RAW_DUMP, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_nvm_raw_dump_input, host_dest_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },

	{ HWRM_FW_GET_STRUCTURED_DATA, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_fw_get_structured_data_input, dest_data_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_FW_SET_STRUCTURED_DATA, 1, FWCTL_RPC_DEBUG_WRITE,
	  {{ offsetof(struct hwrm_fw_set_structured_data_input, src_data_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE }} },
	{ HWRM_FW_LIVEPATCH, 1, FWCTL_RPC_DEBUG_WRITE_FULL,
	  {{ offsetof(struct hwrm_fw_livepatch_input, host_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE }} },

	{ HWRM_DBG_COREDUMP_LIST, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_dbg_coredump_list_input, host_dest_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_DBG_COREDUMP_RETRIEVE, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_dbg_coredump_retrieve_input, host_dest_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_DBG_READ_DIRECT, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_dbg_read_direct_input, host_dest_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_DBG_READ_INDIRECT, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_dbg_read_indirect_input, host_dest_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_DBG_SERDES_TEST, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_dbg_serdes_test_input, resp_data_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_DBG_TOKEN_CFG, 1, FWCTL_RPC_DEBUG_WRITE_FULL,
	  {{ offsetof(struct hwrm_dbg_token_cfg_input, host_src_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE }} },

	{ HWRM_QUEUE_DSCP2PRI_QCFG, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_queue_dscp2pri_qcfg_input, dest_data_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },

	{ HWRM_PORT_QSTATS, 2, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_port_qstats_input, tx_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE },
	   { offsetof(struct hwrm_port_qstats_input, rx_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_PORT_QSTATS_EXT, 2, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_port_qstats_ext_input, tx_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE },
	   { offsetof(struct hwrm_port_qstats_ext_input, rx_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_PORT_QSTATS_EXT_PFC_ADV, 2, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_port_qstats_ext_pfc_adv_input,
		      tx_pfc_adv_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE },
	   { offsetof(struct hwrm_port_qstats_ext_pfc_adv_input,
		      rx_pfc_adv_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_PCIE_QSTATS, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_pcie_qstats_input, pcie_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_STAT_GENERIC_QSTATS, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_stat_generic_qstats_input,
		      generic_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_STAT_QUERY_ROCE_STATS, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_stat_query_roce_stats_input,
		      roce_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_STAT_QUERY_ROCE_STATS_EXT, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_stat_query_roce_stats_ext_input,
		      roce_stat_host_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },

	{ HWRM_PORT_EVENTS_LOG, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_port_events_log_input, host_dest_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_PORT_PRBS_TEST, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_port_prbs_test_input, resp_data_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
	{ HWRM_PORT_DSC_DUMP, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_port_dsc_dump_input, resp_data_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },

	{ HWRM_SCH_GRP_CFG, 1, FWCTL_RPC_DEBUG_WRITE,
	  {{ offsetof(struct hwrm_sch_grp_cfg_input, fid_table_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE }} },
	{ HWRM_SCH_GRP_QCFG, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_sch_grp_qcfg_input, fid_table_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },

	{ HWRM_SELFTEST_RETRIEVE_SERDES_DATA, 1, FWCTL_RPC_DEBUG_READ_ONLY,
	  {{ offsetof(struct hwrm_selftest_retrieve_serdes_data_input, resp_data_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },

	{ HWRM_DBG_PTRACE, 2, FWCTL_RPC_DEBUG_WRITE,
	  {{ offsetof(struct hwrm_dbg_ptrace_input, pdi_cmd_buf_addr),
	     FWCTL_BNXT_BUF_TO_DEVICE },
	   { offsetof(struct hwrm_dbg_ptrace_input, pdi_resp_buf_addr),
	     FWCTL_BNXT_BUF_FROM_DEVICE }} },
};

static const struct bnxtctl_cmd_dma_desc *
bnxtctl_find_dma_desc(u16 req_type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bnxtctl_dma_cmds); i++)
		if (bnxtctl_dma_cmds[i].req_type == req_type)
			return &bnxtctl_dma_cmds[i];
	return NULL;
}

static void bnxtctl_zero_dma_fields(void *cmd,
				    const struct bnxtctl_cmd_dma_desc *desc)
{
	int i;

	for (i = 0; i < desc->num_fields; i++) {
		__le64 *field = cmd + desc->fields[i].offset;

		*field = 0;
	}
}

static int bnxtctl_map_dma_bufs(struct device *dev, void *cmd,
				const struct bnxtctl_cmd_dma_desc *desc,
				const struct fwctl_bnxt_driver_data *dd,
				void **kbufs, dma_addr_t *dma_addrs,
				unsigned int *num_mapped)
{
	unsigned int i;

	*num_mapped = 0;
	for (i = 0; i < dd->num_bufs; i++) {
		enum dma_data_direction dma_dir;
		__le64 *field;

		if (dd->bufs[i].dir != desc->fields[i].dir)
			return -EINVAL;

		if (!dd->bufs[i].len || dd->bufs[i].len > FWCTL_BNXT_MAX_DMABUF)
			return -EINVAL;

		dma_dir = (dd->bufs[i].dir == FWCTL_BNXT_BUF_TO_DEVICE) ?
			DMA_TO_DEVICE : DMA_FROM_DEVICE;

		kbufs[i] = kvzalloc(dd->bufs[i].len, GFP_KERNEL);
		if (!kbufs[i])
			return -ENOMEM;

		if (dma_dir == DMA_TO_DEVICE &&
		    copy_from_user(kbufs[i],
				   u64_to_user_ptr(dd->bufs[i].addr),
				   dd->bufs[i].len)) {
			kvfree(kbufs[i]);
			kbufs[i] = NULL;
			return -EFAULT;
		}

		dma_addrs[i] = dma_map_single(dev, kbufs[i],
					      dd->bufs[i].len, dma_dir);
		if (dma_mapping_error(dev, dma_addrs[i])) {
			kvfree(kbufs[i]);
			kbufs[i] = NULL;
			return -ENOMEM;
		}

		(*num_mapped)++;

		field = cmd + desc->fields[i].offset;
		*field = cpu_to_le64(dma_addrs[i]);
	}
	return 0;
}

static int bnxtctl_unmap_dma_bufs(struct device *dev,
				  const struct bnxtctl_cmd_dma_desc *desc,
				  const struct fwctl_bnxt_driver_data *dd,
				  void **kbufs, dma_addr_t *dma_addrs,
				  unsigned int num_mapped)
{
	unsigned int i;
	int rc = 0;

	for (i = 0; i < num_mapped; i++) {
		enum dma_data_direction dma_dir;

		dma_dir = (dd->bufs[i].dir == FWCTL_BNXT_BUF_TO_DEVICE) ?
			DMA_TO_DEVICE : DMA_FROM_DEVICE;

		dma_unmap_single(dev, dma_addrs[i], dd->bufs[i].len, dma_dir);

		if (dma_dir == DMA_FROM_DEVICE &&
		    copy_to_user(u64_to_user_ptr(dd->bufs[i].addr),
				 kbufs[i], dd->bufs[i].len))
			rc = -EFAULT;

		kvfree(kbufs[i]);
	}
	return rc;
}

/* Caller must hold edev->en_dev_lock */
static bool bnxtctl_validate_rpc(struct bnxt_en_dev *edev,
				 struct bnxt_fw_msg *hwrm_in,
				 enum fwctl_rpc_scope scope)
{
	struct input *req = (struct input *)hwrm_in->msg;

	lockdep_assert_held(&edev->en_dev_lock);
	if (edev->flags & BNXT_EN_FLAG_ULP_STOPPED)
		return false;

	switch (le16_to_cpu(req->req_type)) {
	case HWRM_FUNC_RESET:
	case HWRM_PORT_CLR_STATS:
	case HWRM_FW_RESET:
	case HWRM_FW_SYNC:
	case HWRM_FW_SET_TIME:
	case HWRM_DBG_LOG_BUFFER_FLUSH:
	case HWRM_DBG_ERASE_NVM:
	case HWRM_DBG_CFG:
	case HWRM_NVM_DEFRAG:
	case HWRM_NVM_FACTORY_DEFAULTS:
	case HWRM_NVM_FLUSH:
	case HWRM_NVM_VERIFY_UPDATE:
	case HWRM_NVM_ERASE_DIR_ENTRY:
	case HWRM_NVM_MOD_DIR_ENTRY:
	case HWRM_NVM_FIND_DIR_ENTRY:
	case HWRM_NVM_SET_VARIABLE:
	case HWRM_NVM_GET_VARIABLE:
		return scope >= FWCTL_RPC_CONFIGURATION;

	case HWRM_VER_GET:
	case HWRM_ERROR_RECOVERY_QCFG:
	case HWRM_FUNC_QCAPS:
	case HWRM_FUNC_QCFG:
	case HWRM_FUNC_QSTATS:
	case HWRM_PORT_PHY_QCFG:
	case HWRM_PORT_MAC_QCFG:
	case HWRM_PORT_PHY_QCAPS:
	case HWRM_PORT_PHY_I2C_READ:
	case HWRM_PORT_PHY_MDIO_READ:
	case HWRM_QUEUE_PRI2COS_QCFG:
	case HWRM_QUEUE_COS2BW_QCFG:
	case HWRM_VNIC_RSS_QCFG:
	case HWRM_QUEUE_GLOBAL_QCFG:
	case HWRM_QUEUE_ADPTV_QOS_RX_FEATURE_QCFG:
	case HWRM_QUEUE_ADPTV_QOS_TX_FEATURE_QCFG:
	case HWRM_QUEUE_QCAPS:
	case HWRM_QUEUE_ADPTV_QOS_RX_TUNING_QCFG:
	case HWRM_QUEUE_ADPTV_QOS_TX_TUNING_QCFG:
	case HWRM_TUNNEL_DST_PORT_QUERY:
	case HWRM_PORT_TX_FIR_QCFG:
	case HWRM_FW_LIVEPATCH_QUERY:
	case HWRM_FW_QSTATUS:
	case HWRM_FW_HEALTH_CHECK:
	case HWRM_FW_GET_TIME:
	case HWRM_PORT_EP_TX_QCFG:
	case HWRM_PORT_QCFG:
	case HWRM_PORT_MAC_QCAPS:
	case HWRM_TEMP_MONITOR_QUERY:
	case HWRM_REG_POWER_QUERY:
	case HWRM_CORE_FREQUENCY_QUERY:
	case HWRM_CFA_REDIRECT_QUERY_TUNNEL_TYPE:
	case HWRM_CFA_ADV_FLOW_MGNT_QCAPS:
	case HWRM_FUNC_RESOURCE_QCAPS:
	case HWRM_FUNC_BACKING_STORE_QCAPS:
	case HWRM_FUNC_BACKING_STORE_QCFG:
	case HWRM_FUNC_QSTATS_EXT:
	case HWRM_FUNC_PTP_PIN_QCFG:
	case HWRM_FUNC_PTP_EXT_QCFG:
	case HWRM_FUNC_BACKING_STORE_QCFG_V2:
	case HWRM_FUNC_BACKING_STORE_QCAPS_V2:
	case HWRM_FUNC_SYNCE_QCFG:
	case HWRM_FUNC_TTX_PACING_RATE_PROF_QUERY:
	case HWRM_PORT_PHY_FDRSTAT:
	case HWRM_DBG_RING_INFO_GET:
	case HWRM_DBG_QCAPS:
	case HWRM_DBG_QCFG:
	case HWRM_DBG_USEQ_FLUSH:
	case HWRM_DBG_USEQ_QCAPS:
	case HWRM_DBG_SIM_CABLE_STATE:
	case HWRM_DBG_TOKEN_QUERY_AUTH_IDS:
	case HWRM_NVM_GET_DEV_INFO:
	case HWRM_NVM_GET_DIR_INFO:
	case HWRM_SELFTEST_QLIST:
	case HWRM_NVM_READ:
	case HWRM_NVM_GET_DIR_ENTRIES:
	case HWRM_FW_GET_STRUCTURED_DATA:
	case HWRM_DBG_COREDUMP_LIST:
	case HWRM_DBG_COREDUMP_RETRIEVE:
	case HWRM_DBG_COREDUMP_INITIATE:
	case HWRM_DBG_READ_DIRECT:
	case HWRM_QUEUE_DSCP2PRI_QCFG:
	case HWRM_PORT_QSTATS:
	case HWRM_PORT_QSTATS_EXT:
	case HWRM_PORT_QSTATS_EXT_PFC_ADV:
	case HWRM_PCIE_QSTATS:
	case HWRM_STAT_GENERIC_QSTATS:
	case HWRM_STAT_QUERY_ROCE_STATS:
	case HWRM_STAT_QUERY_ROCE_STATS_EXT:
	case HWRM_PORT_EVENTS_LOG:
	case HWRM_PORT_PRBS_TEST:
	case HWRM_PORT_DSC_DUMP:
	case HWRM_DBG_READ_INDIRECT:
	case HWRM_DBG_SERDES_TEST:
	case HWRM_SCH_GRP_QCFG:
	case HWRM_SELFTEST_RETRIEVE_SERDES_DATA:
	case HWRM_NVM_RAW_DUMP:
		return scope >= FWCTL_RPC_DEBUG_READ_ONLY;

	case HWRM_PORT_PHY_I2C_WRITE:
	case HWRM_PORT_PHY_MDIO_WRITE:
	case HWRM_NVM_WRITE:
	case HWRM_NVM_MODIFY:
	case HWRM_FW_SET_STRUCTURED_DATA:
	case HWRM_SCH_GRP_CFG:
	case HWRM_DBG_PTRACE:
		return scope >= FWCTL_RPC_DEBUG_WRITE;

	case HWRM_FW_LIVEPATCH:
	case HWRM_NVM_RAW_WRITE_BLK:
	case HWRM_DBG_TOKEN_CFG:
		return scope >= FWCTL_RPC_DEBUG_WRITE_FULL;

	default:
		return false;
	}
}

#define BNXTCTL_HWRM_CMD_TIMEOUT_DFLT	500	/* ms */
#define BNXTCTL_HWRM_CMD_TIMEOUT_MEDM	2000	/* ms */
#define BNXTCTL_HWRM_CMD_TIMEOUT_LONG	60000	/* ms */

static unsigned int bnxtctl_get_timeout(struct input *req)
{
	switch (le16_to_cpu(req->req_type)) {
	case HWRM_NVM_DEFRAG:
	case HWRM_NVM_FACTORY_DEFAULTS:
	case HWRM_NVM_FLUSH:
	case HWRM_NVM_VERIFY_UPDATE:
	case HWRM_NVM_ERASE_DIR_ENTRY:
	case HWRM_NVM_MOD_DIR_ENTRY:
	case HWRM_NVM_WRITE:
	case HWRM_FW_SYNC:
	case HWRM_DBG_COREDUMP_LIST:
	case HWRM_DBG_COREDUMP_RETRIEVE:
	case HWRM_DBG_COREDUMP_INITIATE:
	case HWRM_SELFTEST_RETRIEVE_SERDES_DATA:
	case HWRM_DBG_SERDES_TEST:
	case HWRM_NVM_RAW_WRITE_BLK:
	case HWRM_FW_HEALTH_CHECK:
		return BNXTCTL_HWRM_CMD_TIMEOUT_LONG;
	case HWRM_FUNC_RESET:
		return BNXTCTL_HWRM_CMD_TIMEOUT_MEDM;
	default:
		return BNXTCTL_HWRM_CMD_TIMEOUT_DFLT;
	}
}

static void *bnxtctl_fw_rpc(struct fwctl_uctx *uctx,
			    enum fwctl_rpc_scope scope,
			    void *in, size_t in_len, size_t *out_len,
			    __u64 driver_data)
{
	struct bnxtctl_dev *bnxtctl =
		container_of(uctx->fwctl, struct bnxtctl_dev, fwctl);
	struct bnxt_en_dev *edev = bnxtctl->aux_priv->edev;
	dma_addr_t dma_addrs[FWCTL_BNXT_MAX_BUFS];
	const struct bnxtctl_cmd_dma_desc *desc;
	void *kbufs[FWCTL_BNXT_MAX_BUFS] = {0};
	struct fwctl_bnxt_driver_data dd = {0};
	struct device *dev = &edev->pdev->dev;
	struct bnxt_fw_msg rpc_in = {0};
	unsigned int num_mapped = 0;
	struct input *req = in;
	int rc;

	if (in_len < sizeof(struct input) || in_len > HWRM_MAX_REQ_LEN)
		return ERR_PTR(-EINVAL);

	if (*out_len < sizeof(struct output))
		return ERR_PTR(-EINVAL);

	desc = bnxtctl_find_dma_desc(le16_to_cpu(req->req_type));

	if (desc) {
		if (!driver_data)
			return ERR_PTR(-EINVAL);
		if (copy_from_user(&dd, u64_to_user_ptr(driver_data),
				   sizeof(dd)))
			return ERR_PTR(-EFAULT);
		if (dd.rsvd || !dd.num_bufs || dd.num_bufs > desc->num_fields)
			return ERR_PTR(-EINVAL);

		for (unsigned int i = 0; i < dd.num_bufs; i++) {
			if (memchr_inv(dd.bufs[i].rsvd, 0,
				       sizeof(dd.bufs[i].rsvd)))
				return ERR_PTR(-EINVAL);
		}
		bnxtctl_zero_dma_fields(in, desc);
	} else {
		/* Non-DMA command: driver_data must be absent. */
		if (driver_data)
			return ERR_PTR(-EOPNOTSUPP);
	}

	rpc_in.msg = in;
	rpc_in.msg_len = in_len;
	rpc_in.resp = kzalloc(*out_len, GFP_KERNEL);
	if (!rpc_in.resp)
		return ERR_PTR(-ENOMEM);

	rpc_in.resp_max_len = *out_len;
	rpc_in.timeout = bnxtctl_get_timeout(in);

	guard(mutex)(&edev->en_dev_lock);

	if (!bnxtctl_validate_rpc(edev, &rpc_in, scope)) {
		kfree(rpc_in.resp);
		return ERR_PTR(-EPERM);
	}

	if (desc) {
		rc = bnxtctl_map_dma_bufs(dev, in, desc, &dd,
					  kbufs, dma_addrs, &num_mapped);
		if (rc) {
			bnxtctl_unmap_dma_bufs(dev, desc, &dd,
					       kbufs, dma_addrs, num_mapped);
			kfree(rpc_in.resp);
			return ERR_PTR(rc);
		}
	}

	rc = bnxt_send_msg(edev, &rpc_in);
	if (rc) {
		struct output *resp = rpc_in.resp;

		/* Copy the response to user always, as it contains
		 * detailed status of the command failure
		 */
		if (!resp->error_code)
			/* bnxt_send_msg() returned much before FW
			 * received the command.
			 */
			resp->error_code = cpu_to_le16(rc);
	}

	if (desc) {
		int unmap_rc;

		unmap_rc = bnxtctl_unmap_dma_bufs(dev, desc, &dd, kbufs,
						  dma_addrs, num_mapped);
		if (unmap_rc) {
			kfree(rpc_in.resp);
			return ERR_PTR(unmap_rc);
		}
	}

	return rpc_in.resp;
}

static const struct fwctl_ops bnxtctl_ops = {
	.device_type = FWCTL_DEVICE_TYPE_BNXT,
	.uctx_size = sizeof(struct bnxtctl_uctx),
	.open_uctx = bnxtctl_open_uctx,
	.close_uctx = bnxtctl_close_uctx,
	.info = bnxtctl_info,
	.fw_rpc = bnxtctl_fw_rpc,
};

static int bnxtctl_probe(struct auxiliary_device *adev,
			 const struct auxiliary_device_id *id)
{
	struct bnxt_aux_priv *aux_priv =
		container_of(adev, struct bnxt_aux_priv, aux_dev);
	struct bnxtctl_dev *bnxtctl __free(bnxtctl) =
		fwctl_alloc_device(&aux_priv->edev->pdev->dev, &bnxtctl_ops,
				   struct bnxtctl_dev, fwctl);
	int rc;

	if (!bnxtctl)
		return -ENOMEM;

	bnxtctl->aux_priv = aux_priv;

	rc = fwctl_register(&bnxtctl->fwctl);
	if (rc)
		return rc;

	auxiliary_set_drvdata(adev, no_free_ptr(bnxtctl));
	return 0;
}

static void bnxtctl_remove(struct auxiliary_device *adev)
{
	struct bnxtctl_dev *ctldev = auxiliary_get_drvdata(adev);

	fwctl_unregister(&ctldev->fwctl);
	fwctl_put(&ctldev->fwctl);
}

static const struct auxiliary_device_id bnxtctl_id_table[] = {
	{ .name = "bnxt_en.fwctl", },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, bnxtctl_id_table);

static struct auxiliary_driver bnxtctl_driver = {
	.name = "bnxt_fwctl",
	.probe = bnxtctl_probe,
	.remove = bnxtctl_remove,
	.id_table = bnxtctl_id_table,
};

module_auxiliary_driver(bnxtctl_driver);

MODULE_IMPORT_NS("FWCTL");
MODULE_DESCRIPTION("BNXT fwctl driver");
MODULE_AUTHOR("Pavan Chebbi <pavan.chebbi@broadcom.com>");
MODULE_AUTHOR("Andy Gospodarek <gospo@broadcom.com>");
MODULE_LICENSE("GPL");
