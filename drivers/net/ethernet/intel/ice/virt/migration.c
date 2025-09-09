// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2025 Intel Corporation */

#include "ice.h"
#include "ice_lib.h"
#include "ice_fltr.h"
#include "ice_base.h"
#include "ice_txrx_lib.h"
#include "virt/migration_tlv.h"

/**
 * ice_migration_init_dev - Enable migration support for the requested VF
 * @vf_dev: pointer to the VF PCI device
 *
 * TODO: currently the vf->migration_enabled field is unused. It is likely
 * that we will need to use it to check that features which cannot migrate are
 * disabled.
 *
 * Return: 0 on success, negative error code on failure.
 */
int ice_migration_init_dev(struct pci_dev *vf_dev)
{
	struct ice_pf *pf = ice_vf_dev_to_pf(vf_dev);
	struct ice_vf *vf;

	if (IS_ERR(pf))
		return PTR_ERR(pf);

	vf = ice_get_vf_by_dev(pf, vf_dev);
	if (!vf) {
		dev_err(&vf_dev->dev, "Unable to locate VF from VF device\n");
		return -EINVAL;
	}

	mutex_lock(&vf->cfg_lock);
	vf->migration_enabled = true;
	mutex_unlock(&vf->cfg_lock);

	ice_put_vf(vf);
	return 0;
}
EXPORT_SYMBOL(ice_migration_init_dev);

/**
 * ice_migration_uninit_dev - Disable migration support for the requested VF
 * @vf_dev: pointer to the VF PCI device
 */
void ice_migration_uninit_dev(struct pci_dev *vf_dev)
{
	struct ice_pf *pf = ice_vf_dev_to_pf(vf_dev);
	struct device *dev;
	struct ice_vf *vf;

	if (IS_ERR(pf))
		return;

	vf = ice_get_vf_by_dev(pf, vf_dev);
	if (!vf) {
		dev_err(&vf_dev->dev, "Unable to locate VF from VF device\n");
		return;
	}

	dev = ice_pf_to_dev(pf);

	mutex_lock(&vf->cfg_lock);

	vf->migration_enabled = false;

	if (!list_empty(&vf->mig_tlvs)) {
		struct ice_mig_tlv_entry *entry, *tmp;

		dev_dbg(dev, "Freeing unused migration TLVs for VF %d\n",
			vf->vf_id);

		list_for_each_entry_safe(entry, tmp, &vf->mig_tlvs,
					 list_entry) {
			list_del(&entry->list_entry);
			kfree(entry);
		}
	}

	mutex_unlock(&vf->cfg_lock);

	ice_put_vf(vf);
}
EXPORT_SYMBOL(ice_migration_uninit_dev);

/**
 * ice_migration_save_vf_info - Save VF information during suspend
 * @vf: pointer to the VF being migrated
 * @vsi: pointer to the VSI for this VF
 *
 * Save the VF device information when suspending a VF for live migration.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int ice_migration_save_vf_info(struct ice_vf *vf, struct ice_vsi *vsi)
{
	struct ice_mig_vf_info *vf_info;

	lockdep_assert_held(&vf->cfg_lock);

	vf_info = ice_mig_alloc_flex_tlv(vf_info, opcodes_allowlist,
					 BITS_TO_U32(VIRTCHNL_OP_MAX));
	if (!vf_info)
		return -ENOMEM;

	vf_info->driver_caps = vf->driver_caps;
	vf_info->port_vlan_tpid = vf->port_vlan_info.tpid;
	vf_info->port_vlan_vid = vf->port_vlan_info.vid;
	vf_info->port_vlan_prio = vf->port_vlan_info.prio;
	vf_info->vlan_v2_caps = vf->vlan_v2_caps;
	vf_info->vf_ver = vf->vf_ver;
	vf_info->min_tx_rate = vf->min_tx_rate;
	vf_info->max_tx_rate = vf->max_tx_rate;
	vf_info->num_vf_qs = vf->num_vf_qs;
	vf_info->num_msix = vf->num_msix;
	vf_info->inner_vlan_strip_ena =
		vf->vlan_strip_ena & ICE_INNER_VLAN_STRIP_ENA ? 1 : 0;
	vf_info->outer_vlan_strip_ena =
		vf->vlan_strip_ena & ICE_OUTER_VLAN_STRIP_ENA ? 1 : 0;
	vf_info->pf_set_mac = vf->pf_set_mac;
	vf_info->trusted = vf->trusted;
	vf_info->spoofchk = vf->spoofchk;
	vf_info->link_forced = vf->link_forced;
	vf_info->link_up = vf->link_up;
	vf_info->driver_active = test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states);

	ether_addr_copy(vf_info->dev_lan_addr, vf->dev_lan_addr);
	ether_addr_copy(vf_info->hw_lan_addr, vf->hw_lan_addr);

	vf_info->virtchnl_op_max = VIRTCHNL_OP_MAX;
	bitmap_to_arr32(vf_info->opcodes_allowlist, vf->opcodes_allowlist,
			VIRTCHNL_OP_MAX);

	ice_mig_tlv_add_tail(vf_info, &vf->mig_tlvs);

	return 0;
}

/**
 * ice_migration_save_tx_queues - Save Tx queue state
 * @vf: pointer to the VF being migrated
 * @vsi: the VSI for this VF
 *
 * Save Tx queue state in preparation for live migration.
 *
 * Return: 0 for success, negative for error
 */
static int ice_migration_save_tx_queues(struct ice_vf *vf, struct ice_vsi *vsi)
{
	struct device *dev = ice_pf_to_dev(vf->pf);
	struct ice_mig_tlv_entry *entry, *tmp;
	struct list_head queue_tlvs;
	int err, i;

	lockdep_assert_held(&vf->cfg_lock);
	INIT_LIST_HEAD(&queue_tlvs);

	dev_dbg(dev, "Saving Tx queue config for VF %u\n",
		vf->vf_id);

	ice_for_each_txq(vsi, i) {
		struct ice_tx_ring *tx_ring = vsi->tx_rings[i];
		struct ice_mig_tx_queue *tx_queue;
		struct ice_tlan_ctx tlan_ctx = {};
		struct ice_hw *hw = &vf->pf->hw;
		u32 qtx_comm_head;
		u16 tx_head;
		int err;

		if (!tx_ring)
			continue;

		/* Ignore queues which were never configured by the VF */
		if (!tx_ring->dma) {
			dev_dbg(dev, "Ignoring unconfigured Tx queue %d on VF %d with NULL DMA address\n",
				i, vf->vf_id);
			continue;
		}

		tx_queue = ice_mig_alloc_tlv(tx_queue);
		if (!tx_queue) {
			err = -ENOMEM;
			goto err_free_tlv_entries;
		}

		err = ice_read_txq_ctx(hw, &tlan_ctx, tx_ring->reg_idx);
		if (err) {
			dev_err(dev, "Failed to read TXQ[%d] context, err=%d\n",
				tx_ring->q_index, err);
			goto err_free_tlv_entries;
		}

		qtx_comm_head = rd32(hw, QTX_COMM_HEAD(tx_ring->reg_idx));
		tx_head = FIELD_GET(QTX_COMM_HEAD_HEAD_M, qtx_comm_head);

		/* Determine the Tx head from the QTX_COMM_HEAD register.
		 *
		 * If no write back has happened since the queue was enabled,
		 * the register will read as QTX_COMM_HEAD_HEAD_M.
		 *
		 * Otherwise, the value from QTX_COMM_HEAD will be precisely
		 * one behind the real Tx head value.
		 */
		if (tx_head == QTX_COMM_HEAD_HEAD_M ||
		    tx_head == tx_ring->count - 1)
			tx_head = 0;
		else
			tx_head++;

		tx_queue->queue_id = i;
		tx_queue->dma = tx_ring->dma;
		tx_queue->count = tx_ring->count;
		tx_queue->head = tx_head;
		if (tx_ring->q_vector) {
			/* we don't need to account for ICE_NONQ_VECS_VF here,
			 * as the deserializing end won't expect it.
			 */
			tx_queue->vector_id = tx_ring->q_vector->v_idx;
			tx_queue->vector_valid = 1;
		}
		tx_queue->ena = test_bit(i, vf->txq_ena);

		ice_mig_tlv_add_tail(tx_queue, &queue_tlvs);
	}

	list_splice_tail(&queue_tlvs, &vf->mig_tlvs);

	return 0;

err_free_tlv_entries:
	list_for_each_entry_safe(entry, tmp, &queue_tlvs, list_entry) {
		list_del(&entry->list_entry);
		kfree(entry);
	}

	return err;
}

