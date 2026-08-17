// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 NXP
 * Generic I3C Hub core implementing virtual controller operations.
 */
#include <linux/i3c/device.h>
#include <linux/i3c/hub.h>

#include "internals.h"

/**
 * struct i3c_hub_dev_data - Per-downstream-device hub state
 * @parent_desc: Permanent parent-facing descriptor whose master points at the
 *		 physical parent controller, used to forward controller-specific
 *		 operations there.
 *
 * The logical descriptor on the virtual hub bus keeps its master aimed at the
 * virtual hub controller and is never modified. This separate descriptor lets
 * the controller-only core helpers resolve the physical parent without racing
 * concurrent readers on the virtual bus.
 */
struct i3c_hub_dev_data {
	struct i3c_dev_desc parent_desc;
};

/**
 * i3c_hub_master_bus_init() - Bind controller to hub device
 * @controller: Virtual controller for a hub port
 *
 * Associates the virtual controller with the hub device descriptor so that
 * transfers are executed through the hub on the parent bus.
 */
static int i3c_hub_master_bus_init(struct i3c_master_controller *controller)
{
	struct i3c_hub_controller *hub_controller;
	struct i3c_hub *hub;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return -ENODEV;

	hub = hub_controller->hub;

	if (!hub->hub_dev)
		return -ENODEV;

	controller->this = hub->hub_dev->desc;
	return 0;
}

static void i3c_hub_master_bus_cleanup(struct i3c_master_controller *controller)
{
	controller->this = NULL;
}

static int i3c_hub_attach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(dev);
	struct i3c_hub_controller *hub_controller;
	struct i3c_hub_dev_data *data;
	struct i3c_master_controller *parent;
	struct i3c_hub *hub;
	int ret;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return -ENODEV;

	hub = hub_controller->hub;
	if (!hub->hub_dev)
		return -ENODEV;

	parent = i3c_dev_get_master(hub->hub_dev->desc);
	if (!parent)
		return -ENODEV;

	data = kzalloc_obj(*data);
	if (!data)
		return -ENOMEM;

	/* Fix the parent-facing descriptor's master to the physical parent. */
	INIT_LIST_HEAD(&data->parent_desc.common.node);
	mutex_init(&data->parent_desc.ibi_lock);

	data->parent_desc.common.master = parent;
	data->parent_desc.info = dev->info;

	i3c_bus_maintenance_lock(&parent->bus);
	ret = i3c_master_attach_i3c_dev_controller(&data->parent_desc);
	i3c_bus_maintenance_unlock(&parent->bus);
	if (ret) {
		mutex_destroy(&data->parent_desc.ibi_lock);
		kfree(data);
		return ret;
	}

	/*
	 * The logical descriptor stores the hub-private data, while the
	 * parent-facing descriptor stores the physical controller's private
	 * data.
	 */
	i3c_dev_set_master_data(dev, data);

	return 0;
}

static int i3c_hub_reattach_i3c_dev(struct i3c_dev_desc *dev,
				    u8 old_dyn_addr)
{
	struct i3c_hub_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *parent;
	int ret;

	if (!data)
		return -ENODEV;

	parent = i3c_dev_get_master(&data->parent_desc);
	if (!parent)
		return -ENODEV;

	/* Re-sync device information after the address change. */
	data->parent_desc.info = dev->info;

	i3c_bus_maintenance_lock(&parent->bus);
	ret = i3c_master_reattach_i3c_dev_controller(&data->parent_desc,
						     old_dyn_addr);
	i3c_bus_maintenance_unlock(&parent->bus);

	return ret;
}

static void i3c_hub_detach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_hub_dev_data *data = i3c_dev_get_master_data(dev);
	struct i3c_master_controller *parent;

	if (!data)
		return;

	parent = i3c_dev_get_master(&data->parent_desc);

	/*
	 * The generic IBI lifecycle must be released before detaching the
	 * physical controller state.
	 */
	WARN_ON(data->parent_desc.ibi);

	if (parent) {
		i3c_bus_maintenance_lock(&parent->bus);
		i3c_master_detach_i3c_dev_controller(&data->parent_desc);
		i3c_bus_maintenance_unlock(&parent->bus);
	}

	i3c_dev_set_master_data(dev, NULL);
	mutex_destroy(&data->parent_desc.ibi_lock);
	kfree(data);
}

