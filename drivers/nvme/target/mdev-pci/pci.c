// SPDX-License-Identifier: GPL-2.0+
/*
 * NVMe virtual controller minimal PCI/PCIe config space implementation
 * Copyright (c) 2019 - Maxim Levitsky
 */
#include <linux/kernel.h>
#include <linux/pci.h>
#include "priv.h"

/* setup a 64 bit PCI bar */
void nvmet_mdev_pci_setup_bar(struct nvmet_mdev_vctrl *vctrl, u8 bar,
			      unsigned int size, region_access_fn access_fn)
{
	nvmet_mdev_vctrl_add_region(vctrl, VFIO_PCI_BAR0_REGION_INDEX +
				    ((bar - PCI_BASE_ADDRESS_0) >> 2),
				    size, access_fn);

	store_le32(vctrl->pcicfg.wmask + bar, ~((u64)size - 1));
	store_le32(vctrl->pcicfg.values + bar,
		   PCI_BASE_ADDRESS_SPACE_MEMORY |
		   PCI_BASE_ADDRESS_MEM_TYPE_64);
}

/* Allocate a pci capability */
static u8 nvmet_mdev_pci_allocate_cap(struct nvmet_mdev_vctrl *vctrl, u8 id,
				      u8 size)
{
	u8 *cfg = vctrl->pcicfg.values;
	u8 newcap = vctrl->pcicfg.end;
	u8 cap = cfg[PCI_CAPABILITY_LIST];

	size = round_up(size, 4);
	/* only standard cfg space caps for now */
	WARN_ON(newcap + size > 256);

	if (!cfg[PCI_CAPABILITY_LIST]) {
		/* special case for first capability */
		u16 status = load_le16(cfg + PCI_STATUS);

		status |= PCI_STATUS_CAP_LIST;
		store_le16(cfg + PCI_STATUS, status);

		cfg[PCI_CAPABILITY_LIST] = newcap;
		goto setupcap;
	}

	while (cfg[cap + PCI_CAP_LIST_NEXT] != 0)
		cap = cfg[cap + PCI_CAP_LIST_NEXT];

	cfg[cap + PCI_CAP_LIST_NEXT] = newcap;

setupcap:
	cfg[newcap + PCI_CAP_LIST_ID] = id;
	cfg[newcap + PCI_CAP_LIST_NEXT] = 0;
	vctrl->pcicfg.end += size;
	return newcap;
}

static void nvmet_mdev_pci_setup_pm_cap(struct nvmet_mdev_vctrl *vctrl)
{
	u8 *cfg  =  vctrl->pcicfg.values;
	u8 *cfgm =  vctrl->pcicfg.wmask;

	u8 cap = nvmet_mdev_pci_allocate_cap(vctrl, PCI_CAP_ID_PM,
					     PCI_PM_SIZEOF);

	store_le16(cfg + cap + PCI_PM_PMC, 0x3);
	store_le16(cfg + cap + PCI_PM_CTRL, PCI_PM_CTRL_NO_SOFT_RESET);
	store_le16(cfgm + cap + PCI_PM_CTRL, 0x3);
	vctrl->pcicfg.pmcap = cap;
}

static void nvmet_mdev_pci_setup_msix_cap(struct nvmet_mdev_vctrl *vctrl)
{
	u8 *cfg  =  vctrl->pcicfg.values;
	u8 *cfgm =  vctrl->pcicfg.wmask;
	u8  cap = nvmet_mdev_pci_allocate_cap(vctrl, PCI_CAP_ID_MSIX,
					      PCI_CAP_MSIX_SIZEOF);

	int MSIX_TBL_SIZE = roundup(MAX_VIRTUAL_IRQS * 16, PAGE_SIZE);
	int MSIX_PBA_SIZE = roundup(DIV_ROUND_UP(MAX_VIRTUAL_IRQS, 8),
				    PAGE_SIZE);

	store_le16(cfg + cap + PCI_MSIX_FLAGS, MAX_VIRTUAL_IRQS - 1);
	store_le16(cfgm + cap + PCI_MSIX_FLAGS,
		   PCI_MSIX_FLAGS_MASKALL | PCI_MSIX_FLAGS_ENABLE);

	store_le32(cfg + cap + PCI_MSIX_TABLE, 0x2);
	store_le32(cfg + cap + PCI_MSIX_PBA, MSIX_TBL_SIZE | 0x2);

	nvmet_mdev_pci_setup_bar(vctrl, PCI_BASE_ADDRESS_2,
				 __roundup_pow_of_two(MSIX_TBL_SIZE +
						MSIX_PBA_SIZE), NULL);
	vctrl->pcicfg.msixcap = cap;
}

static void nvmet_mdev_pci_setup_pcie_cap(struct nvmet_mdev_vctrl *vctrl)
{
	u8 *cfg = vctrl->pcicfg.values;
	u8 cap = nvmet_mdev_pci_allocate_cap(vctrl, PCI_CAP_ID_EXP,
					     PCI_CAP_EXP_ENDPOINT_SIZEOF_V2);

	store_le16(cfg + cap + PCI_EXP_FLAGS, 0x02 |
		   (PCI_EXP_TYPE_ENDPOINT << 4));

	store_le32(cfg + cap + PCI_EXP_DEVCAP,
		   PCI_EXP_DEVCAP_RBER | PCI_EXP_DEVCAP_FLR);
	store_le32(cfg + cap + PCI_EXP_LNKCAP,
		   PCI_EXP_LNKCAP_SLS_8_0GB | (4 << 4) /*4x*/);
	store_le16(cfg + cap + PCI_EXP_LNKSTA,
		   PCI_EXP_LNKSTA_CLS_8_0GB | (4 << 4) /*4x*/);

	store_le32(cfg + cap + PCI_EXP_LNKCAP2, PCI_EXP_LNKCAP2_SLS_8_0GB);
	store_le16(cfg + cap + PCI_EXP_LNKCTL2, PCI_EXP_LNKCTL2_TLS_8_0GT);
	vctrl->pcicfg.pciecap = cap;
}