/**
 * ice_migration_save_rx_queues - Save Rx queue state
 * @vf: pointer to the VF being migrated
 * @vsi: the VSI for this VF
 *
 * Save Rx queue state in preparation for live migration.
 *
 * Return: 0 for success, negative for error
 */
static int ice_migration_save_rx_queues(struct ice_vf *vf, struct ice_vsi *vsi)
{
	struct device *dev = ice_pf_to_dev(vf->pf);
	struct ice_mig_tlv_entry *entry, *tmp;
	struct list_head queue_tlvs;
	int err, i;

	lockdep_assert_held(&vf->cfg_lock);
	INIT_LIST_HEAD(&queue_tlvs);

	dev_dbg(dev, "Saving Rx queue config for VF %u\n",
		vf->vf_id);

	ice_for_each_rxq(vsi, i) {
		struct ice_rx_ring *rx_ring = vsi->rx_rings[i];
		struct ice_mig_rx_queue *rx_queue;
		struct ice_rlan_ctx rlan_ctx = {};
		struct ice_hw *hw = &vf->pf->hw;
		u32 rxflxp;
		int err;

		if (!rx_ring)
			continue;

		/* Ignore queues which were never configured by the VF */
		if (!rx_ring->dma) {
			dev_dbg(dev, "Ignoring unconfigured Rx queue %d on VF %d with NULL DMA address\n",
				i, vf->vf_id);
			continue;
		}

		rx_queue = ice_mig_alloc_tlv(rx_queue);
		if (!rx_queue) {
			err = -ENOMEM;
			goto err_free_tlv_entries;
		}

		err = ice_read_rxq_ctx(hw, &rlan_ctx, rx_ring->reg_idx);
		if (err) {
			dev_err(dev, "Failed to read RXQ[%d] context, err=%d\n",
				rx_ring->q_index, err);
			goto err_free_tlv_entries;
		}

		rxflxp = rd32(hw, QRXFLXP_CNTXT(rx_ring->reg_idx));

		rx_queue->queue_id = i;
		rx_queue->head = rlan_ctx.head;
		rx_queue->tail = QRX_TAIL(rx_ring->reg_idx);
		rx_queue->dma = rx_ring->dma;
		rx_queue->max_frame = rlan_ctx.rxmax;
		rx_queue->rx_buf_len = rx_ring->rx_buf_len;
		rx_queue->rxdid = FIELD_GET(QRXFLXP_CNTXT_RXDID_IDX_M, rxflxp);
		rx_queue->count = rx_ring->count;
		if (rx_ring->q_vector) {
			/* we don't need to account for ICE_NONQ_VECS_VF here,
			 * as the deserializing end won't expect it.
			 */
			rx_queue->vector_id = rx_ring->q_vector->v_idx;
			rx_queue->vector_valid = 1;
		}
		rx_queue->crc_strip = rlan_ctx.crcstrip;
		rx_queue->ena = test_bit(i, vf->rxq_ena);

		ice_mig_tlv_add_tail(rx_queue, &queue_tlvs);
	}

	list_splice_tail(&queue_tlvs, &vf->mig_tlvs);

	return 0;

err_free_tlv_entries:
	list_for_each_entry_safe(entry, tmp, &queue_tlvs, list_entry) {
		list_del(&entry->list_entry);
		kfree(entry);
	}

	return err;
}

/**
 * ice_migration_save_msix_regs - Save MSI-X registers during suspend
 * @vf: pointer to the VF being migrated
 * @vsi: the VSI for this VF
 *
 * Save the MMIO registers associated with MSI-X interrupts, including the
 * miscellaneous interrupt used for the mailbox. Called during suspend to save
 * the values prior to queue shutdown, to ensure they match the VF suspended
 * state accurately.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int ice_migration_save_msix_regs(struct ice_vf *vf,
					struct ice_vsi *vsi)
{
	struct ice_mig_tlv_entry *entry, *tmp;
	struct ice_hw *hw = &vf->pf->hw;
	struct list_head msix_tlvs;
	int err;

	lockdep_assert_held(&vf->cfg_lock);
	INIT_LIST_HEAD(&msix_tlvs);

	/* Copy the IRQ registers, starting with the non-queue vectors */
	for (int idx = 0; idx < vsi->num_q_vectors + ICE_NONQ_VECS_VF; idx++) {
		struct ice_mig_msix_regs *msix_regs;
		u16 reg_idx, tx_itr_idx, rx_itr_idx;

		if (idx < ICE_NONQ_VECS_VF) {
			reg_idx = vf->first_vector_idx + idx;
			tx_itr_idx = 0;
			rx_itr_idx = 0;
		} else {
			struct ice_q_vector *q_vector;
			int v_id;

			v_id = idx - ICE_NONQ_VECS_VF;
			q_vector = vsi->q_vectors[v_id];
			reg_idx = q_vector->reg_idx;
			tx_itr_idx = q_vector->tx.itr_idx;
			rx_itr_idx = q_vector->rx.itr_idx;
		}

		msix_regs = ice_mig_alloc_tlv(msix_regs);
		if (!msix_regs) {
			err = -ENOMEM;
			goto err_free_tlv_entries;
		}

		msix_regs->vector_id = idx;
		msix_regs->tx_itr_idx = tx_itr_idx;
		msix_regs->rx_itr_idx = rx_itr_idx;

		msix_regs->int_dyn_ctl = rd32(hw, GLINT_DYN_CTL(reg_idx));
		for (int itr = 0; itr < ICE_MIG_VF_ITR_NUM; itr++)
			msix_regs->int_intr[itr] =
				rd32(hw, GLINT_ITR(itr, reg_idx));

		ice_mig_tlv_add_tail(msix_regs, &msix_tlvs);
	}

	list_splice_tail(&msix_tlvs, &vf->mig_tlvs);

	return 0;

