// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Arm Limited
 */

#define pr_fmt(fmt) "smccc: " fmt

#include <linux/cache.h>
#include <linux/init.h>
#include <linux/arm-smccc.h>
#include <linux/kernel.h>
#include <linux/arm-smccc-bus.h>
#include <linux/idr.h>
#include <linux/slab.h>

#include <asm/archrandom.h>

#include "rmm.h"

static u32 smccc_version = ARM_SMCCC_VERSION_1_0;
static enum arm_smccc_conduit smccc_conduit = SMCCC_CONDUIT_NONE;
static DEFINE_IDA(arm_smccc_bus_id);

bool __ro_after_init smccc_trng_available = false;
s32 __ro_after_init smccc_soc_id_version = SMCCC_RET_NOT_SUPPORTED;
s32 __ro_after_init smccc_soc_id_revision = SMCCC_RET_NOT_SUPPORTED;

void __init arm_smccc_version_init(u32 version, enum arm_smccc_conduit conduit)
{
	struct arm_smccc_res res;

	smccc_version = version;
	smccc_conduit = conduit;

	smccc_trng_available = smccc_probe_trng();

	if ((smccc_version >= ARM_SMCCC_VERSION_1_2) &&
	    (smccc_conduit != SMCCC_CONDUIT_NONE)) {
		arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				     ARM_SMCCC_ARCH_SOC_ID, &res);
		if ((s32)res.a0 >= 0) {
			arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_SOC_ID, 0, &res);
			smccc_soc_id_version = (s32)res.a0;
			arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_SOC_ID, 1, &res);
			smccc_soc_id_revision = (s32)res.a0;
		}
	}
}

enum arm_smccc_conduit arm_smccc_1_1_get_conduit(void)
{
	if (smccc_version < ARM_SMCCC_VERSION_1_1)
		return SMCCC_CONDUIT_NONE;

	return smccc_conduit;
}
EXPORT_SYMBOL_GPL(arm_smccc_1_1_get_conduit);

u32 arm_smccc_get_version(void)
{
	return smccc_version;
}
EXPORT_SYMBOL_GPL(arm_smccc_get_version);

s32 arm_smccc_get_soc_id_version(void)
{
	return smccc_soc_id_version;
}

s32 arm_smccc_get_soc_id_revision(void)
{
	return smccc_soc_id_revision;
}
EXPORT_SYMBOL_GPL(arm_smccc_get_soc_id_revision);

bool arm_smccc_hypervisor_has_uuid(const uuid_t *hyp_uuid)
{
	struct arm_smccc_res res = {};
	uuid_t uuid;

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_CALL_UID_FUNC_ID, &res);
	if (res.a0 == SMCCC_RET_NOT_SUPPORTED)
		return false;

	uuid = smccc_res_to_uuid(res.a0, res.a1, res.a2, res.a3);
	return uuid_equal(&uuid, hyp_uuid);
}
EXPORT_SYMBOL_GPL(arm_smccc_hypervisor_has_uuid);

static int arm_smccc_bus_match(struct device *dev,
		const struct device_driver *drv)
{
	const struct arm_smccc_device_id *id_table;
	struct arm_smccc_device *smccc_dev = to_arm_smccc_device(dev);

	id_table = to_arm_smccc_driver(drv)->id_table;
	if (!id_table)
		return 0;

	while (id_table->name[0]) {
		if (!strcmp(smccc_dev->name, id_table->name))
			return 1;
		id_table++;
	}

	return 0;
}

static int arm_smccc_bus_probe(struct device *dev)
{
	struct arm_smccc_driver *smccc_drv = to_arm_smccc_driver(dev->driver);

	return smccc_drv->probe(to_arm_smccc_device(dev));
}

static void arm_smccc_bus_remove(struct device *dev)
{
	struct arm_smccc_driver *smcc_drv = to_arm_smccc_driver(dev->driver);

	if (smcc_drv->remove)
		smcc_drv->remove(to_arm_smccc_device(dev));
}

static int arm_smccc_bus_uevent(const struct device *dev,
		struct kobj_uevent_env *env)
{
	const struct arm_smccc_device *smccc_dev = to_arm_smccc_device(dev);

