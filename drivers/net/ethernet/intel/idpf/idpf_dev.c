// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2023 Intel Corporation */

#include "idpf.h"
#include "idpf_lan_pf_regs.h"
#include "idpf_virtchnl.h"
#include "idpf_ptp.h"

#define IDPF_PF_ITR_IDX_SPACING		0x4

/**
 * idpf_ctlq_reg_init - initialize default mailbox registers
 * @mmio: struct that contains MMIO region info
 * @cci: struct where the register offset pointer to be copied to
 */
static void idpf_ctlq_reg_init(struct libie_mmio_info *mmio,
			       struct libie_ctlq_create_info *cci)
{
	struct libie_ctlq_reg *tx_reg = &cci[LIBIE_CTLQ_TYPE_TX].reg;
	struct libie_ctlq_reg *rx_reg = &cci[LIBIE_CTLQ_TYPE_RX].reg;

	tx_reg->head		= libie_pci_get_mmio_addr(mmio, PF_FW_ATQH);
	tx_reg->tail		= libie_pci_get_mmio_addr(mmio, PF_FW_ATQT);
	tx_reg->len		= libie_pci_get_mmio_addr(mmio, PF_FW_ATQLEN);
	tx_reg->addr_high	= libie_pci_get_mmio_addr(mmio, PF_FW_ATQBAH);
	tx_reg->addr_low	= libie_pci_get_mmio_addr(mmio, PF_FW_ATQBAL);
	tx_reg->len_mask	= PF_FW_ATQLEN_ATQLEN_M;
	tx_reg->len_ena_mask	= PF_FW_ATQLEN_ATQENABLE_M;
	tx_reg->head_mask	= PF_FW_ATQH_ATQH_M;

	rx_reg->head		= libie_pci_get_mmio_addr(mmio, PF_FW_ARQH);
	rx_reg->tail		= libie_pci_get_mmio_addr(mmio, PF_FW_ARQT);
	rx_reg->len		= libie_pci_get_mmio_addr(mmio, PF_FW_ARQLEN);
	rx_reg->addr_high	= libie_pci_get_mmio_addr(mmio, PF_FW_ARQBAH);
	rx_reg->addr_low	= libie_pci_get_mmio_addr(mmio, PF_FW_ARQBAL);
	rx_reg->len_mask	= PF_FW_ARQLEN_ARQLEN_M;
	rx_reg->len_ena_mask	= PF_FW_ARQLEN_ARQENABLE_M;
	rx_reg->head_mask	= PF_FW_ARQH_ARQH_M;
}

/**
 * idpf_mb_intr_reg_init - Initialize mailbox interrupt register
 * @adapter: adapter structure
 */
static void idpf_mb_intr_reg_init(struct idpf_adapter *adapter)
{
	struct libie_mmio_info *mmio = &adapter->ctlq_ctx.mmio_info;
	struct idpf_intr_reg *intr = &adapter->mb_vector.intr_reg;
	u32 dyn_ctl;

	dyn_ctl = adapter->irq_info.vectors[IDPF_MBX_IRQ_INDEX].regs.dyn_ctl;

	intr->dyn_ctl = libie_pci_get_mmio_addr(mmio, dyn_ctl);
	intr->dyn_ctl_intena_m = PF_GLINT_DYN_CTL_INTENA_M;
	intr->dyn_ctl_itridx_m = PF_GLINT_DYN_CTL_ITR_INDX_M;
	intr->icr_ena = libie_pci_get_mmio_addr(mmio, PF_INT_DIR_OICR_ENA);
	intr->icr_ena_ctlq_m = PF_INT_DIR_OICR_ENA_M;
}

/**
 * idpf_noirq_intr_reg_init - Initialize noirq registers
 * @adapter: adapter structure
 * @rsrc: to store noirq register and value
 * @idx: global software irq index used to get hardware information from
 *	 irq_info structure
 */
static void idpf_noirq_intr_reg_init(struct idpf_adapter *adapter,
				     struct idpf_q_vec_rsrc *rsrc, u16 idx)
{
	u32 val = adapter->irq_info.vectors[idx].regs.dyn_ctl;

	rsrc->noirq_dyn_ctl =
		libie_pci_get_mmio_addr(&adapter->ctlq_ctx.mmio_info, val);

	val = PF_GLINT_DYN_CTL_WB_ON_ITR_M | PF_GLINT_DYN_CTL_INTENA_MSK_M |
	      FIELD_PREP(PF_GLINT_DYN_CTL_ITR_INDX_M, IDPF_NO_ITR_UPDATE_IDX);
	rsrc->noirq_dyn_ctl_ena = val;
}

/**
 * idpf_intr_reg_init - Initialize interrupt registers
 * @adapter: adapter structure
 * @q_vector: q_vector in which the registers should be initialized
 * @idx: global software irq index used to get hardware information from
 *	 irq_info structure
 */
static void idpf_intr_reg_init(struct idpf_adapter *adapter,
			       struct idpf_q_vector *q_vector, u16 idx)
{
	struct libie_hw_vector *v = &adapter->irq_info.vectors[idx];
	struct idpf_intr_reg *intr = &q_vector->intr_reg;
	struct libie_mmio_info *mmio;
	u32 rx_itr, tx_itr;
	u32 spacing;

