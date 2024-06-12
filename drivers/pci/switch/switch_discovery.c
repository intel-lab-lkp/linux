// SPDX-License-Identifier: GPL-2.0
/*
 *  PCI Switch Discovery module
 *
 *  Copyright (c) 2024  Broadcom Inc.
 *
 *  Authors: Broadcom Inc.
 *           Sumanesh Samanta <sumanesh.samanta@broadcom.com>
 *           Shivasharan S <shivasharan.srikanteshwara@broadcom.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/pci.h>
#include <linux/vmalloc.h>
#include "switch_discovery.h"

static DECLARE_RWSEM(sw_disc_rwlock);
static struct kobject *sw_disc_kobj, *sw_link_kobj;
static struct kobject *sw_kobj[SWD_MAX_VIRT_SWITCH];
static DECLARE_BITMAP(swdata_valid, SWD_MAX_VIRT_SWITCH);

static struct switch_data *swdata;

static int sw_disc_probe(void);
static int sw_disc_create_sysfs_files(void);
static bool brcm_sw_is_p2p_supported(struct pci_dev *pdev, char *serial_num);

static inline bool sw_disc_is_supported_pdev(struct pci_dev *pdev)
{
	if ((pdev->vendor == PCI_VENDOR_ID_LSI) &&
	   ((pdev->device == PCI_DEVICE_ID_BRCM_PEX_89000_HLC) ||
	    (pdev->device == PCI_DEVICE_ID_BRCM_PEX_89000_LLC)))
		return true;

	return false;
}

static ssize_t sw_disc_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	int retval;

	down_write(&sw_disc_rwlock);
	retval = sw_disc_probe();
	if (!retval) {
		pr_debug("No new switch found\n");
		goto exit_success;
	}

	retval = sw_disc_create_sysfs_files();
	if (retval < 0) {
		pr_err("Failed to create the sysfs entries, retval %d\n",
		       retval);
	}

exit_success:
	up_write(&sw_disc_rwlock);
	return sysfs_emit(buf, SWD_SCAN_DONE);
}

/* This function probes the PCIe devices for virtual links */
static int sw_disc_probe(void)
{
	int i, bit;
	struct pci_dev *pdev = NULL;
	int topology_changed = 0;
	DECLARE_BITMAP(sw_found_map, SWD_MAX_VIRT_SWITCH);

	while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL) {
		int sw_found;

		/* Currently this function only traverses Broadcom
		 * PEX switches and determines the virtual SW links.
		 * Other Switch vendors can add their specific logic
		 * determine the virtual links.
		 */
		if (!sw_disc_is_supported_pdev(pdev))
			continue;

		sw_found = -1;

		for_each_set_bit(bit, swdata_valid, SWD_MAX_VIRT_SWITCH) {
			if (swdata[bit].devfn == pdev->devfn &&
			    swdata[bit].bus == pdev->bus) {
				sw_found = bit;
				set_bit(sw_found, sw_found_map);
				break;
			}
		}

		if (sw_found != -1)
			continue;

		for (i = 0; i < SWD_MAX_VIRT_SWITCH; i++)
			if (!swdata[i].bus)
				break;

		if (i >= SWD_MAX_VIRT_SWITCH) {
			pr_err("Max switch exceeded\n");
			break;
		}

		sw_found = i;

		if (!brcm_sw_is_p2p_supported(pdev, (char *)&swdata[sw_found].serial_num))
			continue;

		/* Found a new switch which supports P2P */
		swdata[sw_found].devfn = pdev->devfn;
		swdata[sw_found].bus = pdev->bus;

		topology_changed = 1;
		set_bit(sw_found, sw_found_map);
		set_bit(sw_found, swdata_valid);
	}

	/* handle device removal */
	for_each_clear_bit(bit, sw_found_map, SWD_MAX_VIRT_SWITCH) {
		if (test_bit(bit, swdata_valid)) {
			memset(&swdata[bit], 0, sizeof(swdata[i]));
			clear_bit(bit, swdata_valid);
			topology_changed = 1;
		}
	}

	return topology_changed;
}