	return add_uevent_var(env, "MODALIAS=" ARM_SMCCC_MODULE_PREFIX "%s",
			      smccc_dev->name);
}

static ssize_t modalias_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct arm_smccc_device *smccc_dev = to_arm_smccc_device(dev);

	return sysfs_emit(buf, ARM_SMCCC_MODULE_PREFIX "%s\n", smccc_dev->name);
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *arm_smccc_device_attrs[] = {
	&dev_attr_modalias.attr,
	NULL,
};
ATTRIBUTE_GROUPS(arm_smccc_device);

const struct bus_type arm_smccc_bus_type = {
	.name = "arm_smccc",
	.match = arm_smccc_bus_match,
	.probe = arm_smccc_bus_probe,
	.remove = arm_smccc_bus_remove,
	.uevent = arm_smccc_bus_uevent,
	.dev_groups = arm_smccc_device_groups,
};
EXPORT_SYMBOL_GPL(arm_smccc_bus_type);

int arm_smccc_driver_register(struct arm_smccc_driver *driver,
		struct module *owner, const char *mod_name)
{
	if (!driver->probe)
		return -EINVAL;

	driver->driver.bus = &arm_smccc_bus_type;
	driver->driver.name = driver->name;
	driver->driver.owner = owner;
	driver->driver.mod_name = mod_name;

	return driver_register(&driver->driver);
}
EXPORT_SYMBOL_GPL(arm_smccc_driver_register);

void arm_smccc_driver_unregister(struct arm_smccc_driver *driver)
{
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL_GPL(arm_smccc_driver_unregister);

static void arm_smccc_release_device(struct device *dev)
{
	struct arm_smccc_device *smccc_dev = to_arm_smccc_device(dev);

	ida_free(&arm_smccc_bus_id, smccc_dev->id);
	kfree(smccc_dev);
}

struct arm_smccc_device *arm_smccc_device_register(const char *name)
{
	struct arm_smccc_device *smccc_dev;
	int id, ret;

	id = ida_alloc_min(&arm_smccc_bus_id, 1, GFP_KERNEL);
	if (id < 0)
		return ERR_PTR(id);

	smccc_dev = kzalloc_obj(*smccc_dev);
	if (!smccc_dev) {
		ida_free(&arm_smccc_bus_id, id);
		return ERR_PTR(-ENOMEM);
	}

	smccc_dev->id = id;
	if (strscpy(smccc_dev->name, name) < 0) {
		kfree(smccc_dev);
		ida_free(&arm_smccc_bus_id, id);
		return ERR_PTR(-EINVAL);
	}
	smccc_dev->dev.bus = &arm_smccc_bus_type;
	smccc_dev->dev.release = arm_smccc_release_device;

	ret = dev_set_name(&smccc_dev->dev, "%s-%d", smccc_dev->name, id);
	if (ret) {
		kfree(smccc_dev);
		ida_free(&arm_smccc_bus_id, id);
		return ERR_PTR(ret);
	}

	ret = device_register(&smccc_dev->dev);
	if (ret) {
		put_device(&smccc_dev->dev);
		return ERR_PTR(ret);
	}

	return smccc_dev;
}
EXPORT_SYMBOL_GPL(arm_smccc_device_register);

void arm_smccc_device_unregister(struct arm_smccc_device *smccc_dev)
{
	if (!smccc_dev)
		return;

	device_unregister(&smccc_dev->dev);
}
EXPORT_SYMBOL_GPL(arm_smccc_device_unregister);

static int __init arm_smccc_bus_init(void)
{
	return bus_register(&arm_smccc_bus_type);
}
subsys_initcall(arm_smccc_bus_init);

static int __init smccc_devices_init(void)
{
	/*
	 * Register the RMI and RSI devices only when firmware exposes
	 * the required SMCCC function IDs at a supported revision.
	 */
	register_rsi_device();

	if (smccc_trng_available) {
		struct arm_smccc_device *sdev;

		sdev = arm_smccc_device_register("arm-smccc-trng");
		if (IS_ERR(sdev))
			pr_err("arm-smccc-trng: could not register device: %ld\n",
			       PTR_ERR(sdev));
	}

	return 0;
}
device_initcall(smccc_devices_init);