err_free_tlv_entries:
	list_for_each_entry_safe(entry, tmp, &msix_tlvs, list_entry) {
		list_del(&entry->list_entry);
		kfree(entry);
	}

	return err;
}

/**
 * ice_migration_suspend_dev - suspend device
 * @vf_dev: pointer to the VF PCI device
 * @save_state: true if the device may be preparing for live migration
 *
 * Suspend the VF device. If save_state is set, first save any state which is
 * necessary for later migration.
 *
 * Return: 0 for success, negative for error
 */
int ice_migration_suspend_dev(struct pci_dev *vf_dev, bool save_state)
{
	struct ice_pf *pf = ice_vf_dev_to_pf(vf_dev);
	struct ice_mig_tlv_entry *entry, *tmp;
	struct ice_vsi *vsi;
	struct device *dev;
	struct ice_vf *vf;
	int err;

	if (IS_ERR(pf))
		return PTR_ERR(pf);

	vf = ice_get_vf_by_dev(pf, vf_dev);
	if (!vf) {
		dev_err(&vf_dev->dev, "Unable to locate VF from VF device\n");
		return -EINVAL;
	}

	dev = ice_pf_to_dev(pf);

	dev_dbg(dev, "Suspending VF %u in preparation for live migration\n",
		vf->vf_id);

	mutex_lock(&vf->cfg_lock);

	vsi = ice_get_vf_vsi(vf);
	if (!vsi) {
		dev_err(dev, "VF %d VSI is NULL\n", vf->vf_id);
		err = -EINVAL;
		goto err_release_cfg_lock;
	}

	if (save_state) {
		if (!list_empty(&vf->mig_tlvs)) {
			dev_dbg(dev, "Freeing unused migration TLVs for VF %d\n",
				vf->vf_id);

			list_for_each_entry_safe(entry, tmp, &vf->mig_tlvs,
						 list_entry) {
				list_del(&entry->list_entry);
				kfree(entry);
			}
		}

		err = ice_migration_save_vf_info(vf, vsi);
		if (err)
			goto err_free_mig_tlvs;

		err = ice_migration_save_msix_regs(vf, vsi);
		if (err)
			goto err_free_mig_tlvs;

	}

	/* Prevent VSI from queuing incoming packets by removing all filters */
	ice_fltr_remove_all(vsi);
	/* TODO: there's probably a better way to handle this, or it may be
	 * unnecessary
	 */
	vf->num_mac = 0;
	vsi->num_vlan = 0;

	/* MAC based filter rule is disabled at this point. Set MAC to zero
	 * to keep consistency with VF mac address info shown by ip link
	 */
	eth_zero_addr(vf->hw_lan_addr);
	eth_zero_addr(vf->dev_lan_addr);

	err = ice_vsi_stop_lan_tx_rings(vsi, ICE_NO_RESET, vf->vf_id);
	if (err)
		dev_warn(dev, "VF %d failed to stop Tx rings. Continuing live migration regardless.\n",
			 vf->vf_id);

	err = ice_vsi_stop_all_rx_rings(vsi);
	if (err)
		dev_warn(dev, "VF %d failed to stop Rx rings. Continuing live migration regardless.\n",
			 vf->vf_id);

	if (save_state) {
		/* Save queue state after stopping the queues */
		err = ice_migration_save_rx_queues(vf, vsi);
		if (err)
			goto err_free_mig_tlvs;

		err = ice_migration_save_tx_queues(vf, vsi);
		if (err)
			goto err_free_mig_tlvs;
	}

	mutex_unlock(&vf->cfg_lock);
	ice_put_vf(vf);

	return 0;

err_free_mig_tlvs:
	if (save_state) {
		list_for_each_entry_safe(entry, tmp, &vf->mig_tlvs,
					 list_entry) {
			list_del(&entry->list_entry);
			kfree(entry);
		}
	}

err_release_cfg_lock:
	mutex_unlock(&vf->cfg_lock);
	ice_put_vf(vf);
	return err;
}
EXPORT_SYMBOL(ice_migration_suspend_dev);

/**
 * ice_migration_calculate_size - Calculate the size of the migration buffer
 * @vf: pointer to the VF being migrated
 *
 * Calculate the total size required for all the TLVs used to form the
 * migration data buffer. The TLVs containing migration data are already
 * recorded and saved in the vf->mig_tlvs linked list. In addition to this, we
 * need to account for the header data and the data-end marker TLV.
 *
 * Return: the size in bytes required to store the full migration payload.
 */
static size_t ice_migration_calculate_size(struct ice_vf *vf)
{
	struct ice_mig_tlv_entry *entry;
	size_t tlv_sz, total_sz;

	lockdep_assert_held(&vf->cfg_lock);

	/* The migration data begins with a header TLV describing the format */
	total_sz = struct_size_t(struct ice_migration_tlv, data,
				 sizeof(struct ice_mig_tlv_header));

	list_for_each_entry(entry, &vf->mig_tlvs, list_entry) {
		tlv_sz = struct_size(&entry->tlv, data, entry->tlv.len);
		total_sz = size_add(total_sz, tlv_sz);
	}

	/* The end of the data is signified by an empty TLV */
	tlv_sz = struct_size_t(struct ice_migration_tlv, data, 0);
	total_sz = size_add(total_sz, tlv_sz);

	return total_sz;
}

/**
 * ice_migration_get_required_size - Request migration payload buffer size
 * @vf_dev: pointer to the VF PCI device
 *
 * Request the size required to serialize the VF migration payload. Used to
 * calculate allocation size of the migration file.
 *
 * Return: the size in bytes required to store the full migration payload, or
 * 0 if this VF is not ready to migrate.
 */
size_t ice_migration_get_required_size(struct pci_dev *vf_dev)
{
	struct ice_pf *pf = ice_vf_dev_to_pf(vf_dev);
	size_t payload_size;
	struct ice_vf *vf;

	if (IS_ERR(pf)) {
		dev_err(&vf_dev->dev, "Unable to locate PF from VF device, err=%pe\n",
			pf);
		return 0;
	}

	vf = ice_get_vf_by_dev(pf, vf_dev);
	if (!vf) {
		dev_err(&vf_dev->dev, "Unable to locate VF from VF device\n");
		return 0;
	}

	mutex_lock(&vf->cfg_lock);

	if (list_empty(&vf->mig_tlvs)) {
		dev_warn(&vf_dev->dev, "VF %d is not ready to migrate\n",
			 vf->vf_id);
		payload_size = 0;
	} else {
		payload_size = ice_migration_calculate_size(vf);
	}

	mutex_unlock(&vf->cfg_lock);

	return payload_size;
}
EXPORT_SYMBOL(ice_migration_get_required_size);

