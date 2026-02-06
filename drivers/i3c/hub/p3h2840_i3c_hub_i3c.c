// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 NXP
 * This P3H2x4x driver file contain functions for I3C virtual Bus creation, connect/disconnect
 * hub network and read/write.
 */
#include <linux/mfd/p3h2840.h>
#include <linux/regmap.h>

#include "p3h2840_i3c_hub.h"

static const struct i3c_ibi_setup p3h2x4x_ibireq = {
	.handler = p3h2x4x_ibi_handler,
	.max_payload_len = P3H2x4x_MAX_PAYLOAD_LEN,
	.num_slots = P3H2x4x_NUM_SLOTS,
};

static void p3h2x4x_en_p3h2x4x_ntwk_tp(struct tp_bus *bus)
{
	struct p3h2x4x_i3c_hub_dev *p3h2x4x_i3c_hub = bus->p3h2x4x_i3c_hub;

	if (p3h2x4x_i3c_hub->hub_config.tp_config[bus->tp_port].always_enable)
		return;

	regmap_set_bits(p3h2x4x_i3c_hub->regmap, P3H2x4x_TP_NET_CON_CONF, bus->tp_mask);
}

static void p3h2x4x_dis_p3h2x4x_ntwk_tp(struct tp_bus *bus)
{
	struct p3h2x4x_i3c_hub_dev *p3h2x4x_i3c_hub = bus->p3h2x4x_i3c_hub;

	if (p3h2x4x_i3c_hub->hub_config.tp_config[bus->tp_port].always_enable)
		return;

	regmap_clear_bits(p3h2x4x_i3c_hub->regmap, P3H2x4x_TP_NET_CON_CONF, bus->tp_mask);
}

static struct tp_bus *p3h2x4x_bus_from_i3c_desc(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);

	return container_of(controller, struct tp_bus, tp_i3c_controller);
}

static struct i3c_master_controller
*get_parent_controller(struct i3c_master_controller *controller)
{
	struct tp_bus *bus = container_of(controller, struct tp_bus, tp_i3c_controller);

	return i3c_dev_get_master(bus->p3h2x4x_i3c_hub->i3cdev->desc);
}

static struct i3c_master_controller
*get_parent_controller_from_i3c_desc(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);
	struct tp_bus *bus = container_of(controller, struct tp_bus, tp_i3c_controller);

	return i3c_dev_get_master(bus->p3h2x4x_i3c_hub->i3cdev->desc);
}

static struct i3c_master_controller
*update_i3c_i2c_desc_parent(struct i3c_i2c_dev_desc *desc,
				struct i3c_master_controller *parent)
{
	struct i3c_master_controller *orig_parent = desc->master;

	desc->master = parent;

	return orig_parent;
}

static void restore_i3c_i2c_desc_parent(struct i3c_i2c_dev_desc *desc,
					struct i3c_master_controller *parent)
{
	desc->master = parent;
}

static int p3h2x4x_i3c_bus_init(struct i3c_master_controller *controller)
{
	struct tp_bus *bus = container_of(controller, struct tp_bus, tp_i3c_controller);

	controller->this = bus->p3h2x4x_i3c_hub->i3cdev->desc;
	return 0;
}

static void p3h2x4x_i3c_bus_cleanup(struct i3c_master_controller *controller)
{
	controller->this = NULL;
}

static int p3h2x4x_attach_i3c_dev(struct i3c_dev_desc *dev)
{
	return 0;
}

static int p3h2x4x_reattach_i3c_dev(struct i3c_dev_desc *dev, u8 old_dyn_addr)
{
	return 0;
}

static void p3h2x4x_detach_i3c_dev(struct i3c_dev_desc *dev)
{
}

static int p3h2x4x_do_daa(struct i3c_master_controller *controller)
{
	struct tp_bus *bus = container_of(controller, struct tp_bus, tp_i3c_controller);
	struct i3c_master_controller *parent = get_parent_controller(controller);
	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	ret = i3c_master_do_daa(parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);

	return ret;
}

