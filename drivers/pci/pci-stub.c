// SPDX-License-Identifier: GPL-2.0
/*
 * Simple stub driver to reserve a PCI device
 *
 * Copyright (C) 2008 Red Hat, Inc.
 * Author:
 *	Chris Wright
 *
 * Usage is simple, allocate a new id to the stub driver and bind the
 * device to it.  For example:
 *
 * # echo "8086 10f5" > /sys/bus/pci/drivers/pci-stub/new_id
 * # echo -n 0000:00:19.0 > /sys/bus/pci/drivers/e1000e/unbind
 * # echo -n 0000:00:19.0 > /sys/bus/pci/drivers/pci-stub/bind
 * # ls -l /sys/bus/pci/devices/0000:00:19.0/driver
 * .../0000:00:19.0/driver -> ../../../bus/pci/drivers/pci-stub
 *
 * Enhanced BUSID support for kernel 6.12+
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>

#define MAX_PCI_STUB_BUSIDS 32

static char ids[1024] __initdata;

struct pci_stub_busid {
	unsigned int domain;
	unsigned int bus;
	unsigned int device;
	unsigned int function;
	bool domain_specified;
};

static struct pci_stub_busid stub_busids[MAX_PCI_STUB_BUSIDS];
static unsigned int num_stub_busids;
static char busid_param[1024] = "";

static int pci_stub_probe(struct pci_dev *dev, const struct pci_device_id *id);
static struct pci_stub_busid *pci_stub_find_busid(struct pci_dev *dev);

module_param_string(busid, busid_param, sizeof(busid_param), 0444);
MODULE_PARM_DESC(busid, "PCI BUSID list to bind to pci-stub, "
		"format: [domain:]bus:dev.func[,domain:]bus:dev.func]* "
		"(e.g. 02:00.0,02:00.1 or 0000:02:00.0)");

module_param_string(ids, ids, sizeof(ids), 0);
MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the stub driver, format is "
		"\"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\""
		" and multiple comma separated entries can be specified");

/**
 * Checking if the PCI device matches the BUSID list.
 */
static struct pci_stub_busid *pci_stub_find_busid(struct pci_dev *dev)
{
	unsigned int i;
	unsigned int dev_domain = pci_domain_nr(dev->bus);
	unsigned int dev_bus = dev->bus->number;
	unsigned int dev_slot = PCI_SLOT(dev->devfn);
	unsigned int dev_func = PCI_FUNC(dev->devfn);

	for (i = 0; i < num_stub_busids; i++) {
		if (stub_busids[i].domain_specified) {
			if (dev_domain == stub_busids[i].domain &&
			    dev_bus == stub_busids[i].bus &&
			    dev_slot == stub_busids[i].device &&
			    dev_func == stub_busids[i].function)
				return &stub_busids[i];
		} else {
			if (dev_bus == stub_busids[i].bus &&
			    dev_slot == stub_busids[i].device &&
			    dev_func == stub_busids[i].function)
				return &stub_busids[i];
		}
	}

	return NULL;
}

static int pci_stub_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct pci_stub_busid *busid = pci_stub_find_busid(dev);

	if (busid) {
		pci_info(dev, "claimed by stub (BUSID match: %s)\n", pci_name(dev));
		return 0;
	}

	/*
	 * If BUSIDs are specified, id_table probing is not used.
	 * This prevents grabbing foreign devices with the same VID:DID.
	 */
	if (num_stub_busids > 0) {
		pci_info(dev, "rejected: not in BUSID list (VID:DID=%04x:%04x)\n",
			 dev->vendor, dev->device);
		return -ENODEV;
	}

	/*
	 * Standard operation mode if no BUSID is specified.
	 */
	if (id) {
		pci_info(dev, "claimed by stub (ID table match: %04x:%04x)\n",
			 id->vendor, id->device);
		return 0;
	}

	return -ENODEV;
}

static struct pci_driver stub_driver = {
	.name			= "pci-stub",
	.id_table		= NULL,	/* only dynamic id's */
	.probe			= pci_stub_probe,
	.driver_managed_dma	= true,
};