/**
 * i3c_hub_do_daa() - Perform DAA via hub port
 * @hub: Hub instance
 * @controller: Virtual controller for a hub port
 *
 * Enables the port connection, performs DAA on the parent controller,
 * then disables the connection.
 */
static int i3c_hub_do_daa(struct i3c_hub *hub,
			  struct i3c_master_controller *controller)
{
	struct i3c_master_controller *parent;
	int ret;

	if (!hub || !hub->hub_dev)
		return -ENODEV;

	parent = i3c_dev_get_master(hub->hub_dev->desc);
	if (!parent)
		return -ENODEV;

	mutex_lock(&hub->lock);
	i3c_hub_enable_port(controller);

	/*
	 * Downstream devices reachable through hub target-port routes share the
	 * parent controller's I3C address space. The hub gates access to a
	 * target-port network, but it does not create an independent dynamic
	 * address domain per virtual bus.
	 *
	 * Run DAA on the parent controller so dynamic addresses remain unique
	 * across all downstream devices, even when they are behind different
	 * target ports.
	 */
	ret = i3c_master_do_daa(parent);
	i3c_hub_disable_port(controller);
	mutex_unlock(&hub->lock);

	return ret;
}

static bool i3c_hub_supports_ccc_cmd(struct i3c_hub *hub,
				     const struct i3c_ccc_cmd *cmd)
{
	struct i3c_master_controller *parent;

	if (!hub || !hub->hub_dev)
		return false;

	parent = i3c_dev_get_master(hub->hub_dev->desc);
	if (!parent)
		return false;

	return i3c_master_supports_ccc_cmd(parent, cmd);
}

/**
 * i3c_hub_send_ccc_cmd() - Send CCC through hub port
 * @hub: Hub instance
 * @controller: Virtual controller
 * @cmd: CCC command
 *
 * Enables the port connection while issuing CCC on the parent controller.
 */
static int i3c_hub_send_ccc_cmd(struct i3c_hub *hub,
				struct i3c_master_controller *controller,
				struct i3c_ccc_cmd *cmd)
{
	struct i3c_master_controller *parent;
	int ret;

	if (!hub || !hub->hub_dev)
		return -ENODEV;

	parent = i3c_dev_get_master(hub->hub_dev->desc);
	if (!parent)
		return -ENODEV;

	mutex_lock(&hub->lock);
	i3c_hub_enable_port(controller);
	ret = i3c_master_send_ccc_cmd(parent, cmd);
	i3c_hub_disable_port(controller);
	mutex_unlock(&hub->lock);

	return ret;
}

/**
 * i3c_hub_master_priv_xfers() - Execute private transfers via hub
 * @dev: Target device descriptor
 * @xfers: Transfer array
 * @nxfers: Number of transfers
 * @mode: Transfer mode (SDR, HDR, etc.)
 *
 * Handles address adjustment and forwards private transfers through the hub
 * device.
 */
static int i3c_hub_master_priv_xfers(struct i3c_dev_desc *dev,
				     struct i3c_xfer *xfers,
				     int nxfers,
				     enum i3c_xfer_mode mode)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(dev);
	struct i3c_hub_controller *hub_controller;
	struct i3c_master_controller *parent;
	struct i3c_hub_dev_data *data;
	struct i3c_hub *hub;
	int ret;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return -ENODEV;

	hub = hub_controller->hub;

	data = i3c_dev_get_master_data(dev);
	if (!data)
		return -ENODEV;

	parent = i3c_dev_get_master(&data->parent_desc);
	if (!parent)
		return -ENODEV;

	mutex_lock(&hub->lock);

	/*
	 * Parent-facing device information may be refreshed before IBI
	 * resources are requested; once IBI resources are requested, the
	 * information remains immutable while the parent controller may use
	 * the descriptor asynchronously.
	 */
	if (!data->parent_desc.ibi)
		data->parent_desc.info = dev->info;

	i3c_hub_enable_port(controller);

	i3c_bus_normaluse_lock(&parent->bus);
	ret = i3c_dev_do_xfers_locked(&data->parent_desc, xfers,
				      nxfers, mode);
	i3c_bus_normaluse_unlock(&parent->bus);

	i3c_hub_disable_port(controller);

	mutex_unlock(&hub->lock);

	return ret;
}