static bool p3h2x4x_supports_ccc_cmd(struct i3c_master_controller *controller,
				     const struct i3c_ccc_cmd *cmd)
{
	struct i3c_master_controller *parent = get_parent_controller(controller);

	return i3c_master_supports_ccc_cmd(parent, cmd);
}

static int p3h2x4x_send_ccc_cmd(struct i3c_master_controller *controller,
				struct i3c_ccc_cmd *cmd)
{
	struct tp_bus *bus = container_of(controller, struct tp_bus, tp_i3c_controller);
	struct i3c_master_controller *parent = get_parent_controller(controller);
	int ret;

	if (cmd->id == I3C_CCC_RSTDAA(true))
		return 0;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	ret = i3c_master_send_ccc_cmd(parent, cmd);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);

	return ret;
}

static int p3h2x4x_i3c_xfers(struct i3c_dev_desc *dev,
			     struct i3c_priv_xfer *xfers, int nxfers,
			     enum i3c_xfer_mode mode)
{
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(dev);
	struct i3c_dev_desc *hub_dev = bus->p3h2x4x_i3c_hub->i3cdev->desc;
	u8 hub_addr, target_addr;
	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);

	/* hub’s current address */
	hub_addr = hub_dev->info.dyn_addr ? hub_dev->info.dyn_addr :
		hub_dev->info.static_addr;

	/* Target device address */
	target_addr = dev->info.dyn_addr ? dev->info.dyn_addr :
					     dev->info.static_addr;

	/* Only reattach if the address is different */
	if (hub_addr != target_addr) {
		hub_dev->info.dyn_addr = target_addr;
		ret = i3c_master_reattach_i3c_dev(hub_dev, target_addr);
		if (ret)
			goto disable_ntwk;
	}

	ret = i3c_device_do_priv_xfers(bus->p3h2x4x_i3c_hub->i3cdev, xfers, nxfers);

	/* Restore hub’s original address if it was changed */
	if (hub_addr != target_addr) {
		hub_dev->info.dyn_addr = hub_addr;
		ret |= i3c_master_reattach_i3c_dev(hub_dev, hub_addr);
	}

disable_ntwk:
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);
	return ret;
}

static int p3h2x4x_attach_i2c_dev(struct i2c_dev_desc *dev)
{
	return 0;
}

static void p3h2x4x_detach_i2c_dev(struct i2c_dev_desc *dev)
{
}

static int p3h2x4x_i2c_xfers(struct i2c_dev_desc *dev,
			     struct i2c_msg *xfers, int nxfers)
{
	return 0;
}

static int p3h2x4x_request_ibi(struct i3c_dev_desc *desc,
			       const struct i3c_ibi_setup *req)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(desc);
	struct i3c_master_controller *orig_parent;
	int ret;

	orig_parent = update_i3c_i2c_desc_parent(&desc->common, parent);
	ret = i3c_master_direct_attach_i3c_dev(parent, desc);
	if (ret) {
		restore_i3c_i2c_desc_parent(&desc->common, orig_parent);
		return ret;
	}

	mutex_unlock(&desc->ibi_lock);
	kfree(desc->ibi);
	desc->ibi = NULL;
	ret = i3c_device_request_ibi(desc->dev, req);
	mutex_lock(&desc->ibi_lock);
	restore_i3c_i2c_desc_parent(&desc->common, orig_parent);

	return ret;
}

static void p3h2x4x_free_ibi(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(desc);
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(desc);
	struct i3c_master_controller *orig_parent;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);

	orig_parent = update_i3c_i2c_desc_parent(&desc->common, parent);
	i3c_master_direct_detach_i3c_dev(desc);
	mutex_unlock(&desc->ibi_lock);
	i3c_device_free_ibi(desc->dev);
	mutex_lock(&desc->ibi_lock);
	restore_i3c_i2c_desc_parent(&desc->common, orig_parent);

	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);
}

static int p3h2x4x_enable_ibi(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(desc);
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(desc);
	struct i3c_master_controller *orig_parent;
	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	orig_parent = update_i3c_i2c_desc_parent(&desc->common, parent);

	down_write(&bus->p3h2x4x_i3c_hub->i3cdev->bus->lock);
	mutex_unlock(&desc->ibi_lock);
	ret = i3c_device_enable_ibi(desc->dev);
	mutex_lock(&desc->ibi_lock);
	up_write(&bus->p3h2x4x_i3c_hub->i3cdev->bus->lock);

	restore_i3c_i2c_desc_parent(&desc->common, orig_parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);

	return ret;
}

