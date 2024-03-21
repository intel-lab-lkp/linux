// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 *
 * The "Virtual Machine Generation ID" is exposed via ACPI or DT and changes when a
 * virtual machine forks or is cloned. This driver exists for shepherding that
 * information to random.c.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/random.h>
#include <acpi/actypes.h>
#include <linux/platform_device.h>
#ifdef CONFIG_OF
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#endif

#ifdef CONFIG_ACPI
ACPI_MODULE_NAME("vmgenid");
#endif

enum { VMGENID_SIZE = 16 };

struct vmgenid_state {
	u8 *next_id;
	u8 this_id[VMGENID_SIZE];
#ifdef CONFIG_OF
	unsigned int irq;
#endif
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

#ifdef	CONFIG_ACPI
static void vmgenid_acpi_handler(acpi_handle handle, u32 event, void *dev)
{
	(void)handle;
	(void)event;
	vmgenid_notify(dev);
}
#endif

#ifdef	CONFIG_OF
static irqreturn_t vmgenid_of_irq_handler(int irq, void *dev)
{
	(void)irq;
	vmgenid_notify(dev);

	return IRQ_HANDLED;
}
#endif

static int setup_vmgenid_state(struct vmgenid_state *state, u8 *next_id)
{
	if (IS_ERR(next_id))
		return PTR_ERR(next_id);

	state->next_id = next_id;
	memcpy(state->this_id, state->next_id, sizeof(state->this_id));
	add_device_randomness(state->this_id, sizeof(state->this_id));
	return 0;
}

static int vmgenid_add_acpi(struct device *dev, struct vmgenid_state *state)
{
#ifdef	CONFIG_ACPI
	struct acpi_device *device = ACPI_COMPANION(dev);
	struct acpi_buffer parsed = { ACPI_ALLOCATE_BUFFER };
	union acpi_object *obj;
	phys_addr_t phys_addr;
	acpi_status status;
	int ret = 0;

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

	ret = setup_vmgenid_state(state,
				  (u8 *)devm_memremap(&device->dev, phys_addr,
						      VMGENID_SIZE, MEMREMAP_WB));
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
	(void)dev;
	(void)state;
	return -EINVAL;
#endif
}

static int vmgenid_add_of(struct device *dev, struct vmgenid_state *state)
{
#ifdef	CONFIG_OF
	struct resource res;
	int ret = 0;

	if (of_address_to_resource(dev->of_node, 0, &res)) {
		dev_err(dev, "Failed to get resources from device tree");
		ret = -EINVAL;
		goto out;
	}

	if (!__request_mem_region(res.start, resource_size(&res),
				  "vmgenid", IORESOURCE_EXCLUSIVE)) {
		dev_err(dev, "Failed to request mem region");
		ret = -EINVAL;
		goto out;
	}

	ret = setup_vmgenid_state(state, (u8 *)of_iomap(dev->of_node, 0));
	if (ret)
		goto out;

	state->irq = irq_of_parse_and_map(dev->of_node, 0);
	dev->driver_data = state;

	if (request_irq(state->irq, vmgenid_of_irq_handler,
			IRQF_SHARED, "vmgenid", dev) < 0) {
		dev_err(dev, "request_irq failed");
		dev->driver_data = NULL;
		ret = -EINVAL;
		goto out;
	}

out:
	return ret;
#else
	(void)dev;
	(void)state;
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

	if (dev->of_node)
		ret = vmgenid_add_of(dev, state);
	else
		ret = vmgenid_add_acpi(dev, state);

	if (ret)
		devm_kfree(dev, state);

	return ret;
}

#ifdef	CONFIG_OF
static const struct of_device_id vmgenid_of_ids[] = {
	{ .compatible = "linux,vmgenctr", },
	{},
};
MODULE_DEVICE_TABLE(of, vmgenid_of_ids);
#endif

#ifdef	CONFIG_ACPI
static const struct acpi_device_id vmgenid_acpi_ids[] = {
	{ "VMGENCTR", 0 },
	{ "VM_GEN_COUNTER", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, vmgenid_acpi_ids);
#endif

static struct platform_driver vmgenid_plaform_driver = {
	.probe      = vmgenid_add,
	.driver     = {
		.name   = "vmgenid",
#ifdef	CONFIG_ACPI
		.acpi_match_table = ACPI_PTR(vmgenid_acpi_ids),
#endif
#ifdef	CONFIG_OF
		.of_match_table = vmgenid_of_ids,
#endif
		.owner = THIS_MODULE,
	},
};

static int vmgenid_platform_device_init(void)
{
	return platform_driver_register(&vmgenid_plaform_driver);
}

static void vmgenid_platform_device_exit(void)
{
	platform_driver_unregister(&vmgenid_plaform_driver);
}

module_init(vmgenid_platform_device_init)
module_exit(vmgenid_platform_device_exit)

MODULE_DESCRIPTION("Virtual Machine Generation ID");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jason A. Donenfeld <Jason@zx2c4.com>");