static int i3c_hub_attach_i2c_dev(struct i2c_dev_desc *dev)
{
	return -EOPNOTSUPP;
}

static void i3c_hub_detach_i2c_dev(struct i2c_dev_desc *dev)
{
}

static int i3c_hub_i2c_xfers(struct i2c_dev_desc *dev,
			     struct i2c_msg *xfers, int nxfers)
{
	return -EOPNOTSUPP;
}

static int i3c_hub_master_do_daa(struct i3c_master_controller *controller)
{
	struct i3c_hub_controller *hub_controller;
	struct i3c_hub *hub;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return -ENODEV;

	hub = hub_controller->hub;

	return i3c_hub_do_daa(hub, controller);
}

static int i3c_hub_master_send_ccc_cmd(struct i3c_master_controller *controller,
				       struct i3c_ccc_cmd *cmd)
{
	struct i3c_hub_controller *hub_controller;
	struct i3c_hub *hub;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return -ENODEV;

	hub = hub_controller->hub;

	if (!hub->hub_dev)
		return -ENODEV;

	/*
	 * Do not forward broadcast RSTDAA through the hub. The hub itself
	 * is visible on the parent bus, so forwarding RSTDAA would also
	 * reset the hub dynamic address. Downstream RSTDAA is not supported
	 * by the hub virtual-controller model.
	 */
	if (cmd->id == I3C_CCC_RSTDAA(true))
		return 0;

	return i3c_hub_send_ccc_cmd(hub, controller, cmd);
}

static bool i3c_hub_master_supports_ccc_cmd(struct i3c_master_controller *controller,
					    const struct i3c_ccc_cmd *cmd)
{
	struct i3c_hub_controller *hub_controller;
	struct i3c_hub *hub;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return false;

	hub = hub_controller->hub;

	return i3c_hub_supports_ccc_cmd(hub, cmd);
}

/**
 * i3c_hub_request_ibi() - Request IBI through parent controller
 * @desc: Target device descriptor
 * @req: IBI setup
 *
 * Publishes the generic IBI object on the permanent parent-facing descriptor
 * and requests IBI for a device connected through the hub. The parent-facing
 * descriptor references the same IBI object so the physical controller uses
 * the logical workqueue, pending counter and client device during
 * asynchronous IBI delivery.
 */
static int i3c_hub_request_ibi(struct i3c_dev_desc *desc,
			       const struct i3c_ibi_setup *req)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);
	struct i3c_hub_controller *hub_controller;
	struct i3c_master_controller *parent;
	struct i3c_hub_dev_data *data;
	struct i3c_hub *hub;
	int ret;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return -ENODEV;

	hub = hub_controller->hub;

	data = i3c_dev_get_master_data(desc);
	if (!data)
		return -ENODEV;

	parent = i3c_dev_get_master(&data->parent_desc);
	if (!parent)
		return -ENODEV;

	/*
	 * Publish the final device information snapshot together with the
	 * generic IBI object under hub->lock. Keep the parent-facing
	 * information immutable while parent_desc.ibi is set and the parent
	 * controller may use the descriptor asynchronously.
	 */
	mutex_lock(&hub->lock);
	data->parent_desc.info = desc->info;
	data->parent_desc.dev = desc->dev;
	data->parent_desc.ibi = desc->ibi;
	mutex_unlock(&hub->lock);

	i3c_bus_normaluse_lock(&parent->bus);
	ret = i3c_dev_request_ibi_controller_locked(&data->parent_desc, req);
	i3c_bus_normaluse_unlock(&parent->bus);

	if (ret) {
		mutex_lock(&hub->lock);
		data->parent_desc.ibi = NULL;
		data->parent_desc.dev = NULL;
		mutex_unlock(&hub->lock);
	}

	return ret;
}

