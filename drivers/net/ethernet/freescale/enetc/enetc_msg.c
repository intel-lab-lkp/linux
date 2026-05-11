// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/* Copyright 2017-2019 NXP */

#include "enetc_pf_common.h"

static void enetc_msg_disable_mr_int(struct enetc_hw *hw)
{
	u32 psiier = enetc_rd(hw, ENETC_PSIIER);
	/* disable MR int source(s) */
	enetc_wr(hw, ENETC_PSIIER, psiier & ~ENETC_PSIIER_MR_MASK);
}

static void enetc_msg_enable_mr_int(struct enetc_hw *hw)
{
	u32 psiier = enetc_rd(hw, ENETC_PSIIER);

	enetc_wr(hw, ENETC_PSIIER, psiier | ENETC_PSIIER_MR_MASK);
}

static irqreturn_t enetc_msg_psi_msix(int irq, void *data)
{
	struct enetc_si *si = (struct enetc_si *)data;
	struct enetc_pf *pf = enetc_si_priv(si);

	enetc_msg_disable_mr_int(&si->hw);
	schedule_work(&pf->msg_task);

	return IRQ_HANDLED;
}

/* Messaging */
static void enetc_msg_set_vf_primary_mac_addr(struct enetc_pf *pf, int vf_id,
					      union enetc_pf_msg *pf_msg)
{
	struct enetc_vf_state *vf_state = &pf->vf_state[vf_id];
	struct enetc_msg_swbd *msg_swbd = &pf->rxmsg[vf_id];
	struct device *dev = &pf->si->pdev->dev;
	struct enetc_msg_mac_exact_filter *msg;
	char *addr;

	msg = (struct enetc_msg_mac_exact_filter *)msg_swbd->vaddr;
	addr = msg->mac[0].addr;
	if (!is_valid_ether_addr(addr)) {
		dev_err(dev, "Invalid MAC address from VSI message\n");
		pf_msg->class_id = ENETC_MSG_CLASS_ID_MAC_FILTER;
		pf_msg->class_code = ENETC_MF_CLASS_CODE_INVALID_MAC;

		return;
	}

	if (vf_state->flags & ENETC_VF_FLAG_PF_SET_MAC)
		dev_warn(dev, "Attempt to override PF set mac addr for VF%d\n",
			 vf_id);
	else
		pf->ops->set_si_primary_mac(&pf->si->hw, vf_id + 1, addr);

	pf_msg->class_id = ENETC_MSG_CLASS_ID_CMD_SUCCESS;
}

static bool enetc_msg_check_crc16(void *msg_addr, u32 msg_size)
{
	u32 data_size = msg_size - 2;
	u8 *data_buf = msg_addr + 2;
	u16 verify_val;

	if (msg_size > ENETC_DEFAULT_MSG_SIZE)
		return false;

	verify_val = crc_itu_t(ENETC_CRC_INIT, data_buf, data_size);
	verify_val = crc_itu_t(verify_val, msg_addr, 2);
	if (verify_val)
		return false;

	return true;
}

static void enetc_msg_handle_mac_filter(struct enetc_msg_header *msg_hdr,
					struct enetc_pf *pf, int vf_id,
					union enetc_pf_msg *pf_msg)
{
	switch (msg_hdr->cmd_id) {
	case ENETC_MSG_SET_PRIMARY_MAC:
		enetc_msg_set_vf_primary_mac_addr(pf, vf_id, pf_msg);
		break;
	default:
		pf_msg->class_id = ENETC_MSG_CLASS_ID_CMD_NOT_SUPPORT;
	}
}

static void enetc_msg_handle_rxmsg(struct enetc_pf *pf, int vf_id,
				   union enetc_pf_msg *pf_msg)
{
	struct enetc_msg_swbd *msg_swbd = &pf->rxmsg[vf_id];
	struct device *dev = &pf->si->pdev->dev;
	struct enetc_msg_header *msg_hdr;
	u32 msg_size;

	msg_hdr = (struct enetc_msg_header *)msg_swbd->vaddr;
	msg_size = ENETC_MSG_SIZE(msg_hdr->len);
	if (!enetc_msg_check_crc16(msg_swbd->vaddr, msg_size)) {
		dev_err(dev, "VSI to PSI Message CRC16 error\n");
		pf_msg->class_id = ENETC_MSG_CLASS_ID_CRC_ERROR;

		return;
	}

	/* Currently, asynchronous actions are not supported */
	if (msg_hdr->cookie) {
		dev_err(dev, "Cookie field is not supported yet\n");
		pf_msg->class_id = ENETC_MSG_CLASS_ID_CMD_NOT_SUPPORT;

		return;
	}

	/* Currently only support protocol version 0 */
	if (msg_hdr->proto_ver) {
		dev_err(dev, "Protocol version %u is not supported yet\n",
			msg_hdr->proto_ver);
		pf_msg->class_id = ENETC_MSG_CLASS_ID_PROTO_NOT_SUPPORT;

		return;
	}

	switch (msg_hdr->class_id) {
	case ENETC_MSG_CLASS_ID_MAC_FILTER:
		enetc_msg_handle_mac_filter(msg_hdr, pf, vf_id, pf_msg);
		break;
	default:
		pf_msg->class_id = ENETC_MSG_CLASS_ID_CMD_NOT_SUPPORT;
	}
}

