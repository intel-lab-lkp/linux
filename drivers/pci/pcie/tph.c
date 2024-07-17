// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *     Eric Van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */

#define pr_fmt(fmt) "TPH: " fmt
#define dev_fmt pr_fmt

#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/bitfield.h>
#include <linux/msi.h>
#include <linux/pci-tph.h>

#include "../pci.h"

/*
 * The st_info struct defines the steering tag returned by the firmware _DSM
 * method defined in PCI Firmware Spec r3.3, sect 4.6.15 "_DSM to Query Cache
 * Locality TPH Features"
 *
 * @vm_st_valid:  8 bit tag for volatile memory is valid
 * @vm_xst_valid: 16 bit tag for volatile memory is valid
 * @vm_ignore:    1 => was and will be ignored, 0 => ph should be supplied
 * @vm_st:        8 bit steering tag for volatile mem
 * @vm_xst:       16 bit steering tag for volatile mem
 * @pm_st_valid:  8 bit tag for persistent memory is valid
 * @pm_xst_valid: 16 bit tag for persistent memory is valid
 * @pm_ph_ignore: 1 => was and will be ignore, 0 => ph should be supplied
 * @pm_st:        8 bit steering tag for persistent mem
 * @pm_xst:       16 bit steering tag for persistent mem
 */
union st_info {
	struct {
		u64 vm_st_valid : 1;
		u64 vm_xst_valid : 1;
		u64 vm_ph_ignore : 1;
		u64 rsvd1 : 5;
		u64 vm_st : 8;
		u64 vm_xst : 16;
		u64 pm_st_valid : 1;
		u64 pm_xst_valid : 1;
		u64 pm_ph_ignore : 1;
		u64 rsvd2 : 5;
		u64 pm_st : 8;
		u64 pm_xst : 16;
	};
	u64 value;
};

static u16 tph_extract_tag(enum tph_mem_type mem_type, u8 req_type,
			   union st_info *st_tag)
{
	switch (req_type) {
	case PCI_TPH_REQ_TPH_ONLY: /* 8 bit tags */
		switch (mem_type) {
		case TPH_MEM_TYPE_VM:
			if (st_tag->vm_st_valid)
				return st_tag->vm_st;
			break;
		case TPH_MEM_TYPE_PM:
			if (st_tag->pm_st_valid)
				return st_tag->pm_st;
			break;
		}
		break;
	case PCI_TPH_REQ_EXT_TPH: /* 16 bit tags */
		switch (mem_type) {
		case TPH_MEM_TYPE_VM:
			if (st_tag->vm_xst_valid)
				return st_tag->vm_xst;
			break;
		case TPH_MEM_TYPE_PM:
			if (st_tag->pm_xst_valid)
				return st_tag->pm_xst;
			break;
		}
		break;
	default:
		pr_err("invalid steering tag in ACPI _DSM\n");
		return 0;
	}

	return 0;
}

#define TPH_ST_DSM_FUNC_INDEX	0xF
static acpi_status tph_invoke_dsm(acpi_handle handle, u32 cpu_uid, u8 ph,
				  u8 target_type, bool cache_ref_valid,
				  u64 cache_ref, union st_info *st_out)
{
	union acpi_object arg3[3], in_obj, *out_obj;

	if (!acpi_check_dsm(handle, &pci_acpi_dsm_guid, 7, BIT(TPH_ST_DSM_FUNC_INDEX)))
		return AE_ERROR;

	/* DWORD: feature ID (0 for processor cache ST query) */
	arg3[0].integer.type = ACPI_TYPE_INTEGER;
	arg3[0].integer.value = 0;

	/* DWORD: target UID */
	arg3[1].integer.type = ACPI_TYPE_INTEGER;
	arg3[1].integer.value = cpu_uid;

	/* QWORD: properties */
	arg3[2].integer.type = ACPI_TYPE_INTEGER;
	arg3[2].integer.value = ph & 3;
	arg3[2].integer.value |= (target_type & 1) << 2;
	arg3[2].integer.value |= (cache_ref_valid & 1) << 3;
	arg3[2].integer.value |= (cache_ref << 32);

	in_obj.type = ACPI_TYPE_PACKAGE;
	in_obj.package.count = ARRAY_SIZE(arg3);
	in_obj.package.elements = arg3;

	out_obj = acpi_evaluate_dsm(handle, &pci_acpi_dsm_guid, 7,
				    TPH_ST_DSM_FUNC_INDEX, &in_obj);

	if (!out_obj)
		return AE_ERROR;

	if (out_obj->type != ACPI_TYPE_BUFFER) {
		ACPI_FREE(out_obj);
		return AE_ERROR;
	}

	st_out->value = *((u64 *)(out_obj->buffer.pointer));

	ACPI_FREE(out_obj);

	return AE_OK;
}

