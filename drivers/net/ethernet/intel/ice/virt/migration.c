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
 *
 * Ensure that the TLV data provided is valid, and matches the expected
 * version and format.
 *
 * Return: 0 for success, negative for error
 */
static int
ice_migration_validate_tlvs(struct device *dev, const void *buf, size_t buf_sz)
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
	} while (buf_sz > 0);

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
	struct ice_pf *pf = ice_vf_dev_to_pf(vf_dev);
	const struct ice_migration_tlv *tlv;
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

	err = ice_migration_validate_tlvs(dev, buf, buf_sz);
	if (err)
		return err;

	vf = ice_get_vf_by_dev(pf, vf_dev);
	if (!vf) {
		dev_err(dev, "Unable to locate VF from VF device\n");
		return -EINVAL;
	}

	mutex_lock(&vf->cfg_lock);

	vsi = ice_get_vf_vsi(vf);
	if (!vsi) {
		dev_err(dev, "VF %d VSI is NULL\n", vf->vf_id);
		err = -EINVAL;
		goto err_release_cfg_lock;
	}

	/* Iterate over TLVs and process migration data */
	tlv = buf;

	do {
		size_t tlv_size;

		switch (tlv->type) {
		case ICE_MIG_TLV_END:
		case ICE_MIG_TLV_HEADER:
			/* These are already handled above */
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

	return 0;

err_release_cfg_lock:
	mutex_unlock(&vf->cfg_lock);
	ice_put_vf(vf);

	return err;
}
EXPORT_SYMBOL(ice_migration_load_devstate);
