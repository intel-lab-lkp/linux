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