/* Update the ST Mode Select field of TPH Control Register */
static void set_ctrl_reg_mode_sel(struct pci_dev *pdev, u8 st_mode)
{
	u32 reg_val;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg_val);

	reg_val &= ~PCI_TPH_CTRL_MODE_SEL_MASK;
	reg_val |= FIELD_PREP(PCI_TPH_CTRL_MODE_SEL_MASK, st_mode);

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg_val);
}

/* Update the TPH Requester Enable field of TPH Control Register */
static void set_ctrl_reg_req_en(struct pci_dev *pdev, u8 req_type)
{
	u32 reg_val;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg_val);

	reg_val &= ~PCI_TPH_CTRL_REQ_EN_MASK;
	reg_val |= FIELD_PREP(PCI_TPH_CTRL_REQ_EN_MASK, req_type);

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg_val);
}

static bool int_vec_mode_supported(struct pci_dev *pdev)
{
	u32 reg_val;
	u8 mode;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg_val);
	mode = FIELD_GET(PCI_TPH_CAP_INT_VEC, reg_val);

	return !!mode;
}

static u32 get_st_table_loc(struct pci_dev *pdev)
{
	u32 reg_val;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg_val);

	return FIELD_GET(PCI_TPH_CAP_LOC_MASK, reg_val);
}

static bool msix_index_in_bound(struct pci_dev *pdev, int msi_idx)
{
	u32 reg_val;
	u16 st_tbl_sz;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg_val);
	st_tbl_sz = FIELD_GET(PCI_TPH_CAP_ST_MASK, reg_val);

	return msi_idx <= st_tbl_sz;
}

/* Write ST to MSI-X vector control reg - Return 0 if OK, otherwise errno */
static int tph_write_tag_to_msix(struct pci_dev *pdev, int msi_idx, u16 tag)
{
	struct msi_desc *msi_desc = NULL;
	void __iomem *vec_ctrl;
	u32 val;
	int err = 0;

	if (!msix_index_in_bound(pdev, msi_idx))
		return -EINVAL;

	msi_lock_descs(&pdev->dev);

	/* find the msi_desc entry with matching msi_idx */
	msi_for_each_desc(msi_desc, &pdev->dev, MSI_DESC_ASSOCIATED) {
		if (msi_desc->msi_index == msi_idx)
			break;
	}

	if (!msi_desc) {
		pci_err(pdev, "MSI-X descriptor for #%d not found\n", msi_idx);
		err = -ENXIO;
		goto err_out;
	}

	/* get the vector control register (offset 0xc) pointed by msi_idx */
	vec_ctrl = pdev->msix_base + msi_idx * PCI_MSIX_ENTRY_SIZE;
	vec_ctrl += PCI_MSIX_ENTRY_VECTOR_CTRL;

	val = readl(vec_ctrl);
	val &= 0xffff;
	val |= (tag << 16);
	writel(val, vec_ctrl);

	/* read back to flush the update */
	val = readl(vec_ctrl);

err_out:
	msi_unlock_descs(&pdev->dev);
	return err;
}

/* Return root port TPH completer capability - 0 means none */
static u8 get_rp_completer_support(struct pci_dev *pdev)
{
	struct pci_dev *rp;
	u32 reg_val;
	int ret;

	rp = pcie_find_root_port(pdev);
	if (!rp) {
		pci_err(pdev, "cannot find root port of %s\n", dev_name(&pdev->dev));
		return 0;
	}

	ret = pcie_capability_read_dword(rp, PCI_EXP_DEVCAP2, &reg_val);
	if (ret) {
		pci_err(pdev, "cannot read device capabilities 2\n");
		return 0;
	}

	return FIELD_GET(PCI_EXP_DEVCAP2_TPH_COMP_MASK, reg_val);
}

/*
 * TPH device needs to be below a rootport with the TPH Completer and
 * the completer must offer a compatible level of completer support to that
 * requested by the device driver.
 */
static bool rp_completer_support_ok(struct pci_dev *pdev, u8 req_cap)
{
	u8 rp_cap;

	rp_cap = get_rp_completer_support(pdev);

	if (req_cap > rp_cap) {
		pci_err(pdev, "root port lacks proper TPH completer capability\n");
		return false;
	}

	return true;
}