/* Check the various config space registers of the Broadcom PCI device and
 * return true if the device supports inter switch P2P.
 * If P2P is supported, return the device serial number back to
 * caller.
 */
bool brcm_sw_is_p2p_supported(struct pci_dev *pdev, char *serial_num)
{
	int base;
	u32 cap_data1, cap_data2;
	u16 vsec;
	u32 vsec_data;

	base = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DSN);
	if (!base) {
		pr_debug("Failed to get extended capability bus %x devfn %x\n",
			 pdev->bus->number, pdev->devfn);
		return false;
	}

	vsec = pci_find_vsec_capability(pdev, PCI_VENDOR_ID_LSI, 1);
	if (!vsec) {
		pr_debug("Failed to get VSEC bus %x devfn %x\n",
			 pdev->bus->number, pdev->devfn);
		return false;
	}

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_UPSTREAM)
		return false;

	pci_read_config_dword(pdev, base + 8, &cap_data1);
	pci_read_config_dword(pdev, base + 4, &cap_data2);

	pci_read_config_dword(pdev, vsec + 12, &vsec_data);

	pr_debug("Found Broadcom device bus 0x%x devfn 0x%x "
		 "Serial Number: 0x%x 0x%x, VSEC 0x%x\n",
		 pdev->bus->number, pdev->devfn,
		 cap_data1, cap_data2, vsec_data);

	if (!SECURE_PART(cap_data1))
		return false;

	if (!(P2PMASK(vsec_data) & INTER_SWITCH_LINK))
		return false;

	if (serial_num)
		snprintf(serial_num, SWD_MAX_CHAR, "%x%x", cap_data1, cap_data2);

	return true;
}

static int sw_disc_create_sysfs_files(void)
{
	int i, j, retval;

	for (i = 0; i < SWD_MAX_VIRT_SWITCH; i++) {
		if (sw_kobj[i]) {
			kobject_put(sw_kobj[i]);
			sw_kobj[i] = NULL;
		}
	}

	if (sw_link_kobj) {
		kobject_put(sw_link_kobj);
		sw_link_kobj = NULL;
	}

	sw_link_kobj = kobject_create_and_add(SWD_LINK_DIR_STRING, sw_disc_kobj);
	if (!sw_link_kobj) {
		pr_err("Failed to create pci link object\n");
		return -ENOMEM;
	}

	for (i = 0; i < SWD_MAX_VIRT_SWITCH; i++) {
		int segment, bus, device, function;
		char bdf_i[SWD_MAX_CHAR];

		if (!test_bit(i, swdata_valid))
			continue;

		segment = pci_domain_nr(swdata[i].bus);
		bus = swdata[i].bus->number;
		device = pci_ari_enabled(swdata[i].bus) ?
				0 : PCI_SLOT(swdata[i].devfn);
		function = pci_ari_enabled(swdata[i].bus) ?
				swdata[i].devfn : PCI_FUNC(swdata[i].devfn);
		sprintf(bdf_i, "%04x:%02x:%02x.%x",
			segment, bus, device, function);

		for (j = i + 1; j < SWD_MAX_VIRT_SWITCH; j++) {
			char bdf_j[SWD_MAX_CHAR];

			if (!test_bit(j, swdata_valid))
				continue;
			segment = pci_domain_nr(swdata[j].bus);
			bus = swdata[j].bus->number;
			device = pci_ari_enabled(swdata[j].bus) ?
					0 : PCI_SLOT(swdata[j].devfn);
			function = pci_ari_enabled(swdata[j].bus) ?
					swdata[j].devfn : PCI_FUNC(swdata[j].devfn);
			sprintf(bdf_j, "%04x:%02x:%02x.%x",
				segment, bus, device, function);

			if (strcmp(swdata[i].serial_num, swdata[j].serial_num) == 0) {
				if (!sw_kobj[i]) {
					sw_kobj[i] = kobject_create_and_add(bdf_i,
									    sw_link_kobj);
					if (!sw_kobj[i]) {
						pr_err("Failed to create sysfs entry for switch %s\n",
						       bdf_i);
					}
				}

				if (!sw_kobj[j]) {
					sw_kobj[j] = kobject_create_and_add(bdf_j,
									    sw_link_kobj);
					if (!sw_kobj[j]) {
						pr_err("Failed to create sysfs entry for switch %s\n",
						       bdf_j);
					}
				}

				retval = sysfs_create_link(sw_kobj[i], sw_kobj[j], bdf_j);
				if (retval)
					pr_err("Error creating symlink %s and %s\n",
					       bdf_i, bdf_j);

				retval = sysfs_create_link(sw_kobj[j], sw_kobj[i], bdf_i);
				if (retval)
					pr_err("Error creating symlink %s and %s\n",
					       bdf_j, bdf_i);
			}
		}
	}

	return 0;
}