static int p3h2x4x_disable_ibi(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(desc);
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(desc);
	struct i3c_master_controller *orig_parent;
	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	orig_parent = update_i3c_i2c_desc_parent(&desc->common, parent);

	down_write(&bus->p3h2x4x_i3c_hub->i3cdev->bus->lock);
	mutex_unlock(&desc->ibi_lock);
	ret = i3c_device_disable_ibi(desc->dev);
	mutex_lock(&desc->ibi_lock);
	up_write(&bus->p3h2x4x_i3c_hub->i3cdev->bus->lock);

	restore_i3c_i2c_desc_parent(&desc->common, orig_parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);

	return ret;
}

static void p3h2x4x_recycle_ibi_slot(struct i3c_dev_desc *desc,
				     struct i3c_ibi_slot *slot)
{
}

static const struct i3c_master_controller_ops p3h2x4x_i3c_ops = {
	.bus_init = p3h2x4x_i3c_bus_init,
	.bus_cleanup = p3h2x4x_i3c_bus_cleanup,
	.attach_i3c_dev = p3h2x4x_attach_i3c_dev,
	.reattach_i3c_dev = p3h2x4x_reattach_i3c_dev,
	.detach_i3c_dev = p3h2x4x_detach_i3c_dev,
	.do_daa = p3h2x4x_do_daa,
	.supports_ccc_cmd = p3h2x4x_supports_ccc_cmd,
	.send_ccc_cmd = p3h2x4x_send_ccc_cmd,
	.i3c_xfers = p3h2x4x_i3c_xfers,
	.attach_i2c_dev = p3h2x4x_attach_i2c_dev,
	.detach_i2c_dev = p3h2x4x_detach_i2c_dev,
	.i2c_xfers = p3h2x4x_i2c_xfers,
	.request_ibi = p3h2x4x_request_ibi,
	.free_ibi = p3h2x4x_free_ibi,
	.enable_ibi = p3h2x4x_enable_ibi,
	.disable_ibi = p3h2x4x_disable_ibi,
	.recycle_ibi_slot = p3h2x4x_recycle_ibi_slot,
};

/**
 * p3h2x4x_tp_i3c_algo - register i3c master for target port who
 * configured as i3c.
 * @hub: p3h2x4x device structure.
 * Return: 0 in case of success, negative error code on failur.
 */
int p3h2x4x_tp_i3c_algo(struct p3h2x4x_i3c_hub_dev *hub)
{
	u8 tp, ntwk_mask = 0;
	int ret;

	for (tp = 0; tp < P3H2x4x_TP_MAX_COUNT; tp++) {
		if (!hub->tp_bus[tp].of_node ||
		    hub->hub_config.tp_config[tp].mode != P3H2x4x_TP_MODE_I3C)
			continue;

		/* Assign DT node for this TP */
		hub->dev->of_node = hub->tp_bus[tp].of_node;

		/* Register I3C master for this TP */
		ret = i3c_master_register(&hub->tp_bus[tp].tp_i3c_controller,
					  hub->dev, &p3h2x4x_i3c_ops, false);
		if (ret)
			return ret;

		/* Perform DAA */
		ret = i3c_master_do_daa(i3c_dev_get_master(hub->i3cdev->desc));
		if (ret)
			return ret;

		ntwk_mask |= hub->tp_bus[tp].tp_mask;
		hub->tp_bus[tp].is_registered = true;
		hub->hub_config.tp_config[tp].always_enable = true;
	}

	ret = i3c_device_request_ibi(hub->i3cdev, &p3h2x4x_ibireq);
	if (ret)
		return ret;

	ret = i3c_device_enable_ibi(hub->i3cdev);
	if (ret)
		return ret;

	return regmap_write(hub->regmap, P3H2x4x_TP_NET_CON_CONF, ntwk_mask);
}