/**
 * Parsing a single BUSID.
 */
static int pci_stub_parse_one_busid(const char *str, struct pci_stub_busid *busid)
{
	unsigned int domain = 0, bus, dev, func;

	if (sscanf(str, "%x:%x:%x.%u", &domain, &bus, &dev, &func) == 4)
		busid->domain_specified = true;
	else if (sscanf(str, "%x:%x.%u", &bus, &dev, &func) == 3)
		busid->domain_specified = false;
	else
		return -EINVAL;

	if ((busid->domain_specified && domain > 0xffff)
	    || bus > 255 || dev > 31 || func > 7) {
		return -EINVAL;
	}
	busid->domain = domain;
	busid->bus = bus;
	busid->device = dev;
	busid->function = func;

	return 0;
}

/**
 * Parsing the "busid" kernel parameter.
 */
static void __init pci_stub_parse_busid_param(void)
{
	char *str, *token;
	int count = 0;

	if (!busid_param[0])
		return;

	pr_info("pci-stub: parsing busid param: %s\n", busid_param);

	str = kstrdup(busid_param, GFP_KERNEL);
	if (!str)
		return;

	for (token = str; token && *token && count < MAX_PCI_STUB_BUSIDS; ) {
		char *busid_str = token;
		char *next = strchr(token, ',');

		if (next) {
			*next = '\0';
			token = next + 1;
		} else {
			token = NULL;
		}

		while (*busid_str == ' ')
			busid_str++;

		if (!*busid_str)
			continue;

		if (pci_stub_parse_one_busid(busid_str,
					     &stub_busids[num_stub_busids + count]) == 0) {
			struct pci_stub_busid *busid = &stub_busids[num_stub_busids + count];

			if (busid->domain_specified) {
				pr_info("pci-stub: registered BUSID %04x:%02x:%02x.%u\n",
					busid->domain, busid->bus, busid->device, busid->function);
			} else {
				pr_info("pci-stub: registered BUSID %02x:%02x.%u\n",
					busid->bus, busid->device, busid->function);
			}
			count++;
		} else {
			pr_warn("pci-stub: invalid BUSID format: '%s'\n", busid_str);
		}
	}

	kfree(str);

	if (count > 0) {
		num_stub_busids += count;
		pr_info("pci-stub: registered %d BUSID(s)\n", count);
	}
}

/**
 * early_param "pci_stub_busid" handler for the built-in module.
 */
static int __init pci_stub_early_param(char *str)
{
	if (!str)
		return 0;

	pr_info("pci-stub: parsing early busid param: %s\n", str);
	strscpy(busid_param, str, sizeof(busid_param));
	return 0;
}

early_param("pci_stub_busid", pci_stub_early_param);

/**
 * Binding devices by BUSID via dynamic IDs.
 */
static void __init pci_stub_bind_free_devices(void)
{
	struct pci_dev *pdev = NULL;
	int bound = 0;
	int skipped = 0;

	if (!num_stub_busids)
		return;

	pr_info("pci-stub: attempting to bind devices by BUSID using dynamic IDs\n");

	while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL) {
		struct pci_stub_busid *busid = pci_stub_find_busid(pdev);

		if (!busid)
			continue;

		if (pdev->dev.driver == &stub_driver.driver) {
			pr_info("pci-stub: %s already bound to stub\n", pci_name(pdev));
			bound++;
			continue;
		}

		if (pdev->dev.driver) {
			pr_info("pci-stub: %s already bound to %s (skipping)\n",
				pci_name(pdev), pdev->dev.driver->name);
			skipped++;
			continue;
		}

		/* Adding a dynamic ID for this specific device */
		int rc = pci_add_dynid(&stub_driver, pdev->vendor, pdev->device,
				       PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0);
		if (rc) {
			pr_warn("pci-stub: failed to add dynamic ID for %s: %d\n",
				pci_name(pdev), rc);
			continue;
		}

		pr_info("pci-stub: added dynamic ID %04x:%04x for %s\n",
			pdev->vendor, pdev->device, pci_name(pdev));

		/* Check if device is now bound */
		if (pdev->dev.driver == &stub_driver.driver) {
			pr_info("pci-stub: successfully bound %s\n", pci_name(pdev));
			bound++;
			continue;
		}

		/* Force bind if not yet bound */
		int ret = device_driver_attach(&stub_driver.driver, &pdev->dev);

		if (ret < 0) {
			pr_warn("pci-stub: error binding %s: %d\n", pci_name(pdev), ret);
		} else {
			pr_info("pci-stub: successfully bound %s\n", pci_name(pdev));
			bound++;
		}
	}

	pr_info("pci-stub: bound %d device(s), skipped %d\n", bound, skipped);
}