/**
 * ice_migration_insert_tlv_header - Insert TLV header into migration buffer
 * @tlv: pointer to TLV in the migration buffer
 *
 * Fill in the TLV header describing the migration format.
 *
 * Return: the full struct_size of the TLV, used to move the migration buffer
 *         pointer to the next entry.
 */
static size_t ice_migration_insert_tlv_header(struct ice_migration_tlv *tlv)
{
	struct ice_mig_tlv_header *tlv_header;

	tlv->type = ice_mig_tlv_type(tlv_header);
	tlv->len = sizeof(*tlv_header);
	tlv_header = (typeof(tlv_header))tlv->data;

	tlv_header->magic = ICE_MIG_MAGIC;
	tlv_header->version = ICE_MIG_VERSION;
	tlv_header->num_supported_tlvs = NUM_ICE_MIG_TLV;

	return struct_size(tlv, data, tlv->len);
}

/**
 * ice_migration_insert_tlv_end - Insert TLV marking end of migration data
 * @tlv: pointer to TLV in the migration buffer
 *
 * Fill in the TLV marking end of the migration buffer data.
 */
static void ice_migration_insert_tlv_end(struct ice_migration_tlv *tlv)
{
	tlv->type = ICE_MIG_TLV_END;
	tlv->len = 0;
}

/**
 * ice_migration_save_devstate - Save device state to migration buffer
 * @vf_dev: pointer to the VF PCI device
 * @buf: pointer to VF msg in migration buffer
 * @buf_sz: The size of the migration buffer.
 *
 * Serialize the saved device state to the migration buffer. It is expected
 * that buf_sz is determined by calling ice_migration_get_required_size()
 * ahead of time.
 *
 * Return: 0 for success, or a negative error code on failure.
 */
int ice_migration_save_devstate(struct pci_dev *vf_dev, void *buf,
				size_t buf_sz)
{
	struct ice_pf *pf = ice_vf_dev_to_pf(vf_dev);
	struct ice_mig_tlv_entry *entry, *tmp;
	struct ice_vsi *vsi;
	struct device *dev;
	struct ice_vf *vf;
	size_t total_sz;
	int err = 0;

	if (IS_ERR(pf))
		return PTR_ERR(pf);

	vf = ice_get_vf_by_dev(pf, vf_dev);
	if (!vf) {
		dev_err(&vf_dev->dev, "Unable to locate VF from VF device\n");
		return -EINVAL;
	}

	dev = ice_pf_to_dev(pf);

	dev_dbg(dev, "Serializing migration device state for VF %u\n",
		vf->vf_id);

	mutex_lock(&vf->cfg_lock);

	vsi = ice_get_vf_vsi(vf);
	if (!vsi) {
		dev_err(dev, "VF %d VSI is NULL\n", vf->vf_id);
		err = -EINVAL;
		goto out_release_cfg_lock;
	}

	/* Make sure we have enough space */
	total_sz = ice_migration_calculate_size(vf);
	if (total_sz > buf_sz) {
		dev_err(dev, "Insufficient buffer to store device state for VF %d. Need %zu bytes, but have only %zu bytes.\n",
			vf->vf_id, total_sz, buf_sz);
		err = -ENOBUFS;
		goto out_release_cfg_lock;
	}

	dev_dbg(dev, "Saving migration data for VF %d. Total migration payload size is %zu bytes\n",
		vf->vf_id, total_sz);

	/* 1. Insert the TLV header describing the migration format */
	buf += ice_migration_insert_tlv_header(buf);

	/* 2. Insert the TLVs prepared by suspend */
	list_for_each_entry_safe(entry, tmp, &vf->mig_tlvs, list_entry) {
		size_t tlv_sz = struct_size(&entry->tlv, data, entry->tlv.len);

		memcpy(buf, &entry->tlv, tlv_sz);
		buf += tlv_sz;

		list_del(&entry->list_entry);
		kfree(entry);
	}

	/* 3. Insert TLV marking the end of the data */
	ice_migration_insert_tlv_end(buf);

out_release_cfg_lock:
	mutex_unlock(&vf->cfg_lock);
	ice_put_vf(vf);

	return err;
}
EXPORT_SYMBOL(ice_migration_save_devstate);

/**
 * ice_migration_check_tlv_size - Validate size of next TLV in buffer
 * @dev: device structure
 * @tlv: pointer to the next TLV in migration buffer
 * @sz_remaining: number of bytes left in migration buffer
 *
 * Check that the migration buffer has sufficient space to completely hold the
 * TLV, and that its length is properly aligned.
 *
 * Note that the tlv variable points into the migration buffer. To avoid
 * a read-overflow, special care is taken to validate the size of the buffer
 * before accessing the contents of the tlv variable.
 *
 * Return: 0 if there is sufficient space for the entire TLV in the migration
 *         buffer. -ENOSPC otherwise.
 */
static int ice_migration_check_tlv_size(struct device *dev,
					const struct ice_migration_tlv *tlv,
					size_t sz_remaining)
{
	/* Make sure we have enough space for the TLV */
	if (sz_remaining < sizeof(*tlv)) {
		dev_dbg(dev, "Not enough space in buffer for TLV header. Need %zu bytes, but only %zu bytes remain.\n",
			sizeof(*tlv), sz_remaining);
		return -ENOSPC;
	}

	sz_remaining -= sizeof(*tlv);

	/* Data lengths must be 4-byte aligned to ensure TLV header positions
	 * are always 4-byte aligned.
	 */
	if (tlv->len != ALIGN(tlv->len, 4)) {
		dev_dbg(dev, "TLV of type %u has unaligned length of %u bytes\n",
			tlv->type, tlv->len);
		return -ENOSPC;
	}

	if (sz_remaining < tlv->len) {
		dev_dbg(dev, "Not enough space in buffer for TLV of type %u, with length %u. Only %zu bytes remain.\n",
			tlv->type, tlv->len, sz_remaining);
		return -ENOSPC;
	}

	return 0;
}

/**
 * ice_migration_validate_tlvs - Validate TLV data integrity and compatibility
 * @dev: pointer to device
 * @buf: pointer to device state buffer
 * @buf_sz: size of buffer
 * @vf_info: on return, pointer to the VF info TLV
 *
 * Ensure that the TLV data provided is valid, and matches the expected
 * version and format.
 *
 * Return: 0 for success, negative for error
 */