static void i3c_hub_free_ibi(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);
	struct i3c_hub_controller *hub_controller;
	struct i3c_master_controller *parent;
	struct i3c_hub_dev_data *data;
	struct i3c_hub *hub;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return;

	hub = hub_controller->hub;

	data = i3c_dev_get_master_data(desc);
	if (!data || !data->parent_desc.ibi)
		return;

	parent = i3c_dev_get_master(&data->parent_desc);
	if (!parent)
		return;

	i3c_bus_normaluse_lock(&parent->bus);
	i3c_dev_free_ibi_controller_locked(&data->parent_desc);
	i3c_bus_normaluse_unlock(&parent->bus);

	/*
	 * The outer generic IBI free path owns and releases desc->ibi after
	 * this callback returns.
	 */
	mutex_lock(&hub->lock);
	data->parent_desc.ibi = NULL;
	data->parent_desc.dev = NULL;
	mutex_unlock(&hub->lock);
}

/**
 * i3c_hub_enable_ibi() - Enable IBI via hub port
 * @desc: Target device descriptor
 *
 * Enables port connection and forwards the IBI enable request to the parent
 * controller.
 */
static int i3c_hub_enable_ibi(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);
	struct i3c_hub_controller *hub_controller;
	struct i3c_master_controller *parent;
	struct i3c_hub_dev_data *data;
	struct i3c_hub *hub;
	int ret;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return -ENODEV;

	hub = hub_controller->hub;

	data = i3c_dev_get_master_data(desc);
	if (!data || !data->parent_desc.ibi)
		return -ENODEV;

	parent = i3c_dev_get_master(&data->parent_desc);
	if (!parent)
		return -ENODEV;

	mutex_lock(&hub->lock);

	i3c_hub_enable_port(controller);

	i3c_bus_maintenance_lock(&parent->bus);
	ret = i3c_dev_enable_ibi_controller_locked(&data->parent_desc);
	i3c_bus_maintenance_unlock(&parent->bus);

	i3c_hub_disable_port(controller);

	mutex_unlock(&hub->lock);

	return ret;
}

/**
 * i3c_hub_disable_ibi() - Disable IBI via hub port
 * @desc: Target device descriptor
 *
 * Enables port connection and forwards the IBI disable request to the parent
 * controller.
 */
static int i3c_hub_disable_ibi(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);
	struct i3c_hub_controller *hub_controller;
	struct i3c_master_controller *parent;
	struct i3c_hub_dev_data *data;
	struct i3c_hub *hub;
	int ret;

	hub_controller = dev_get_drvdata(&controller->dev);
	if (!hub_controller || !hub_controller->hub)
		return -ENODEV;

	hub = hub_controller->hub;

	data = i3c_dev_get_master_data(desc);
	if (!data || !data->parent_desc.ibi)
		return -ENODEV;

	parent = i3c_dev_get_master(&data->parent_desc);
	if (!parent)
		return -ENODEV;

	mutex_lock(&hub->lock);

	i3c_hub_enable_port(controller);

	i3c_bus_maintenance_lock(&parent->bus);
	ret = i3c_dev_disable_ibi_controller_locked(&data->parent_desc);
	i3c_bus_maintenance_unlock(&parent->bus);

	i3c_hub_disable_port(controller);

	mutex_unlock(&hub->lock);

	return ret;
}

static void i3c_hub_recycle_ibi_slot(struct i3c_dev_desc *desc,
				     struct i3c_ibi_slot *slot)
{
	struct i3c_hub_dev_data *data = i3c_dev_get_master_data(desc);

	if (!data)
		return;

	i3c_dev_recycle_ibi_slot_controller(&data->parent_desc, slot);
}

