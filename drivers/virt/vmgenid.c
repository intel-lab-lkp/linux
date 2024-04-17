// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * The "Virtual Machine Generation ID" is exposed via ACPI and changes when a
 * virtual machine forks or is cloned. This driver exists for shepherding that
 * information to random.c.
 */

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/random.h>

ACPI_MODULE_NAME("vmgenid");

enum { VMGENID_SIZE = 16 };

struct vmgenid_state {
	u8 *next_id;
	u8 this_id[VMGENID_SIZE];
};

static void vmgenid_notify(struct device *device)
{
	struct vmgenid_state *state = device->driver_data;
	char *envp[] = { "NEW_VMGENID=1", NULL };
	u8 old_id[VMGENID_SIZE];

	memcpy(old_id, state->this_id, sizeof(old_id));
	memcpy(state->this_id, state->next_id, sizeof(state->this_id));
	if (!memcmp(old_id, state->this_id, sizeof(old_id)))
		return;
	add_vmfork_randomness(state->this_id, sizeof(state->this_id));
	kobject_uevent_env(&device->kobj, KOBJ_CHANGE, envp);
}

static void __maybe_unused
vmgenid_acpi_handler(acpi_handle __always_unused handle,
		     u32 __always_unused event, void *dev)
{
	vmgenid_notify(dev);
}

static int __maybe_unused
setup_vmgenid_state(struct vmgenid_state *state, u8 *next_id)
{
	if (IS_ERR(next_id))
		return PTR_ERR(next_id);

	state->next_id = next_id;
	memcpy(state->this_id, state->next_id, sizeof(state->this_id));
	add_device_randomness(state->this_id, sizeof(state->this_id));
	return 0;
}

static int vmgenid_add_acpi(struct device __maybe_unused *dev,
			    struct vmgenid_state __maybe_unused *state)
{
#if IS_ENABLED(CONFIG_ACPI)
	struct acpi_device *device = ACPI_COMPANION(dev);
	struct acpi_buffer parsed = { ACPI_ALLOCATE_BUFFER };
	union acpi_object *obj;
	phys_addr_t phys_addr;
	acpi_status status;
	int ret = 0;
	u8 *virt_addr;

	status = acpi_evaluate_object(device->handle, "ADDR", NULL, &parsed);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Evaluating ADDR"));
		return -ENODEV;
	}
	obj = parsed.pointer;
	if (!obj || obj->type != ACPI_TYPE_PACKAGE || obj->package.count != 2 ||
	    obj->package.elements[0].type != ACPI_TYPE_INTEGER ||
	    obj->package.elements[1].type != ACPI_TYPE_INTEGER) {
		ret = -EINVAL;
		goto out;
	}

	phys_addr = (obj->package.elements[0].integer.value << 0) |
		    (obj->package.elements[1].integer.value << 32);

	virt_addr = (u8 *)devm_memremap(&device->dev, phys_addr,
					VMGENID_SIZE, MEMREMAP_WB);
	ret = setup_vmgenid_state(state, virt_addr);
	if (ret)
		goto out;

	dev->driver_data = state;
	status = acpi_install_notify_handler(device->handle, ACPI_DEVICE_NOTIFY,
					     vmgenid_acpi_handler, dev);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to install acpi notify handler");
		ret = -ENODEV;
		dev->driver_data = NULL;
		goto out;
	}
out:
	ACPI_FREE(parsed.pointer);
	return ret;
#else
	return -EINVAL;
#endif
}

static int vmgenid_add(struct platform_device *pdev)
{
	struct vmgenid_state *state;
	struct device *dev = &pdev->dev;
	int ret = 0;

	state = devm_kmalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	ret = vmgenid_add_acpi(dev, state);

	if (ret)
		devm_kfree(dev, state);

	return ret;
}

static const struct acpi_device_id vmgenid_acpi_ids[] = {
	{ "VMGENCTR", 0 },
	{ "VM_GEN_COUNTER", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, vmgenid_acpi_ids);

static struct platform_driver vmgenid_plaform_driver = {
	.probe      = vmgenid_add,
	.driver     = {
		.name   = "vmgenid",
		.acpi_match_table = vmgenid_acpi_ids,
	},
};

module_platform_driver(vmgenid_plaform_driver)

MODULE_DESCRIPTION("Virtual Machine Generation ID");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jason A. Donenfeld <Jason@zx2c4.com>");