static int
ice_migration_validate_tlvs(struct device *dev, const void *buf, size_t buf_sz,
			    const struct ice_mig_vf_info **vf_info)
{
	const struct ice_mig_tlv_header *header;
	const struct ice_migration_tlv *tlv;
	size_t tlv_size;
	int err;

	tlv = buf;

	dev_dbg(dev, "Validating TLVs in migration payload of size %zu\n",
		buf_sz);

	err = ice_migration_check_tlv_size(dev, tlv, buf_sz);
	if (err)
		return err;

	if (tlv->type != ICE_MIG_TLV_HEADER) {
		dev_dbg(dev, "First TLV in migration payload must be the header\n");
		return -EBADMSG;
	}

	header = (typeof(header))tlv->data;

	if (header->magic != ICE_MIG_MAGIC) {
		dev_dbg(dev, "Got magic value 0x%08x, expected 0x%08x\n",
			header->magic, ICE_MIG_MAGIC);
		return -EPROTONOSUPPORT;
	}

	if (header->version != ICE_MIG_VERSION) {
		dev_dbg(dev, "Got migration version %d, expected version %d\n",
			header->version, ICE_MIG_VERSION);
		return -EPROTONOSUPPORT;
	}

	*vf_info = NULL;

	/* Validate remaining TLVs */
	do {
		/* Move to next TLV */
		tlv_size = struct_size(tlv, data, tlv->len);
		buf_sz -= tlv_size;
		tlv = (const void *)tlv + tlv_size;

		/* Check buffer for space before dereferencing */
		err = ice_migration_check_tlv_size(dev, tlv, buf_sz);
		if (err)
			return err;

		/* Stop if we reach the end */
		if (tlv->type == ICE_MIG_TLV_END)
			break;

		if (tlv->type >= NUM_ICE_MIG_TLV ||
		    tlv->type >= header->num_supported_tlvs) {
			dev_dbg(dev, "Unsupported TLV of type %d in migration payload\n",
				tlv->type);
			return -EPROTONOSUPPORT;
		}

		/* TODO: implement other validation? Check for compatibility
		 * with queue sizes, vector counts, VLAN capabilities, etc?
		 */

		/* Save the VF info pointer, as we must process it first */
		if (tlv->type == ICE_MIG_TLV_VF_INFO)
			*vf_info = (typeof(*vf_info))tlv->data;

	} while (buf_sz > 0);

	if (!*vf_info) {
		dev_dbg(dev, "Missing VF information TLV in migration payload\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * ice_migration_load_vf_info - Load VF information from migration buffer
 * @vf: pointer to the VF being migrated to
 * @vsi: the VSI for this VF
 * @vf_info: VF information from the migration buffer
 *
 * Load the VF information from the migration buffer, preparing the VF to
 * complete migration.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
static int ice_migration_load_vf_info(struct ice_vf *vf, struct ice_vsi *vsi,
				      const struct ice_mig_vf_info *vf_info)
{
	struct device *dev = ice_pf_to_dev(vf->pf);
	int err;

	lockdep_assert_held(&vf->cfg_lock);

	dev_dbg(dev, "Loading general VF configuration for VF %u\n",
		vf->vf_id);

	dev_dbg(dev, "VF %d had %u MSI-X vectors. Requesting %u vectors\n",
		vf->vf_id, vf->num_msix, vf_info->num_msix);

	/* Change the number of MSI-X vectors first */
	// TODO: ice_sriov_set_msix_vec_count sets the MSI-X to 1 more than
	// the value passed in. This should be fixed.
	err = ice_sriov_set_msix_vec_count(vf->vfdev, vf_info->num_msix - ICE_NONQ_VECS_VF);
	if (err) {
		dev_dbg(dev, "Unable to reconfigure MSI-X vectors, err %d\n",
			err);
		return err;
	}

	/* Set values which are configured by VF reset */
	vf->trusted = vf_info->trusted;
	vf->num_req_qs = vf_info->num_vf_qs;
	vf->port_vlan_info.tpid = vf_info->port_vlan_tpid;
	vf->port_vlan_info.vid = vf_info->port_vlan_vid;
	vf->port_vlan_info.prio = vf_info->port_vlan_prio;
	vf->min_tx_rate = vf_info->min_tx_rate;
	vf->max_tx_rate = vf_info->max_tx_rate;
	vf->spoofchk = vf_info->spoofchk;

	ether_addr_copy(vf->dev_lan_addr, vf_info->dev_lan_addr);
	ether_addr_copy(vf->hw_lan_addr, vf_info->hw_lan_addr);

	/* Reset the VF */
	ice_reset_vf(vf, 0);

	/* Configure the rest of the settings */
	vf->vlan_v2_caps = vf_info->vlan_v2_caps;
	vf->vf_ver = vf_info->vf_ver;
	vf->driver_caps = vf_info->driver_caps;

	if (vf_info->inner_vlan_strip_ena) {
		err = vsi->inner_vlan_ops.ena_stripping(vsi, ETH_P_8021Q);
		if (err) {
			dev_dbg(dev, "Failed to enable inner VLAN stripping, err %d\n",
				err);
			return err;
		}
		vf->vlan_strip_ena |= ICE_INNER_VLAN_STRIP_ENA;
	} else {
		err = vsi->inner_vlan_ops.dis_stripping(vsi);
		if (err) {
			dev_dbg(dev, "Failed to enable inner VLAN stripping, err %d\n",
				err);
			return err;
		}
		vf->vlan_strip_ena &= ~ICE_INNER_VLAN_STRIP_ENA;
	}

	if (vf_info->outer_vlan_strip_ena) {
		enum ice_l2tsel l2tsel =
			ICE_L2TSEL_EXTRACT_FIRST_TAG_L2TAG2_2ND;

		err = vsi->outer_vlan_ops.ena_stripping(vsi, ETH_P_8021Q);
		if (err) {
			dev_dbg(dev, "Failed to enable outer VLAN stripping, err %d\n",
				err);
			return err;
		}
		ice_vsi_update_l2tsel(vsi, l2tsel);
		vf->vlan_strip_ena |= ICE_OUTER_VLAN_STRIP_ENA;
	} else {
		enum ice_l2tsel l2tsel =
			ICE_L2TSEL_EXTRACT_FIRST_TAG_L2TAG1;

		err = vsi->outer_vlan_ops.dis_stripping(vsi);
		if (err) {
			dev_dbg(dev, "Failed to enable outer VLAN stripping, err %d\n",
				err);
			return err;
		}
		ice_vsi_update_l2tsel(vsi, l2tsel);
		vf->vlan_strip_ena &= ~ICE_OUTER_VLAN_STRIP_ENA;
	}

	vf->pf_set_mac = vf_info->pf_set_mac;
	vf->link_forced = vf_info->link_forced;
	vf->link_up = vf_info->link_up;

	/* TODO: should we just enforce that virtchnl_op_max matches
	 * VIRTCHNL_OP_MAX?
	 */
	bitmap_from_arr32(vf->opcodes_allowlist, vf_info->opcodes_allowlist,
			  min(VIRTCHNL_OP_MAX, vf_info->virtchnl_op_max));

	/* Disallow any ops the original VF didn't recognize */
	if (vf_info->virtchnl_op_max < VIRTCHNL_OP_MAX)
		bitmap_clear(vf->opcodes_allowlist,
			     vf_info->virtchnl_op_max,
			     VIRTCHNL_OP_MAX - vf_info->virtchnl_op_max);

	if (vf_info->driver_active)
		set_bit(ICE_VF_STATE_ACTIVE, vf->vf_states);

	return 0;
}

/**
 * ice_migration_init_dummy_desc - Initialize DMA for the dummy descriptors
 * @tx_desc: Tx ring descriptor array
 * @len: length of the descriptor array
 * @tx_pkt_dma: dummy packet DMA memory
 *
 * Initialize the dummy ring data descriptors using the provided DMA for
 * packet data memory.
 */
static void ice_migration_init_dummy_desc(struct ice_tx_desc *tx_desc,
					  u16 len, dma_addr_t tx_pkt_dma)
{
	for (int i = 0; i < len; i++) {
		u32 td_cmd;

		td_cmd = ICE_TXD_LAST_DESC_CMD | ICE_TX_DESC_CMD_DUMMY;
		tx_desc[i].cmd_type_offset_bsz =
				ice_build_ctob(td_cmd, 0, SZ_256, 0);
		tx_desc[i].buf_addr = cpu_to_le64(tx_pkt_dma);
	}
}

/**
 * ice_migration_wait_for_tx_completion - Wait for Tx transmission completion
 * @hw: pointer to the device HW structure
 * @tx_ring: Tx ring structure
 * @head: target Tx head position
 *
 * Wait for hardware to complete updating the Tx ring head. We read this value
 * from QTX_COMM_HEAD. This will either be the initially programmed
 * QTX_COMM_HEAD_HEAD_M marker value, or one before the actual head of the Tx
 * ring.
 *
 * Since we only inject packets when the head needs to move from zero, the
 * target head position will always be non-zero.
 *
 * Return: 0 for success, negative for error.
 */
static int
ice_migration_wait_for_tx_completion(struct ice_hw *hw,
				     struct ice_tx_ring *tx_ring, u16 head)
{
	u32 tx_head;
	int err;

	err = rd32_poll_timeout(hw, QTX_COMM_HEAD(tx_ring->reg_idx),
				tx_head,
				FIELD_GET(QTX_COMM_HEAD_HEAD_M, tx_head) == head - 1,
				10, 500);
	if (err) {
		dev_dbg(ice_hw_to_dev(hw), "Timed out waiting for Tx ring completion, target head %u, qtx_comm_head %u, err %d\n",
			head, tx_head, err);
		return err;
	}

	return 0;
}

/**
 * ice_migration_inject_dummy_desc - Inject dummy descriptors to move Tx head
 * @vf: pointer to the VF being migrated to
 * @tx_ring: Tx ring instance
 * @head: Tx head to be loaded
 * @tx_desc_dma: Tx descriptor ring base DMG address
 *
 * Load the Tx head for the given Tx ring using the following steps:
 *
 * 1. Initialize QTX_COMM_HEAD to marker value.
 * 2. Backup the current Tx context.
 * 3. Temporarily update the Tx context to point to the PF space, using the
 *    provided PF Tx descriptor DMA, filled with dummy descriptors and packet
 *    data.
 * 4. Disable the Tx queue interrupt.
 * 5. Bump the Tx ring doorbell to the desired Tx head position.
 * 6. Wait for hardware to DMA and update Tx head.
 *    and update the Tx head.
 * 7. Restore the backed up Tx queue context.
 * 8. Re-enable the Tx queue interrupt.
 *
 * By updating the queue context to point to the PF space with the PF-managed
 * DMA address, the HW will issue PCI upstream memory transactions tagged by
 * the PF BDF. This will work successfully to update the Tx head without
 * needing to interact with the VF DMA.
 *
 * Return: 0 for success, negative for error.
 */
static int
ice_migration_inject_dummy_desc(struct ice_vf *vf, struct ice_tx_ring *tx_ring,
				u16 head, dma_addr_t tx_desc_dma)
{
	struct ice_tlan_ctx tlan_ctx, tlan_ctx_orig;
	struct device *dev = ice_pf_to_dev(vf->pf);
	struct ice_hw *hw = &vf->pf->hw;
	u32 dynctl;
	u32 tqctl;
	int err;

	/* 1. Initialize head after re-programming the queue */
	wr32(hw, QTX_COMM_HEAD(tx_ring->reg_idx), QTX_COMM_HEAD_HEAD_M);

	/* 2. Backup Tx Queue context */
	err = ice_read_txq_ctx(hw, &tlan_ctx, tx_ring->reg_idx);
	if (err) {
		dev_err(dev, "Failed to read TXQ[%d] context, err=%d\n",
			tx_ring->q_index, err);
		return -EIO;
	}
	memcpy(&tlan_ctx_orig, &tlan_ctx, sizeof(tlan_ctx));
	tqctl = rd32(hw, QINT_TQCTL(tx_ring->reg_idx));
	if (tx_ring->q_vector)
		dynctl = rd32(hw, GLINT_DYN_CTL(tx_ring->q_vector->reg_idx));

	/* 3. Switch Tx queue context as PF space and PF DMA ring base. */
	tlan_ctx.vmvf_type = ICE_TLAN_CTX_VMVF_TYPE_PF;
	tlan_ctx.vmvf_num = 0;
	tlan_ctx.base = tx_desc_dma >> ICE_TLAN_CTX_BASE_S;
	err = ice_write_txq_ctx(hw, &tlan_ctx, tx_ring->reg_idx);
	if (err) {
		dev_err(dev, "Failed to write TXQ[%d] context, err=%d\n",
			tx_ring->q_index, err);
		return -EIO;
	}

	/* 4. Disable Tx queue interrupt. */
	wr32(hw, QINT_TQCTL(tx_ring->reg_idx), QINT_TQCTL_ITR_INDX_M);

	/* To disable Tx queue interrupt during run time, software should
	 * write mmio to trigger a MSIX interrupt.
	 */
	if (tx_ring->q_vector)
		wr32(hw, GLINT_DYN_CTL(tx_ring->q_vector->reg_idx),
		     (ICE_ITR_NONE << GLINT_DYN_CTL_ITR_INDX_S) |
		     GLINT_DYN_CTL_SWINT_TRIG_M |
		     GLINT_DYN_CTL_INTENA_M);

	/* Force memory writes to complete before letting h/w know there
	 * are new descriptors to fetch.
	 */
	wmb();

	/* 5. Bump doorbell to advance Tx Queue head */
	writel(head, tx_ring->tail);

	/* 6. Wait until Tx Queue head move to expected place */
	err = ice_migration_wait_for_tx_completion(hw, tx_ring, head);
	if (err) {
		dev_err(dev, "VF %d txq[%d] head loading timeout\n",
			vf->vf_id, tx_ring->q_index);
		return err;
	}

	/* 7. Overwrite Tx Queue context with backup context */
	err = ice_write_txq_ctx(hw, &tlan_ctx_orig, tx_ring->reg_idx);
	if (err) {
		dev_err(dev, "Failed to write TXQ[%d] context, err=%d\n",
			tx_ring->q_index, err);
		return -EIO;
	}

	/* 8. Re-enable Tx queue interrupt */
	wr32(hw, QINT_TQCTL(tx_ring->reg_idx), tqctl);
	if (tx_ring->q_vector)
		wr32(hw, GLINT_DYN_CTL(tx_ring->q_vector->reg_idx), dynctl);

	return 0;
}

/**
 * ice_migration_load_tx_queue - Load Tx queue data from migration payload
 * @vf: pointer to the VF being migrated to
 * @vsi: the VSI for this VF
 * @tx_queue: Tx queue data from migration payload
 * @tx_desc: temporary descriptor for moving Tx head
 * @tx_desc_dma: temporary descriptor DMA for moving Tx head
 * @tx_pkt_dma: temporary packet DMA for moving Tx head
 *
 * Load the Tx queue information from the migration buffer into the target VF.
 *
 * Return: 0 for success, negative for error
 */
static int ice_migration_load_tx_queue(struct ice_vf *vf, struct ice_vsi *vsi,
				       const struct ice_mig_tx_queue *tx_queue,
				       struct ice_tx_desc *tx_desc,
				       dma_addr_t tx_desc_dma,
				       dma_addr_t tx_pkt_dma)
{
	struct device *dev = ice_pf_to_dev(vf->pf);
	struct ice_q_vector *q_vector;
	struct ice_tx_ring *tx_ring;
	int err;

	lockdep_assert_held(&vf->cfg_lock);

	if (tx_queue->queue_id >= vsi->num_txq) {
		dev_dbg(dev, "Got data for queue %d but the VF is only configured with %d Tx queues\n",
			tx_queue->queue_id, vsi->num_txq);
		return -EINVAL;
	}

	dev_dbg(dev, "Loading Tx VF queue %d (PF queue %d) on VF %d\n",
		tx_queue->queue_id, vsi->txq_map[tx_queue->queue_id],
		vf->vf_id);

	tx_ring = vsi->tx_rings[tx_queue->queue_id];

	if (WARN_ON_ONCE(!tx_ring))
		return -EINVAL;

	tx_ring->dma = tx_queue->dma;
	tx_ring->count = tx_queue->count;

	/* Disable any existing queue first */
	err = ice_vf_vsi_dis_single_txq(vf, vsi, tx_queue->queue_id);
	if (err) {
		dev_dbg(dev, "Failed to disable existing queue, err %d\n",
			err);
		return err;
	}

	err = ice_vsi_cfg_single_txq(vsi, vsi->tx_rings, tx_queue->queue_id);
	if (err) {
		dev_dbg(dev, "Failed to configure Tx queue %u, err %d\n",
			tx_queue->queue_id, err);
		return err;
	}

	if (tx_queue->head >= tx_ring->count) {
		dev_err(dev, "VF %d: invalid tx ring length to load\n",
			vf->vf_id);
		return -EINVAL;
	}

	/* After the initial reset and Tx queue re-programming, the Tx head
	 * and tail state will be zero. If the desired state for the head is
	 * non-zero, we need to inject some dummy packets into the queue to
	 * move the head of the ring to the desired value.
	 */
	if (tx_queue->head) {
		ice_migration_init_dummy_desc(tx_desc, ICE_MAX_NUM_DESC,
					      tx_pkt_dma);
		err = ice_migration_inject_dummy_desc(vf, tx_ring,
						      tx_queue->head,
						      tx_desc_dma);
		if (err)
			return err;
	}

	if (tx_queue->vector_valid) {
		q_vector = vsi->q_vectors[tx_queue->vector_id];
		ice_cfg_txq_interrupt(vsi, tx_queue->queue_id,
				      q_vector->vf_reg_idx,
				      q_vector->tx.itr_idx);
	}

	if (tx_queue->ena) {
		ice_vf_ena_txq_interrupt(vsi, tx_queue->queue_id);
		set_bit(tx_queue->queue_id, vf->txq_ena);
	}

	return 0;
}

/**
 * ice_migration_load_rx_queue - Load Rx queue data from migration buffer
 * @vf: pointer to the VF being migrated to
 * @vsi: pointer to the VSI for the VF
 * @rx_queue: pointer to Rx queue migration data
 *
 * Load the Rx queue data from the migration payload into the target VF.
 *
 * Return: 0 for success, negative for error
 */
static int ice_migration_load_rx_queue(struct ice_vf *vf, struct ice_vsi *vsi,
				       const struct ice_mig_rx_queue *rx_queue)
{
	struct device *dev = ice_pf_to_dev(vf->pf);
	struct ice_rlan_ctx rlan_ctx = {};
	struct ice_hw *hw = &vf->pf->hw;
	struct ice_q_vector *q_vector;
	struct ice_rx_ring *rx_ring;
	int err;

	lockdep_assert_held(&vf->cfg_lock);

	if (rx_queue->queue_id >= vsi->num_rxq) {
		dev_dbg(dev, "Got data for queue %d but the VF is only configured with %d Rx queues\n",
			rx_queue->queue_id, vsi->num_rxq);
		return -EINVAL;
	}

	dev_dbg(dev, "Loading Rx queue %d on VF %d\n",
		rx_queue->queue_id, vf->vf_id);

	if (!(BIT(rx_queue->rxdid) & vf->pf->supported_rxdids)) {
		dev_dbg(dev, "Got unsupported Rx descriptor ID %u\n",
			rx_queue->rxdid);
		return -EINVAL;
	}

	rx_ring = vsi->rx_rings[rx_queue->queue_id];

	if (WARN_ON_ONCE(!rx_ring))
		return -EINVAL;

	rx_ring->dma = rx_queue->dma;
	rx_ring->count = rx_queue->count;

	if (rx_queue->crc_strip)
		rx_ring->flags &= ~ICE_RX_FLAGS_CRC_STRIP_DIS;
	else
		rx_ring->flags |= ICE_RX_FLAGS_CRC_STRIP_DIS;

	rx_ring->rx_buf_len = rx_queue->rx_buf_len;
	rx_ring->max_frame = rx_queue->max_frame;

	err = ice_vsi_cfg_single_rxq(vsi, rx_queue->queue_id);
	if (err) {
		dev_dbg(dev, "Failed to configure Rx queue %u for VF %u, err %d\n",
			rx_queue->queue_id, vf->vf_id, err);
		return err;
	}

	ice_write_qrxflxp_cntxt(hw, rx_ring->reg_idx,
				rx_queue->rxdid, 0x03, false);

	err = ice_read_rxq_ctx(hw, &rlan_ctx, rx_ring->reg_idx);
	if (err) {
		dev_err(dev, "Failed to read RXQ[%d] context, err=%d\n",
			rx_ring->q_index, err);
		return -EIO;
	}

	rlan_ctx.head = rx_queue->head;
	err = ice_write_rxq_ctx(hw, &rlan_ctx, rx_ring->reg_idx);
	if (err) {
		dev_err(dev, "Failed to set LAN RXQ[%d] context, err=%d\n",
			rx_ring->q_index, err);
		return -EIO;
	}

	wr32(hw, QRX_TAIL(rx_ring->reg_idx), rx_queue->tail);

	if (rx_queue->vector_valid) {
		q_vector = vsi->q_vectors[rx_queue->vector_id];
		ice_cfg_rxq_interrupt(vsi, rx_queue->queue_id,
				      q_vector->vf_reg_idx,
				      q_vector->rx.itr_idx);
	}

	if (rx_queue->ena) {
		err = ice_vsi_ctrl_one_rx_ring(vsi, true, rx_queue->queue_id,
					       true);
		if (err) {
			dev_err(dev, "Failed to enable Rx ring %d on VSI %d, err %d\n",
				rx_queue->queue_id, vsi->vsi_num, err);
			return -EIO;
		}

		ice_vf_ena_rxq_interrupt(vsi, rx_queue->queue_id);
		set_bit(rx_queue->queue_id, vf->rxq_ena);
	}

	return 0;
}

/**
 * ice_migration_load_msix_regs - Load MSI-X vector registers
 * @vf: pointer to the VF being migrated to
 * @vsi: the VSI of the target VF
 * @msix_regs: MSI-X register data from migration payload
 *
 * Load the MSI-X vector register data from the migration payload into the
 * target VF.
 *
 * Return: 0 for success, negative for error
 */
static int
ice_migration_load_msix_regs(struct ice_vf *vf, struct ice_vsi *vsi,
			     const struct ice_mig_msix_regs *msix_regs)
{
	struct device *dev = ice_pf_to_dev(vf->pf);
	struct ice_hw *hw = &vf->pf->hw;
	u16 reg_idx;
	int itr;

	lockdep_assert_held(&vf->cfg_lock);

	if (msix_regs->vector_id > vsi->num_q_vectors + ICE_NONQ_VECS_VF) {
		dev_dbg(dev, "Got data for MSI-X vector %d, but the VF is only configured with %d vectors\n",
			msix_regs->vector_id,
			vsi->num_q_vectors + ICE_NONQ_VECS_VF);
		return -EINVAL;
	}

	dev_dbg(dev, "Loading MSI-X register configuration for VF %u\n",
		vf->vf_id);

	if (msix_regs->vector_id < ICE_NONQ_VECS_VF) {
		reg_idx = vf->first_vector_idx + msix_regs->vector_id;
	} else {
		struct ice_q_vector *q_vector;
		int v_id;

		v_id = msix_regs->vector_id - ICE_NONQ_VECS_VF;
		q_vector = vsi->q_vectors[v_id];
		reg_idx = q_vector->reg_idx;

		q_vector->tx.itr_idx = msix_regs->tx_itr_idx;
		q_vector->rx.itr_idx = msix_regs->rx_itr_idx;
	}

	wr32(hw, GLINT_DYN_CTL(reg_idx), msix_regs->int_dyn_ctl);
	for (itr = 0; itr < ICE_MIG_VF_ITR_NUM; itr++)
		wr32(hw, GLINT_ITR(itr, reg_idx), msix_regs->int_intr[itr]);

	return 0;
}

/**
 * ice_migration_load_devstate - Load device state into the target VF
 * @vf_dev: pointer to the VF PCI device
 * @buf: pointer to device state buf in migration buffer
 * @buf_sz: size of migration buffer
 *
 * Deserialize the migration buffer TLVs and program the target VF in the
 * destination VM to match.
 *
 * Return: 0 on success, or e negative error code on failure.
 */
int ice_migration_load_devstate(struct pci_dev *vf_dev, const void *buf,
				size_t buf_sz)
{
	const size_t dma_size = ICE_MAX_NUM_DESC * sizeof(struct ice_tx_desc);
	struct ice_pf *pf = ice_vf_dev_to_pf(vf_dev);
	const struct ice_mig_vf_info *vf_info;
	const struct ice_migration_tlv *tlv;
	dma_addr_t tx_desc_dma, tx_pkt_dma;
	void *tx_desc, *tx_pkt;
	struct ice_vsi *vsi;
	struct device *dev;
	struct ice_vf *vf;
	int err;

	if (!buf)
		return -EINVAL;

	if (IS_ERR(pf))
		return PTR_ERR(pf);

	dev = ice_pf_to_dev(pf);

	dev_dbg(&vf_dev->dev, "Loading live migration state. Migration buffer is %zu bytes\n",
		buf_sz);

	err = ice_migration_validate_tlvs(dev, buf, buf_sz, &vf_info);
	if (err)
		return err;

	vf = ice_get_vf_by_dev(pf, vf_dev);
	if (!vf) {
		dev_err(dev, "Unable to locate VF from VF device\n");
		return -EINVAL;
	}

	/* Allocate DMA ring and descriptor by PF */
	tx_desc = dma_alloc_coherent(dev, dma_size, &tx_desc_dma, GFP_KERNEL);
	if (!tx_desc)
		return -ENOMEM;

	tx_pkt = dma_alloc_coherent(dev, SZ_4K, &tx_pkt_dma, GFP_KERNEL);
	if (!tx_pkt)
		goto err_free_tx_desc_dma;

	mutex_lock(&vf->cfg_lock);

	vsi = ice_get_vf_vsi(vf);
	if (!vsi) {
		dev_err(dev, "VF %d VSI is NULL\n", vf->vf_id);
		err = -EINVAL;
		goto err_release_cfg_lock;
	}

	err = ice_migration_load_vf_info(vf, vsi, vf_info);
	if (err) {
		dev_dbg(dev, "Failed to load initial VF information, err %d\n",
			err);
		goto err_release_cfg_lock;
	}

	/* Iterate over TLVs and process migration data */
	tlv = buf;

	do {
		const void *data = tlv->data;
		size_t tlv_size;

		switch (tlv->type) {
		case ICE_MIG_TLV_END:
		case ICE_MIG_TLV_HEADER:
		case ICE_MIG_TLV_VF_INFO:
			/* These are already handled above */
			break;
		case ICE_MIG_TLV_TX_QUEUE:
			err = ice_migration_load_tx_queue(vf, vsi, data,
							  tx_desc,
							  tx_desc_dma,
							  tx_pkt_dma);
			break;
		case ICE_MIG_TLV_RX_QUEUE:
			err = ice_migration_load_rx_queue(vf, vsi, data);
			break;
		case ICE_MIG_TLV_MSIX_REGS:
			err = ice_migration_load_msix_regs(vf, vsi, data);
			break;
		default:
			dev_dbg(dev, "Unexpected TLV %d in payload?\n",
				tlv->type);
			err = -EINVAL;
		}

		if (err) {
			dev_dbg(dev, "Failed to load TLV data for TLV of type %d, err %d\n",
				tlv->type, err);
			goto err_release_cfg_lock;
		}

		tlv_size = struct_size(tlv, data, tlv->len);
		tlv = (const void *)tlv + tlv_size;
	} while (tlv->type != ICE_MIG_TLV_END);

	mutex_unlock(&vf->cfg_lock);

	ice_put_vf(vf);

	dma_free_coherent(dev, SZ_4K, tx_pkt, tx_pkt_dma);
	dma_free_coherent(dev, dma_size, tx_desc, tx_desc_dma);

	return 0;

err_release_cfg_lock:
	mutex_unlock(&vf->cfg_lock);
	ice_put_vf(vf);

	dma_free_coherent(dev, SZ_4K, tx_pkt, tx_pkt_dma);
err_free_tx_desc_dma:
	dma_free_coherent(dev, dma_size, tx_desc, tx_desc_dma);

	return err;
}
EXPORT_SYMBOL(ice_migration_load_devstate);