/**
 * Checking the final binding state.
 */
static void __init pci_stub_verify_bindings(void)
{
	struct pci_dev *pdev = NULL;
	int total_found = 0;
	int bound_to_stub = 0;
	int bound_to_other = 0;
	int not_bound = 0;

	if (!num_stub_busids)
		return;

	pr_info("pci-stub: verifying device bindings...\n");

	while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL) {
		struct pci_stub_busid *busid = pci_stub_find_busid(pdev);

		if (!busid)
			continue;

		total_found++;

		if (pdev->dev.driver == &stub_driver.driver) {
			pr_info("pci-stub: ✓ %s is bound to stub\n", pci_name(pdev));
			bound_to_stub++;
		} else if (pdev->dev.driver) {
			pr_info("pci-stub: ✗ %s is bound to %s (not stub)\n",
				pci_name(pdev), pdev->dev.driver->name);
			bound_to_other++;
		} else {
			pr_info("pci-stub: ? %s is not bound to any driver\n", pci_name(pdev));
			not_bound++;
		}
	}

	pr_info("pci-stub: summary:\n");
	pr_info("  Total devices in BUSID list: %d\n", total_found);
	pr_info("  Bound to pci-stub: %d\n", bound_to_stub);
	pr_info("  Bound to other drivers: %d\n", bound_to_other);
	pr_info("  Not bound to any driver: %d\n", not_bound);

	if (bound_to_other > 0) {
		pr_warn("pci-stub: warning: %d device(s) are bound to other drivers\n",
			bound_to_other);
		pr_warn("pci-stub: use 'modprobe.blacklist' in kernel cmdline to "
			"prevent driver loading\n");
		pr_warn("pci-stub: or ensure pci-stub loads before other drivers\n");
	}
}

static int __init pci_stub_init(void)
{
	char *p, *id;
	int rc;

	pr_info("pci-stub: initializing\n");

	pci_stub_parse_busid_param();

	rc = pci_register_driver(&stub_driver);
	if (rc) {
		pr_err("pci-stub: failed to register driver: %d\n", rc);
		return rc;
	}

	if (ids[0] != '\0') {
		p = ids;
		while ((id = strsep(&p, ","))) {
			unsigned int vendor, device, subvendor = PCI_ANY_ID,
				     subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
			int fields;

			if (!strlen(id))
				continue;

			fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
					&vendor, &device, &subvendor, &subdevice,
					&class, &class_mask);
			if (fields < 2) {
				pr_warn("pci-stub: invalid ID string \"%s\"\n", id);
				continue;
			}

			rc = pci_add_dynid(&stub_driver, vendor, device,
					   subvendor, subdevice, class, class_mask, 0);
			if (rc)
				pr_warn("pci-stub: failed to add dynamic ID (%d)\n", rc);
		}
	}

	if (num_stub_busids > 0) {
		pr_info("pci-stub: driver registered with %d BUSID(s)\n", num_stub_busids);
		pci_stub_bind_free_devices();
		pci_stub_verify_bindings();
	}

	return 0;
}

static void __exit pci_stub_exit(void)
{
	pci_unregister_driver(&stub_driver);
}

module_init(pci_stub_init);
module_exit(pci_stub_exit);

MODULE_DESCRIPTION("VM device assignment stub driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Wright <chrisw@sous-sol.org>");
