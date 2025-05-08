// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 NXP
 * This P3H2x4x driver file contain functions for I3C virtual Bus creation, connect/disconnect
 * hub network and read/write.
 */
#include "p3h2840_i3c_hub.h"

static void p3h2x4x_en_p3h2x4x_ntwk_tp(struct tp_bus *bus)
{
	struct p3h2x4x *priv = bus->priv;
	struct device *dev = i3cdev_to_dev(priv->i3cdev);
	int ret;

	if (priv->settings.tp[bus->tp_port].always_enable)
		return;

	ret = regmap_write(priv->regmap, P3H2x4x_TP_NET_CON_CONF,
			   (bus->tp_mask | priv->tp_always_enable_mask));
	if (ret)
		dev_warn(dev, "Failed to open Target Port(s)\n");
}

static void p3h2x4x_dis_p3h2x4x_ntwk_tp(struct tp_bus *bus)
{
	struct p3h2x4x *priv = bus->priv;
	struct device *dev = i3cdev_to_dev(priv->i3cdev);
	int ret;

	if (priv->settings.tp[bus->tp_port].always_enable)
		return;

	ret = regmap_write(priv->regmap, P3H2x4x_TP_NET_CON_CONF, priv->tp_always_enable_mask);
	if (ret)
		dev_warn(dev, "Failed to close Target Port(s)\n");
}

static struct tp_bus *p3h2x4x_bus_from_i3c_desc(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);

	return container_of(controller, struct tp_bus, i3c_port_controller);
}

static struct tp_bus *p3h2x4x_bus_from_i2c_desc(struct i2c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i2c_dev_get_master(desc);

	return container_of(controller, struct tp_bus, i3c_port_controller);
}

static struct i3c_master_controller
*get_parent_controller(struct i3c_master_controller *controller)
{
	struct tp_bus *bus = container_of(controller, struct tp_bus, i3c_port_controller);

	return bus->priv->driving_master;
}

static struct i3c_master_controller
*get_parent_controller_from_i3c_desc(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);
	struct tp_bus *bus = container_of(controller, struct tp_bus, i3c_port_controller);

	return bus->priv->driving_master;
}

static struct i3c_master_controller
*get_parent_controller_from_i2c_desc(struct i2c_dev_desc *desc)
{
	struct i3c_master_controller *controller = desc->common.master;
	struct tp_bus *bus = container_of(controller, struct tp_bus, i3c_port_controller);

	return bus->priv->driving_master;
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
	struct tp_bus *bus = container_of(controller, struct tp_bus, i3c_port_controller);

	controller->this = bus->priv->i3cdev->desc;
	return 0;
}

static void p3h2x4x_i3c_bus_cleanup(struct i3c_master_controller *controller)
{
	controller->this = NULL;
}

static int p3h2x4x_attach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->attach_i3c_dev(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	return ret;
}

static int p3h2x4x_reattach_i3c_dev(struct i3c_dev_desc *dev, u8 old_dyn_addr)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->reattach_i3c_dev(dev, old_dyn_addr);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	return ret;
}

static void p3h2x4x_detach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	parent->ops->detach_i3c_dev(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
}

static int p3h2x4x_do_daa(struct i3c_master_controller *controller)
{
	struct tp_bus *bus = container_of(controller, struct tp_bus, i3c_port_controller);
	struct i3c_master_controller *parent = get_parent_controller(controller);

	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	down_write(&parent->bus.lock);
	ret = parent->ops->do_daa(parent);
	up_write(&parent->bus.lock);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);

	return ret;
}

static bool p3h2x4x_supports_ccc_cmd(struct i3c_master_controller *controller,
				     const struct i3c_ccc_cmd *cmd)
{
	struct i3c_master_controller *parent = get_parent_controller(controller);

	return parent->ops->supports_ccc_cmd(parent, cmd);
}

static int p3h2x4x_send_ccc_cmd(struct i3c_master_controller *controller,
				struct i3c_ccc_cmd *cmd)
{
	struct tp_bus *bus = container_of(controller, struct tp_bus, i3c_port_controller);
	struct i3c_master_controller *parent = get_parent_controller(controller);
	int ret;

	if (cmd->id == I3C_CCC_RSTDAA(true))
		return 0;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	ret = parent->ops->send_ccc_cmd(parent, cmd);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);

	return ret;
}

static int p3h2x4x_priv_xfers(struct i3c_dev_desc *dev,
			      struct i3c_priv_xfer *xfers, int nxfers)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int res;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	res = parent->ops->priv_xfers(dev, xfers, nxfers);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);

	return res;
}

static int p3h2x4x_attach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i2c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->attach_i2c_dev(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	return ret;
}

static void p3h2x4x_detach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i2c_desc(dev);
	struct i3c_master_controller *orig_parent;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	parent->ops->detach_i2c_dev(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
}

static int p3h2x4x_i2c_xfers(struct i2c_dev_desc *dev,
			     const struct i2c_msg *xfers, int nxfers)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i2c_desc(dev);
	struct tp_bus *bus = p3h2x4x_bus_from_i2c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->i2c_xfers(dev, xfers, nxfers);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);
	return ret;
}

static int p3h2x4x_request_ibi(struct i3c_dev_desc *dev,
			       const struct i3c_ibi_setup *req)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->request_ibi(dev, req);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);
	return ret;
}

static void p3h2x4x_free_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	parent->ops->free_ibi(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);
}

static int p3h2x4x_enable_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->enable_ibi(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);
	return ret;
}

static int p3h2x4x_disable_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct tp_bus *bus = p3h2x4x_bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	p3h2x4x_en_p3h2x4x_ntwk_tp(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->disable_ibi(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	p3h2x4x_dis_p3h2x4x_ntwk_tp(bus);
	return ret;
}

static void p3h2x4x_recycle_ibi_slot(struct i3c_dev_desc *dev,
				     struct i3c_ibi_slot *slot)
{
	struct i3c_master_controller *parent = get_parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	parent->ops->recycle_ibi_slot(dev, slot);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
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
	.priv_xfers = p3h2x4x_priv_xfers,
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
 * @priv: p3h2x4x device structure.
 * @tp: target port.
 *
 * Return: 0 in case of success, a negative EINVAL code if the error.
 */
int p3h2x4x_tp_i3c_algo(struct p3h2x4x *priv, int tp)
{
	struct device *dev = i3cdev_to_dev(priv->i3cdev);
	int ret;

	priv->tp_bus[tp].tp_mask =  BIT(tp);
	dev->of_node = priv->tp_bus[tp].of_node;

	ret = i3c_master_register(&priv->tp_bus[tp].i3c_port_controller,
				  dev, &p3h2x4x_i3c_ops, false);
	if (ret) {
		dev_warn(dev, "Failed to register i3c controller for tp %d\n", tp);
		return -EINVAL;
	}

	priv->tp_bus[tp].is_registered = true;
	return 0;
}