/* Return 0 if OK, otherwise errno on failure */
static int pcie_tph_write_st(struct pci_dev *pdev, unsigned int msix_idx,
			     u8 req_type, u16 tag)
{
	int offset;
	u32 loc;
	int err = 0;

	/* setting ST isn't needed - not an error, just return OK */
	if (!pdev->tph_cap || pci_tph_disabled() || pci_tph_nostmode() ||
	    !pdev->msix_enabled || !int_vec_mode_supported(pdev))
		return 0;

	/* setting ST is incorrect in the following cases - return error */
	if (!msix_index_in_bound(pdev, msix_idx) || !rp_completer_support_ok(pdev, req_type))
		return -EINVAL;

	/*
	 * disable TPH before updating the tag to avoid potential instability
	 * as cautioned in PCIE Base Spec r6.2, sect 6.17.3 "ST Modes of Operation"
	 */
	pcie_tph_disable(pdev);

	loc = get_st_table_loc(pdev);
	/* Note: use FIELD_PREP to match PCI_TPH_LOC_* definitions in header */
	loc = FIELD_PREP(PCI_TPH_CAP_LOC_MASK, loc);

	switch (loc) {
	case PCI_TPH_LOC_MSIX:
		err = tph_write_tag_to_msix(pdev, msix_idx, tag);
		break;
	case PCI_TPH_LOC_CAP:
		offset = pdev->tph_cap + PCI_TPH_BASE_SIZEOF + msix_idx * sizeof(u16);
		err = pci_write_config_word(pdev, offset, tag);
		break;
	default:
		pci_err(pdev, "unable to write steering tag for device %s\n",
			dev_name(&pdev->dev));
		err = -EINVAL;
		break;
	}

	if (!err) {
		/* re-enable interrupt vector mode */
		set_ctrl_reg_mode_sel(pdev, PCI_TPH_INT_VEC_MODE);
		set_ctrl_reg_req_en(pdev, req_type);
	}

	return err;
}

void pcie_tph_set_nostmode(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)
		return;

	set_ctrl_reg_mode_sel(pdev, PCI_TPH_NO_ST_MODE);
	set_ctrl_reg_req_en(pdev, PCI_TPH_REQ_TPH_ONLY);
}

void pcie_tph_disable(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)
		return;

	set_ctrl_reg_req_en(pdev, PCI_TPH_REQ_DISABLE);
}

void pcie_tph_init(struct pci_dev *pdev)
{
	pdev->tph_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_TPH);
}

/**
 * pcie_tph_intr_vec_supported() - Check if interrupt vector mode supported for dev
 * @pdev: pci device
 *
 * Return:
 *        true : intr vector mode supported
 *        false: intr vector mode not supported
 */
bool pcie_tph_intr_vec_supported(struct pci_dev *pdev)
{
	if (!pdev->tph_cap || pci_tph_disabled() || !pdev->msix_enabled ||
	    !int_vec_mode_supported(pdev))
		return false;

	return true;
}
EXPORT_SYMBOL(pcie_tph_intr_vec_supported);

/**
 * pcie_tph_get_st_from_acpi() - Retrieve steering tag for a specific CPU
 * using platform ACPI _DSM
 * @pdev: pci device
 * @cpu_acpi_uid: the acpi cpu_uid.
 * @mem_type: memory type (vram, nvram)
 * @req_type: request type (disable, tph, extended tph)
 * @tag: steering tag return value
 *
 * Return: 0 if success, otherwise errno
 */
int pcie_tph_get_st_from_acpi(struct pci_dev *pdev, unsigned int cpu_acpi_uid,
			      enum tph_mem_type mem_type, u8 req_type,
			      u16 *tag)
{
	struct pci_dev *rp;
	acpi_handle rp_acpi_handle;
	union st_info info;

	if (!pdev->tph_cap)
		return -ENODEV;

	/* find ACPI handler for device's root port */
	rp = pcie_find_root_port(pdev);
	if (!rp || !rp->bus || !rp->bus->bridge)
		return -ENODEV;
	rp_acpi_handle = ACPI_HANDLE(rp->bus->bridge);

	/* invoke _DSM to extract tag value */
	if (tph_invoke_dsm(rp_acpi_handle, cpu_acpi_uid, 0, 0, false, 0, &info) != AE_OK) {
		*tag = 0;
		return -EINVAL;
	}

	*tag = tph_extract_tag(mem_type, req_type, &info);
	pci_dbg(pdev, "%s: cpu=%d tag=%d\n", __func__, cpu_acpi_uid, *tag);

	return 0;
}
EXPORT_SYMBOL(pcie_tph_get_st_from_acpi);

/**
 * pcie_tph_set_st() - Set steering tag in ST table entry
 * @pdev: pci device
 * @msix_idx: ordinal number of msix interrupt.
 * @cpu_acpi_uid: the acpi cpu_uid.
 * @mem_type: memory type (vram, nvram)
 * @req_type: request type (disable, tph, extended tph)
 *
 * Return: 0 if success, otherwise errno
 */
int pcie_tph_set_st(struct pci_dev *pdev, unsigned int msix_idx,
		    unsigned int cpu_acpi_uid, enum tph_mem_type mem_type,
		    u8 req_type)
{
	u16 tag;
	int err = 0;

	if (!pdev->tph_cap)
		return -ENODEV;

	err = pcie_tph_get_st_from_acpi(pdev, cpu_acpi_uid, mem_type,
					req_type, &tag);

	if (err)
		return err;

	pci_dbg(pdev, "%s: writing tag %d for msi-x intr %d (cpu: %d)\n",
		__func__, tag, msix_idx, cpu_acpi_uid);

	err = pcie_tph_write_st(pdev, msix_idx, req_type, tag);

	return err;
}
EXPORT_SYMBOL(pcie_tph_set_st);
