// SPDX-License-Identifier: GPL-2.0
/*
 * ACPI PCI Hot Plug Ampere Altra Extension
 *
 * Copyright (C) 2023 Ampere Computing LLC
 *
 */

#define pr_fmt(fmt) "acpiphp_ampere_altra: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>

#include "acpiphp.h"

#define HANDLE_OPEN	0xb0200000
#define HANDLE_CLOSE	0xb0300000
#define REQUEST		0xf0700000
#define LED_CMD		0x00000004
#define LED_ATTENTION	0x00000002
#define LED_SET_ON	0x00000001
#define LED_SET_OFF	0x00000002
#define LED_SET_BLINK	0x00000003

static const struct acpi_device_id acpi_ids[] = {
	{"AMPC0008", 0}, {}
};
MODULE_DEVICE_TABLE(acpi, acpi_ids);

static u32 led_service_id[4];

static int led_status(u8 status)
{
	switch (status) {
	case 1: return LED_SET_ON;
	case 2: return LED_SET_BLINK;
	default: return LED_SET_OFF;
	}
}

static int set_attention_status(struct hotplug_slot *slot, u8 status)
{
	struct arm_smccc_res res;
	struct pci_bus *bus;
	struct pci_dev *root_port;
	unsigned long flags;
	u32 handle;
	int ret = 0;

	bus = slot->pci_slot->bus;
	root_port = pcie_find_root_port(bus->self);
	if (!root_port)
		return -ENODEV;

	local_irq_save(flags);
	arm_smccc_smc(HANDLE_OPEN, led_service_id[0], led_service_id[1],
		      led_service_id[2], led_service_id[3], 0, 0, 0, &res);
	if (res.a0) {
		ret = -ENODEV;
		goto out;
	}
	handle = res.a1 & 0xffff0000;

	arm_smccc_smc(REQUEST, LED_CMD, led_status(status), LED_ATTENTION,
		      pci_domain_nr(bus) | ((root_port->devfn >> 3) << 4), 0, 0,
		      handle, &res);
	if (res.a0)
		ret = -ENODEV;

	arm_smccc_smc(HANDLE_CLOSE, handle, 0, 0, 0, 0, 0, 0, &res);

 out:
	local_irq_restore(flags);
	return ret;
}

static int get_attention_status(struct hotplug_slot *slot, u8 *status)
{
	return -EINVAL;
}

static struct acpiphp_attention_info ampere_altra_attn = {
	.set_attn = set_attention_status,
	.get_attn = get_attention_status,
	.owner = THIS_MODULE,
};

static acpi_status __init get_acpi_handle(acpi_handle handle, u32 level,
					  void *context, void **return_value)
{
	*(acpi_handle *)return_value = handle;
	return AE_CTRL_TERMINATE;
}

static int __init acpiphp_ampere_altra_init(void)
{
	struct fwnode_handle *fwnode;
	acpi_handle leds_handle = NULL;
	struct acpi_device *leds;
	acpi_status status;
	int ret;

	status = acpi_get_devices("AMPC0008", get_acpi_handle, NULL,
				  &leds_handle);
	if (ACPI_FAILURE(status) || !leds_handle)
		return -ENODEV;
	leds = acpi_get_acpi_dev(leds_handle);
	if (!leds) {
		pr_err("can't find device\n");
		return -ENODEV;
	}

	fwnode = acpi_fwnode_handle(leds);
	ret = fwnode_property_read_u32_array(fwnode, "uuid", led_service_id, 4);
	acpi_put_acpi_dev(leds);
	if (ret) {
		pr_err("can't find uuid\n");
		return -ENODEV;
	}

	if (acpiphp_register_attention(&ampere_altra_attn)) {
		pr_err("can't register driver\n");
		return -ENODEV;
	}

	return 0;
}

module_init(acpiphp_ampere_altra_init);

static void __exit acpiphp_ampere_altra_exit(void)
{
	acpiphp_unregister_attention(&ampere_altra_attn);
}

module_exit(acpiphp_ampere_altra_exit);

MODULE_AUTHOR("D Scott Phillips <scott@os.amperecomputing.com>");
MODULE_LICENSE("GPL");