/*
 * Check if the two pci devices have virtual P2P link available.
 * This function is used by the p2pdma to determine virtual
 * links between the PCI-to-PCI bridges
 */
bool sw_disc_check_virtual_link(struct pci_dev *a,
				 struct pci_dev *b)
{
	char serial_num_a[SWD_MAX_CHAR], serial_num_b[SWD_MAX_CHAR];

	/*
	 * Check if the PCIe devices support Virtual P2P links
	 */
	if (!sw_disc_is_supported_pdev(a))
		return false;

	if (!sw_disc_is_supported_pdev(b))
		return false;

	if (brcm_sw_is_p2p_supported(a, serial_num_a) &&
	    brcm_sw_is_p2p_supported(b, serial_num_b))
		if (!strcmp(serial_num_a, serial_num_b))
			return true;

	return false;
}
EXPORT_SYMBOL_GPL(sw_disc_check_virtual_link);

static struct kobj_attribute sw_disc_attribute =
	__ATTR(SWD_FILE_NAME_STRING, 0444, sw_disc_show, NULL);

// Create attribute group
static struct attribute *attrs[] = {
	&sw_disc_attribute.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int __init sw_discovery_init(void)
{
	int i, retval;

	for (i = 0; i < SWD_MAX_VIRT_SWITCH; i++)
		sw_kobj[i] = NULL;

	// Create "sw_disc" kobject
	sw_disc_kobj = kobject_create_and_add(SWD_DIR_STRING, kernel_kobj);
	if (!sw_disc_kobj) {
		pr_err("Failed to create sw_disc_kobj\n");
		return -ENOMEM;
	}

	retval = sysfs_create_group(sw_disc_kobj, &attr_group);
	if (retval) {
		pr_err("Cannot register sysfs attribute group\n");
		kobject_put(sw_disc_kobj);
	}

	swdata = kzalloc(sizeof(swdata) * SWD_MAX_VIRT_SWITCH, GFP_KERNEL);
	if (!swdata) {
		sysfs_remove_group(sw_disc_kobj, &attr_group);
		kobject_put(sw_disc_kobj);
		return 0;
	}

	pr_info("Loading PCIe switch discovery module, version %s\n",
		SWITCH_DISC_VERSION);

	return 0;
}

static void __exit sw_discovery_exit(void)
{
	int i;

	if (!swdata)
		kfree(swdata);

	for (i = 0; i < SWD_MAX_VIRT_SWITCH; i++) {
		if (sw_kobj[i])
			kobject_put(sw_kobj[i]);
	}

	// Remove kobject
	if (sw_link_kobj)
		kobject_put(sw_link_kobj);

	sysfs_remove_group(sw_disc_kobj, &attr_group);
	kobject_put(sw_disc_kobj);
}

module_init(sw_discovery_init);
module_exit(sw_discovery_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Broadcom Inc.");
MODULE_VERSION(SWITCH_DISC_VERSION);
MODULE_DESCRIPTION("PCIe Switch Discovery Module");