static const struct i3c_master_controller_ops i3c_hub_master_ops_data = {
	.bus_init = i3c_hub_master_bus_init,
	.bus_cleanup = i3c_hub_master_bus_cleanup,
	.attach_i3c_dev = i3c_hub_attach_i3c_dev,
	.reattach_i3c_dev = i3c_hub_reattach_i3c_dev,
	.detach_i3c_dev = i3c_hub_detach_i3c_dev,
	.do_daa = i3c_hub_master_do_daa,
	.supports_ccc_cmd = i3c_hub_master_supports_ccc_cmd,
	.send_ccc_cmd = i3c_hub_master_send_ccc_cmd,
	.i3c_xfers = i3c_hub_master_priv_xfers,
	.attach_i2c_dev = i3c_hub_attach_i2c_dev,
	.detach_i2c_dev = i3c_hub_detach_i2c_dev,
	.i2c_xfers = i3c_hub_i2c_xfers,
	.request_ibi = i3c_hub_request_ibi,
	.free_ibi = i3c_hub_free_ibi,
	.enable_ibi = i3c_hub_enable_ibi,
	.disable_ibi = i3c_hub_disable_ibi,
	.recycle_ibi_slot = i3c_hub_recycle_ibi_slot,
};

/**
 * i3c_hub_init() - Initialize hub context
 * @hub: Hub instance
 * @ops: Vendor callbacks
 * @hub_dev: I3C hub device
 */
void i3c_hub_init(struct i3c_hub *hub,
		  const struct i3c_hub_ops *ops,
		  struct i3c_device *hub_dev)
{
	hub->ops = ops;
	hub->hub_dev = hub_dev;
	mutex_init(&hub->lock);
}
EXPORT_SYMBOL_GPL(i3c_hub_init);

const struct i3c_master_controller_ops *i3c_hub_master_ops(void)
{
	return &i3c_hub_master_ops_data;
}
EXPORT_SYMBOL_GPL(i3c_hub_master_ops);

/**
 * i3c_hub_reserve_parent_addrslots_from_dt() - Reserve child addresses in parent bus.
 * @hubc: I3C hub controller for a target-port virtual bus.
 * @node: Target-port bus Device Tree node.
 *
 * Reserve parent bus address slots for downstream I3C devices that keep the
 * same static and assigned dynamic address, so parent DAA does not reuse them.
 *
 * Return: 0 on success, or a negative error code.
 */
int i3c_hub_reserve_parent_addrslots_from_dt(struct i3c_hub_controller *hubc,
					     struct device_node *node)
{
	struct i3c_master_controller *parent = hubc->parent;
	enum i3c_addr_slot_status status;
	u32 assigned_addr;
	u8 static_addr;
	u32 reg[3];
	int ret;

	if (!parent || !node)
		return -ENODEV;

	for_each_available_child_of_node_scoped(node, child) {
		ret = of_property_read_variable_u32_array(child, "reg", reg, 1, 3);
		if (ret < 0)
			continue;

		ret = of_property_read_u32(child, "assigned-address", &assigned_addr);
		if (ret)
			continue;

		static_addr = reg[0];

		if (!static_addr || !assigned_addr)
			continue;

		if (static_addr != assigned_addr)
			continue;

		i3c_bus_maintenance_lock(&parent->bus);
		status = i3c_bus_get_addr_slot_status(&parent->bus,
						      assigned_addr);
		if (status == I3C_ADDR_SLOT_FREE)
			i3c_bus_set_addr_slot_status(&parent->bus,
						     assigned_addr,
						     I3C_ADDR_SLOT_I3C_DEV);
		i3c_bus_maintenance_unlock(&parent->bus);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(i3c_hub_reserve_parent_addrslots_from_dt);

MODULE_AUTHOR("Aman Kumar Pandey <aman.kumarpandey@nxp.com>");
MODULE_AUTHOR("Vikash Bansal <vikash.bansal@nxp.com>");
MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("Generic I3C hub support");
MODULE_LICENSE("GPL");
