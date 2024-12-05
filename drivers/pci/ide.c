// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

/* PCIe 6.2 section 6.33 Integrity & Data Encryption (IDE) */

#define dev_fmt(fmt) "PCI/IDE: " fmt
#include <linux/pci.h>
#include <linux/sysfs.h>
#include <linux/pci-ide.h>
#include <linux/bitfield.h>
#include "pci.h"

static int sel_ide_offset(u16 cap, int stream_id, int nr_ide_mem)
{
	return cap + stream_id * PCI_IDE_SELECTIVE_BLOCK_SIZE(nr_ide_mem);
}

void pci_ide_init(struct pci_dev *pdev)
{
	u16 ide_cap, sel_ide_cap;
	int nr_ide_mem = 0;
	u32 val = 0;

	if (!pci_is_pcie(pdev))
		return;

	ide_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_IDE);
	if (!ide_cap)
		return;

	/*
	 * Check for selective stream capability from endpoint to root-port, and
	 * require consistent number of address association blocks
	 */
	pci_read_config_dword(pdev, ide_cap + PCI_IDE_CAP, &val);
	if ((val & PCI_IDE_CAP_SELECTIVE) == 0)
		return;

	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ENDPOINT) {
		struct pci_dev *rp = pcie_find_root_port(pdev);

		if (!rp->ide_cap)
			return;
	}

	if (val & PCI_IDE_CAP_LINK)
		sel_ide_cap = ide_cap + PCI_IDE_LINK_STREAM +
			      (PCI_IDE_CAP_LINK_TC_NUM(val) + 1) *
				      PCI_IDE_LINK_BLOCK_SIZE;
	else
		sel_ide_cap = ide_cap + PCI_IDE_LINK_STREAM;

	for (int i = 0; i < PCI_IDE_CAP_SELECTIVE_STREAMS_NUM(val); i++) {
		if (i == 0) {
			pci_read_config_dword(pdev, sel_ide_cap, &val);
			nr_ide_mem = PCI_IDE_SEL_CAP_ASSOC_NUM(val);
		} else {
			int offset = sel_ide_offset(sel_ide_cap, i, nr_ide_mem);

			pci_read_config_dword(pdev, offset, &val);

			/*
			 * lets not entertain devices that do not have a
			 * constant number of address association blocks
			 */
			if (PCI_IDE_SEL_CAP_ASSOC_NUM(val) != nr_ide_mem) {
				pci_info(pdev, "Unsupported Selective Stream %d capability\n", i);
				return;
			}
		}
	}

	pdev->ide_cap = ide_cap;
	pdev->sel_ide_cap = sel_ide_cap;
	pdev->nr_ide_mem = nr_ide_mem;
}

void pci_init_host_bridge_ide(struct pci_host_bridge *hb)
{
	hb->ide_stream_res =
		DEFINE_RES_MEM_NAMED(0, 0, "IDE Address Association");
}

/*
 * Retrieve stream association parameters for devid (RID) and resources
 * (device address ranges)
 */
void pci_ide_stream_probe(struct pci_dev *pdev, struct pci_ide *ide)
{
	int num_vf = pci_num_vf(pdev);

	*ide = (struct pci_ide) { .stream_id = -1 };

	if (pdev->fm_enabled)
		ide->domain = pci_domain_nr(pdev->bus);
	ide->devid_start = pci_dev_id(pdev);

	/* for SR-IOV case, cover all VFs */
	if (num_vf)
		ide->devid_end = PCI_DEVID(pci_iov_virtfn_bus(pdev, num_vf),
					   pci_iov_virtfn_devfn(pdev, num_vf));
	else
		ide->devid_end = ide->devid_start;

	/* TODO: address association probing... */
}
EXPORT_SYMBOL_GPL(pci_ide_stream_probe);

static void __pci_ide_stream_teardown(struct pci_dev *pdev, struct pci_ide *ide)
{
	int pos;

	pos = sel_ide_offset(pdev->sel_ide_cap, ide->stream_id,
			     pdev->nr_ide_mem);

	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, 0);
	for (int i = ide->nr_mem - 1; i >= 0; i--) {
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_3(i), 0);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_2(i), 0);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_1(i), 0);
	}
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_2, 0);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_1, 0);
}