/* This is called on PCI config read/write */
static int nvmet_mdev_pci_cfg_access(struct nvmet_mdev_vctrl *vctrl, u16 offset,
				     char *buf, u32 count, bool is_write)
{
	unsigned int i;

	mutex_lock(&vctrl->lock);

	if (!is_write) {
		memcpy(buf, (vctrl->pcicfg.values + offset), count);
		goto out;
	}

	for (i = 0; i < count; i++) {
		u8 address = offset + i;
		u8 value = buf[i];
		u8 old_value = vctrl->pcicfg.values[address];
		u8 wmask = vctrl->pcicfg.wmask[address];
		u8 new_value = (value & wmask) | (old_value & ~wmask);

		/* D3/D0 power control */
		if (address == vctrl->pcicfg.pmcap + PCI_PM_CTRL) {
			u8 state = new_value & 0x03;

			if (state != 0 && state != 3)
				new_value = old_value;

			if (old_value != new_value) {
				const char *s = state == 3 ? "D3" : "D0";

				if (state == 3)
					__nvmet_mdev_vctrl_reset(vctrl, true);
				_DBG(vctrl, "PCI: going to %s\n", s);
			}
		}

		/* FLR reset */
		if (address == vctrl->pcicfg.pciecap + PCI_EXP_DEVCTL + 1)
			if (value & 0x80) {
				_DBG(vctrl, "PCI: FLR reset\n");
				__nvmet_mdev_vctrl_reset(vctrl, true);
			}
		vctrl->pcicfg.values[offset + i] = new_value;
	}
out:
	mutex_unlock(&vctrl->lock);
	return count;
}

/* setup pci configuration */
int nvmet_mdev_pci_create(struct nvmet_mdev_vctrl *vctrl)
{
	u8 *cfg, *cfgm;

	vctrl->pcicfg.values = kzalloc(NVMET_MDEV_PCI_CFG_SIZE, GFP_KERNEL);
	if (!vctrl->pcicfg.values)
		return -ENOMEM;

	vctrl->pcicfg.wmask = kzalloc(NVMET_MDEV_PCI_CFG_SIZE, GFP_KERNEL);
	if (!vctrl->pcicfg.wmask) {
		kfree(vctrl->pcicfg.values);
		return -ENOMEM;
	}

	cfg = vctrl->pcicfg.values;
	cfgm = vctrl->pcicfg.wmask;

	nvmet_mdev_vctrl_add_region(vctrl, VFIO_PCI_CONFIG_REGION_INDEX,
				    NVMET_MDEV_PCI_CFG_SIZE,
				    nvmet_mdev_pci_cfg_access);

	/* vendor information */
	store_le16(cfg + PCI_VENDOR_ID, NVME_MDEV_PCI_VENDOR_ID);
	store_le16(cfg + PCI_DEVICE_ID, NVME_MDEV_PCI_DEVICE_ID);

	/* pci command register */
	store_le16(cfgm + PCI_COMMAND,
		   PCI_COMMAND_INTX_DISABLE |
		   PCI_COMMAND_MEMORY |
		   PCI_COMMAND_MASTER);

	/* pci status register */
	store_le16(cfg + PCI_STATUS, PCI_STATUS_CAP_LIST);

	/* subsystem information */
	store_le16(cfg + PCI_SUBSYSTEM_VENDOR_ID, NVME_MDEV_PCI_SUBVENDOR_ID);
	store_le16(cfg + PCI_SUBSYSTEM_ID, NVME_MDEV_PCI_SUBDEVICE_ID);
	store_le8(cfg + PCI_CLASS_REVISION, NVME_MDEV_PCI_REVISION);

	/* Programming Interface (NVM Express) */
	store_le8(cfg + PCI_CLASS_PROG, 0x02);

	/*
	 * Device class and subclass (Mass storage controller, Non-Volatile
	 * memory controller)
	 */
	store_le16(cfg + PCI_CLASS_DEVICE, 0x0108);

	/* dummy read/write */
	store_le8(cfgm + PCI_CACHE_LINE_SIZE, 0xFF);

	/* initial value */
	store_le8(cfg + PCI_CAPABILITY_LIST, 0);
	vctrl->pcicfg.end = 0x40;

	nvmet_mdev_pci_setup_pm_cap(vctrl);
	nvmet_mdev_pci_setup_msix_cap(vctrl);
	nvmet_mdev_pci_setup_pcie_cap(vctrl);

	/* INTX IRQ number - info only for BIOS */
	store_le8(cfgm + PCI_INTERRUPT_LINE, 0xFF);
	store_le8(cfg + PCI_INTERRUPT_PIN, 0x01);

	return 0;
}

/* teardown pci configuration */
void nvmet_mdev_pci_free(struct nvmet_mdev_vctrl *vctrl)
{
	kfree(vctrl->pcicfg.values);
	kfree(vctrl->pcicfg.wmask);
	vctrl->pcicfg.values = NULL;
	vctrl->pcicfg.wmask = NULL;
}