static void enetc_msg_task(struct work_struct *work)
{
	struct enetc_pf *pf = container_of(work, struct enetc_pf, msg_task);
	struct enetc_hw *hw = &pf->si->hw;
	unsigned long mr_mask;
	int i;

	for (;;) {
		mr_mask = enetc_rd(hw, ENETC_PSIMSGRR) & ENETC_PSIMSGRR_MR_MASK;
		if (!mr_mask) {
			/* re-arm MR interrupts, w1c the IDR reg */
			enetc_wr(hw, ENETC_PSIIDR, ENETC_PSIIER_MR_MASK);
			enetc_msg_enable_mr_int(hw);
			return;
		}

		for (i = 0; i < pf->num_vfs; i++) {
			union enetc_pf_msg pf_msg = {};
			u32 psimsgrr;

			if (!(ENETC_PSIMSGRR_MR(i) & mr_mask))
				continue;

			enetc_msg_handle_rxmsg(pf, i, &pf_msg);

			psimsgrr = ENETC_SIMSGSR_SET_MC(pf_msg.code);
			psimsgrr |= ENETC_PSIMSGRR_MR(i); /* w1c */
			enetc_wr(hw, ENETC_PSIMSGRR, psimsgrr);
		}
	}
}

/* Init */
static int enetc_msg_alloc_mbx(struct enetc_si *si, int idx)
{
	struct enetc_pf *pf = enetc_si_priv(si);
	struct device *dev = &si->pdev->dev;
	struct enetc_hw *hw = &si->hw;
	struct enetc_msg_swbd *msg;
	u32 val;

	msg = &pf->rxmsg[idx];
	/* allocate and set receive buffer */
	msg->size = ENETC_DEFAULT_MSG_SIZE;

	msg->vaddr = dma_alloc_coherent(dev, msg->size, &msg->dma,
					GFP_KERNEL);
	if (!msg->vaddr) {
		dev_err(dev, "msg: fail to alloc dma buffer of size: %d\n",
			msg->size);
		return -ENOMEM;
	}

	/* set multiple of 32 bytes */
	val = lower_32_bits(msg->dma);
	enetc_wr(hw, ENETC_PSIVMSGRCVAR0(idx), val);
	val = upper_32_bits(msg->dma);
	enetc_wr(hw, ENETC_PSIVMSGRCVAR1(idx), val);

	return 0;
}

static void enetc_msg_free_mbx(struct enetc_si *si, int idx)
{
	struct enetc_pf *pf = enetc_si_priv(si);
	struct enetc_hw *hw = &si->hw;
	struct enetc_msg_swbd *msg;

	msg = &pf->rxmsg[idx];
	dma_free_coherent(&si->pdev->dev, msg->size, msg->vaddr, msg->dma);
	memset(msg, 0, sizeof(*msg));

	enetc_wr(hw, ENETC_PSIVMSGRCVAR0(idx), 0);
	enetc_wr(hw, ENETC_PSIVMSGRCVAR1(idx), 0);
}

static int enetc_msg_psi_init(struct enetc_pf *pf)
{
	struct enetc_si *si = pf->si;
	int vector, i, err;

	/* register message passing interrupt handler */
	snprintf(pf->msg_int_name, sizeof(pf->msg_int_name), "%s-vfmsg",
		 si->ndev->name);
	vector = pci_irq_vector(si->pdev, ENETC_SI_INT_IDX);
	err = request_irq(vector, enetc_msg_psi_msix, 0, pf->msg_int_name, si);
	if (err) {
		dev_err(&si->pdev->dev,
			"PSI messaging: request_irq() failed!\n");
		return err;
	}

	/* set one IRQ entry for PSI message receive notification (SI int) */
	enetc_wr(&si->hw, ENETC_SIMSIVR, ENETC_SI_INT_IDX);

	/* initialize PSI mailbox */
	INIT_WORK(&pf->msg_task, enetc_msg_task);

	for (i = 0; i < pf->num_vfs; i++) {
		err = enetc_msg_alloc_mbx(si, i);
		if (err)
			goto err_init_mbx;
	}

	/* enable MR interrupts */
	enetc_msg_enable_mr_int(&si->hw);

	return 0;

err_init_mbx:
	for (i--; i >= 0; i--)
		enetc_msg_free_mbx(si, i);

	free_irq(vector, si);

	return err;
}

static void enetc_msg_psi_free(struct enetc_pf *pf)
{
	struct enetc_si *si = pf->si;
	int i;

	cancel_work_sync(&pf->msg_task);

	/* disable MR interrupts */
	enetc_msg_disable_mr_int(&si->hw);

	for (i = 0; i < pf->num_vfs; i++)
		enetc_msg_free_mbx(si, i);

	/* de-register message passing interrupt handler */
	free_irq(pci_irq_vector(si->pdev, ENETC_SI_INT_IDX), si);
}

int enetc_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	struct enetc_si *si = pci_get_drvdata(pdev);
	struct enetc_pf *pf = enetc_si_priv(si);
	int err;

	if (!num_vfs) {
		pci_disable_sriov(pdev);
		enetc_msg_psi_free(pf);
		pf->num_vfs = 0;
	} else {
		pf->num_vfs = num_vfs;

		err = enetc_msg_psi_init(pf);
		if (err) {
			dev_err(&pdev->dev, "enetc_msg_psi_init (%d)\n", err);
			goto err_msg_psi;
		}

		err = pci_enable_sriov(pdev, num_vfs);
		if (err) {
			dev_err(&pdev->dev, "pci_enable_sriov err %d\n", err);
			goto err_en_sriov;
		}
	}

	return num_vfs;

err_en_sriov:
	enetc_msg_psi_free(pf);
err_msg_psi:
	pf->num_vfs = 0;

	return err;
}
EXPORT_SYMBOL_GPL(enetc_sriov_configure);