static void __pci_ide_stream_setup(struct pci_dev *pdev, struct pci_ide *ide)
{
	int pos;
	u32 val;

	pos = sel_ide_offset(pdev->sel_ide_cap, ide->stream_id,
			     pdev->nr_ide_mem);

	val = FIELD_PREP(PCI_IDE_SEL_RID_1_LIMIT_MASK, ide->devid_end);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_1, val);

	val = FIELD_PREP(PCI_IDE_SEL_RID_2_VALID, 1) |
	      FIELD_PREP(PCI_IDE_SEL_RID_2_BASE_MASK, ide->devid_start) |
	      FIELD_PREP(PCI_IDE_SEL_RID_2_SEG_MASK, ide->domain);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_2, val);

	for (int i = 0; i < ide->nr_mem; i++) {
		val = FIELD_PREP(PCI_IDE_SEL_ADDR_1_VALID, 1) |
		      FIELD_PREP(PCI_IDE_SEL_ADDR_1_BASE_LOW_MASK,
				 lower_32_bits(ide->mem[i].start) >>
					 PCI_IDE_SEL_ADDR_1_BASE_LOW_SHIFT) |
		      FIELD_PREP(PCI_IDE_SEL_ADDR_1_LIMIT_LOW_MASK,
				 lower_32_bits(ide->mem[i].end) >>
					 PCI_IDE_SEL_ADDR_1_LIMIT_LOW_SHIFT);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_1(i), val);

		val = upper_32_bits(ide->mem[i].end);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_2(i), val);

		val = upper_32_bits(ide->mem[i].start);
		pci_write_config_dword(pdev, pos + PCI_IDE_SEL_ADDR_3(i), val);
	}
}

/*
 * Establish IDE stream parameters in @pdev and, optionally, its root port
 */
int pci_ide_stream_setup(struct pci_dev *pdev, struct pci_ide *ide,
			 enum pci_ide_flags flags)
{
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);
	struct pci_dev *rp = pcie_find_root_port(pdev);
	int mem = 0, rc;

	if (ide->stream_id < 0 || ide->stream_id > U8_MAX) {
		pci_err(pdev, "Setup fail: Invalid stream id: %d\n", ide->stream_id);
		return -ENXIO;
	}

	if (test_and_set_bit_lock(ide->stream_id, hb->ide_stream_ids)) {
		pci_err(pdev, "Setup fail: Busy stream id: %d\n",
			ide->stream_id);
		return -EBUSY;
	}

	ide->name = kasprintf(GFP_KERNEL, "stream%d:%s", ide->stream_id,
			      dev_name(&pdev->dev));
	if (!ide->name) {
		rc = -ENOMEM;
		goto err_name;
	}

	rc = sysfs_create_link(&hb->dev.kobj, &pdev->dev.kobj, ide->name);
	if (rc)
		goto err_link;

	for (mem = 0; mem < ide->nr_mem; mem++)
		if (!__request_region(&hb->ide_stream_res, ide->mem[mem].start,
				      range_len(&ide->mem[mem]), ide->name,
				      0)) {
			pci_err(pdev,
				"Setup fail: stream%d: address association conflict [%#llx-%#llx]\n",
				ide->stream_id, ide->mem[mem].start,
				ide->mem[mem].end);

			rc = -EBUSY;
			goto err;
		}

	__pci_ide_stream_setup(pdev, ide);
	if (flags & PCI_IDE_SETUP_ROOT_PORT)
		__pci_ide_stream_setup(rp, ide);

	return 0;
err:
	for (; mem >= 0; mem--)
		__release_region(&hb->ide_stream_res, ide->mem[mem].start,
				 range_len(&ide->mem[mem]));
	sysfs_remove_link(&hb->dev.kobj, ide->name);
err_link:
	kfree(ide->name);
err_name:
	clear_bit_unlock(ide->stream_id, hb->ide_stream_ids);
	return rc;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_setup);

void pci_ide_enable_stream(struct pci_dev *pdev, struct pci_ide *ide)
{
	int pos;
	u32 val;

	pos = sel_ide_offset(pdev->sel_ide_cap, ide->stream_id,
			     pdev->nr_ide_mem);

	val = FIELD_PREP(PCI_IDE_SEL_CTL_ID_MASK, ide->stream_id) |
	      FIELD_PREP(PCI_IDE_SEL_CTL_DEFAULT, 1);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, val);
}
EXPORT_SYMBOL_GPL(pci_ide_enable_stream);

void pci_ide_disable_stream(struct pci_dev *pdev, struct pci_ide *ide)
{
	int pos;

	pos = sel_ide_offset(pdev->sel_ide_cap, ide->stream_id,
			     pdev->nr_ide_mem);

	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, 0);
}
EXPORT_SYMBOL_GPL(pci_ide_disable_stream);

void pci_ide_stream_teardown(struct pci_dev *pdev, struct pci_ide *ide,
			     enum pci_ide_flags flags)
{
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);
	struct pci_dev *rp = pcie_find_root_port(pdev);

	__pci_ide_stream_teardown(pdev, ide);
	if (flags & PCI_IDE_SETUP_ROOT_PORT)
		__pci_ide_stream_teardown(rp, ide);

	for (int i = ide->nr_mem - 1; i >= 0; i--)
		__release_region(&hb->ide_stream_res, ide->mem[i].start,
				 range_len(&ide->mem[i]));
	sysfs_remove_link(&hb->dev.kobj, ide->name);
	kfree(ide->name);
	clear_bit_unlock(ide->stream_id, hb->ide_stream_ids);
}
EXPORT_SYMBOL_GPL(pci_ide_stream_teardown);