	mmio = &adapter->ctlq_ctx.mmio_info;

	intr->dyn_ctl = libie_pci_get_mmio_addr(mmio, v->regs.dyn_ctl);
	intr->dyn_ctl_intena_m = PF_GLINT_DYN_CTL_INTENA_M;
	intr->dyn_ctl_intena_msk_m = PF_GLINT_DYN_CTL_INTENA_MSK_M;
	intr->dyn_ctl_itridx_s = PF_GLINT_DYN_CTL_ITR_INDX_S;
	intr->dyn_ctl_intrvl_s = PF_GLINT_DYN_CTL_INTERVAL_S;
	intr->dyn_ctl_wb_on_itr_m = PF_GLINT_DYN_CTL_WB_ON_ITR_M;
	intr->dyn_ctl_swint_trig_m = PF_GLINT_DYN_CTL_SWINT_TRIG_M;
	intr->dyn_ctl_sw_itridx_ena_m =
		PF_GLINT_DYN_CTL_SW_ITR_INDX_ENA_M;

	spacing = IDPF_ITR_IDX_SPACING(v->regs.itrn_index_spacing,
				       IDPF_PF_ITR_IDX_SPACING);
	rx_itr = PF_GLINT_ITR_ADDR(VIRTCHNL2_ITR_IDX_0, v->regs.itrn,
				   spacing);
	tx_itr = PF_GLINT_ITR_ADDR(VIRTCHNL2_ITR_IDX_1, v->regs.itrn,
				   spacing);
	intr->rx_itr = libie_pci_get_mmio_addr(mmio, rx_itr);
	intr->tx_itr = libie_pci_get_mmio_addr(mmio, tx_itr);
}

/**
 * idpf_reset_reg_init - Initialize reset registers
 * @adapter: Driver specific private structure
 */
static void idpf_reset_reg_init(struct idpf_adapter *adapter)
{
	adapter->reset_reg.rstat =
		libie_pci_get_mmio_addr(&adapter->ctlq_ctx.mmio_info,
					PFGEN_RSTAT);
	adapter->reset_reg.rstat_m = PFGEN_RSTAT_PFR_STATE_M;
}

/**
 * idpf_trigger_reset - trigger reset
 * @adapter: Driver specific private structure
 * @trig_cause: Reason to trigger a reset
 */
static void idpf_trigger_reset(struct idpf_adapter *adapter,
			       enum idpf_flags __always_unused trig_cause)
{
	void __iomem *addr;

	addr = libie_pci_get_mmio_addr(&adapter->ctlq_ctx.mmio_info,
				       PFGEN_CTRL);
	writel(readl(addr) | PFGEN_CTRL_PFSWR, addr);
}

/**
 * idpf_ptp_reg_init - Initialize required registers
 * @adapter: Driver specific private structure
 *
 * Set the bits required for enabling shtime and cmd execution
 */
static void idpf_ptp_reg_init(const struct idpf_adapter *adapter)
{
	adapter->ptp->cmd.shtime_enable_mask = PF_GLTSYN_CMD_SYNC_SHTIME_EN_M;
	adapter->ptp->cmd.exec_cmd_mask = PF_GLTSYN_CMD_SYNC_EXEC_CMD_M;
}

/**
 * idpf_idc_register - register for IDC callbacks
 * @adapter: Driver specific private structure
 *
 * Return: 0 on success or error code on failure.
 */
static int idpf_idc_register(struct idpf_adapter *adapter)
{
	return idpf_idc_init_aux_core_dev(adapter, IIDC_FUNCTION_TYPE_PF);
}

/**
 * idpf_reg_ops_init - Initialize register API function pointers
 * @adapter: Driver specific private structure
 */
static void idpf_reg_ops_init(struct idpf_adapter *adapter)
{
	adapter->dev_ops.reg_ops.ctlq_reg_init = idpf_ctlq_reg_init;
	adapter->dev_ops.reg_ops.intr_reg_init = idpf_intr_reg_init;
	adapter->dev_ops.reg_ops.noirq_intr_reg_init = idpf_noirq_intr_reg_init;
	adapter->dev_ops.reg_ops.mb_intr_reg_init = idpf_mb_intr_reg_init;
	adapter->dev_ops.reg_ops.reset_reg_init = idpf_reset_reg_init;
	adapter->dev_ops.reg_ops.trigger_reset = idpf_trigger_reset;
	adapter->dev_ops.reg_ops.ptp_reg_init = idpf_ptp_reg_init;
}

/**
 * idpf_dev_ops_init - Initialize device API function pointers
 * @adapter: Driver specific private structure
 */
void idpf_dev_ops_init(struct idpf_adapter *adapter)
{
	idpf_reg_ops_init(adapter);

	adapter->dev_ops.idc_init = idpf_idc_register;

	resource_set_range(&adapter->dev_ops.static_reg_info[0],
			   PF_FW_BASE, IDPF_PF_MBX_REGION_SZ);
	resource_set_range(&adapter->dev_ops.static_reg_info[1],
			   PFGEN_RTRIG, IDPF_PF_RSTAT_REGION_SZ);
}
