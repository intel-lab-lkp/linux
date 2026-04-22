// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 NXP
 *
 * The driver exports a standard gpiochip interface to control
 * the GPIO controllers via RPMSG on a remote processor.
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/irqdomain.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/rpmsg.h>
#include <linux/virtio_gpio.h>

#define MAX_PORT_PER_CHANNEL    10
#define GPIOS_PER_PORT_DEFAULT	32
#define RPMSG_TIMEOUT		1000

/* GPIO RPMSG Type */
#define GPIO_RPMSG_SEND		0
#define GPIO_RPMSG_REPLY	1
#define GPIO_RPMSG_NOTIFY	2

struct rpmsg_gpio_packet {
	u8 type;	/* Message type */
	u8 cmd;		/* Command code */
	u8 port_idx;
	u8 line;
	u8 val1;
	u8 val2;
};

struct rpmsg_gpio_line {
	u8 irq_shutdown;
	u8 irq_unmask;
	u8 irq_mask;
	u32 irq_wake_enable;
	u32 irq_type;
	struct rpmsg_gpio_packet msg;
};

struct rpmsg_gpio_info {
	struct rpmsg_device *rpdev;
	struct rpmsg_gpio_packet *reply_msg;
	struct completion cmd_complete;
	struct mutex lock;
	void **port_store;
};

struct rpmsg_gpio_port {
	struct gpio_chip gc;
	struct rpmsg_gpio_line lines[GPIOS_PER_PORT_DEFAULT];
	struct rpmsg_gpio_info info;
	u32 ngpios;
	u32 idx;
};

/**
 * struct rpmsg_drvdata - driver data per channel.
 * @rproc_name: the name of the remote proc.
 * @recv_pkt: a pointer to the received packet for protocol fix up.
 * @channel_devices: an array of the devices related to the rpdev.
 */
struct rpdev_drvdata {
	const char *rproc_name;
	void *recv_pkt;
	void *channel_devices[MAX_PORT_PER_CHANNEL];
};

static int rpmsg_gpio_send_message(struct rpmsg_gpio_port *port,
				   struct rpmsg_gpio_packet *msg)
{
	struct rpmsg_gpio_info *info = &port->info;
	int ret;

	reinit_completion(&info->cmd_complete);

	ret = rpmsg_send(info->rpdev->ept, msg, sizeof(*msg));
	if (ret) {
		dev_err(&info->rpdev->dev, "rpmsg_send failed: %d\n", ret);
		return ret;
	}

	ret = wait_for_completion_timeout(&info->cmd_complete,
					  msecs_to_jiffies(RPMSG_TIMEOUT));
	if (ret == 0) {
		dev_err(&info->rpdev->dev, "rpmsg_send timeout!\n");
		return -ETIMEDOUT;
	}

	if (info->reply_msg->val1) {
		dev_err(&info->rpdev->dev, "remote core replies an error: %d!\n",
			info->reply_msg->val1);
		return -EINVAL;
	}

	/* copy the reply message */
	memcpy(&port->lines[info->reply_msg->line].msg,
	       info->reply_msg, sizeof(*info->reply_msg));

	return 0;
}

static struct rpmsg_gpio_packet *
rpmsg_gpio_msg_init_common(struct rpmsg_gpio_port *port, unsigned int line, u8 cmd)
{
	struct rpmsg_gpio_packet *msg = &port->lines[line].msg;

	memset(msg, 0, sizeof(*msg));
	msg->type = GPIO_RPMSG_SEND;
	msg->cmd = cmd;
	msg->port_idx = port->idx;
	msg->line = line;

	return msg;
}

static int rpmsg_gpio_get(struct gpio_chip *gc, unsigned int line)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct rpmsg_gpio_packet *msg;
	int ret;

	guard(mutex)(&port->info.lock);

	msg = rpmsg_gpio_msg_init_common(port, line, VIRTIO_GPIO_MSG_GET_VALUE);

	ret = rpmsg_gpio_send_message(port, msg);
	if (!ret)
		ret = !!port->lines[line].msg.val2;

	return ret;
}

static int rpmsg_gpio_get_direction(struct gpio_chip *gc, unsigned int line)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct rpmsg_gpio_packet *msg;
	int ret;

	guard(mutex)(&port->info.lock);

	msg = rpmsg_gpio_msg_init_common(port, line, VIRTIO_GPIO_MSG_GET_DIRECTION);

	ret = rpmsg_gpio_send_message(port, msg);
	if (ret)
		return ret;

	switch (port->lines[line].msg.val2) {
	case VIRTIO_GPIO_DIRECTION_IN:
		return GPIO_LINE_DIRECTION_IN;
	case VIRTIO_GPIO_DIRECTION_OUT:
		return GPIO_LINE_DIRECTION_OUT;
	default:
		break;
	}

	return -EINVAL;
}

static int rpmsg_gpio_direction_input(struct gpio_chip *gc, unsigned int line)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct rpmsg_gpio_packet *msg;

	guard(mutex)(&port->info.lock);

	msg = rpmsg_gpio_msg_init_common(port, line, VIRTIO_GPIO_MSG_SET_DIRECTION);
	msg->val1 = VIRTIO_GPIO_DIRECTION_IN;

	return rpmsg_gpio_send_message(port, msg);
}

static int rpmsg_gpio_set(struct gpio_chip *gc, unsigned int line, int val)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct rpmsg_gpio_packet *msg;

	guard(mutex)(&port->info.lock);

	msg = rpmsg_gpio_msg_init_common(port, line, VIRTIO_GPIO_MSG_SET_VALUE);
	msg->val1 = val;

	return rpmsg_gpio_send_message(port, msg);
}

static int rpmsg_gpio_direction_output(struct gpio_chip *gc, unsigned int line, int val)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct rpmsg_gpio_packet *msg;
	int ret;

	guard(mutex)(&port->info.lock);

	msg = rpmsg_gpio_msg_init_common(port, line, VIRTIO_GPIO_MSG_SET_DIRECTION);
	msg->val1 = VIRTIO_GPIO_DIRECTION_OUT;

	ret = rpmsg_gpio_send_message(port, msg);
	if (ret)
		return ret;

	msg = rpmsg_gpio_msg_init_common(port, line, VIRTIO_GPIO_MSG_SET_VALUE);
	msg->val1 = val;

	return rpmsg_gpio_send_message(port, msg);
}

static int gpio_rpmsg_irq_set_type(struct irq_data *d, u32 type)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 line = d->hwirq;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		type = VIRTIO_GPIO_IRQ_TYPE_EDGE_RISING;
		irq_set_handler_locked(d, handle_simple_irq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		type = VIRTIO_GPIO_IRQ_TYPE_EDGE_FALLING;
		irq_set_handler_locked(d, handle_simple_irq);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		type = VIRTIO_GPIO_IRQ_TYPE_EDGE_BOTH;
		irq_set_handler_locked(d, handle_simple_irq);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		type = VIRTIO_GPIO_IRQ_TYPE_LEVEL_LOW;
		irq_set_handler_locked(d, handle_level_irq);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		type = VIRTIO_GPIO_IRQ_TYPE_LEVEL_HIGH;
		irq_set_handler_locked(d, handle_level_irq);
		break;
	default:
		dev_err(&port->info.rpdev->dev, "unsupported irq type: %u\n", type);
		return -EINVAL;
	}

	port->lines[line].irq_type = type;

	return 0;
}

static int gpio_rpmsg_irq_set_wake(struct irq_data *d, u32 enable)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 line = d->hwirq;

	port->lines[line].irq_wake_enable = enable;

	return 0;
}

/*
 * This unmask/mask function is invoked in two situations:
 *   - when an interrupt is being set up, and
 *   - after an interrupt has occurred.
 *
 * The GPIO driver does not access hardware registers directly.
 * Instead, it caches all relevant information locally, and then sends
 * the accumulated state to the remote system at this stage.
 */
static void gpio_rpmsg_unmask_irq(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 line = d->hwirq;

	port->lines[line].irq_unmask = 1;
}

static void gpio_rpmsg_mask_irq(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 line = d->hwirq;

	/*
	 * When an interrupt occurs, the remote system masks the interrupt
	 * and then sends a notification to Linux. After Linux processes
	 * that notification, it sends an RPMsg command back to the remote
	 * system to unmask the interrupt again.
	 */
	port->lines[line].irq_mask = 1;
}

static void gpio_rpmsg_irq_shutdown(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 line = d->hwirq;

	port->lines[line].irq_shutdown = 1;
}

static void gpio_rpmsg_irq_bus_lock(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);

	mutex_lock(&port->info.lock);
}

static void gpio_rpmsg_irq_bus_sync_unlock(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	struct rpmsg_gpio_packet *msg;
	u32 line = d->hwirq;

	/*
	 * For mask irq, do nothing here.
	 * The remote system will mask interrupt after an interrupt occurs,
	 * and then send a notification to Linux system. After Linux system
	 * handles the notification, it sends an rpmsg back to the remote
	 * system to unmask this interrupt again.
	 */
	if (port->lines[line].irq_mask && !port->lines[line].irq_unmask) {
		port->lines[line].irq_mask = 0;
		mutex_unlock(&port->info.lock);
		return;
	}

	msg = rpmsg_gpio_msg_init_common(port, line, VIRTIO_GPIO_MSG_IRQ_TYPE);

	if (port->lines[line].irq_shutdown) {
		port->lines[line].irq_shutdown = 0;
		msg->val1 = VIRTIO_GPIO_IRQ_TYPE_NONE;
		msg->val2 = 0;
	} else {
		msg->val1 = port->lines[line].irq_type;

		if (port->lines[line].irq_unmask) {
			msg->val2 = 0;
			port->lines[line].irq_unmask = 0;
		} else /* irq set wake */
			msg->val2 = port->lines[line].irq_wake_enable;
	}

	rpmsg_gpio_send_message(port, msg);
	mutex_unlock(&port->info.lock);
}

static const struct irq_chip gpio_rpmsg_irq_chip = {
	.irq_mask = gpio_rpmsg_mask_irq,
	.irq_unmask = gpio_rpmsg_unmask_irq,
	.irq_set_wake = gpio_rpmsg_irq_set_wake,
	.irq_set_type = gpio_rpmsg_irq_set_type,
	.irq_shutdown = gpio_rpmsg_irq_shutdown,
	.irq_bus_lock = gpio_rpmsg_irq_bus_lock,
	.irq_bus_sync_unlock = gpio_rpmsg_irq_bus_sync_unlock,
	.flags = IRQCHIP_IMMUTABLE,
};

static void rpmsg_gpio_remove_action(void *data)
{
	struct rpmsg_gpio_port *port = data;

	port->info.port_store[port->idx] = NULL;
}

static int rpmsg_gpiochip_register(struct rpmsg_device *rpdev, struct device_node *np)
{
	struct rpdev_drvdata *drvdata = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_gpio_port *port;
	struct gpio_irq_chip *girq;
	struct gpio_chip *gc;
	int ret;

	port = devm_kzalloc(&rpdev->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	ret = of_property_read_u32(np, "reg", &port->idx);
	if (ret)
		return ret;

	if (port->idx >= MAX_PORT_PER_CHANNEL)
		return -EINVAL;

	ret = devm_mutex_init(&rpdev->dev, &port->info.lock);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ngpios", &port->ngpios);
	if (ret || port->ngpios > GPIOS_PER_PORT_DEFAULT)
		port->ngpios = GPIOS_PER_PORT_DEFAULT;

	port->info.reply_msg = devm_kzalloc(&rpdev->dev,
					    sizeof(*port->info.reply_msg),
					    GFP_KERNEL);
	if (!port->info.reply_msg)
		return -ENOMEM;

	init_completion(&port->info.cmd_complete);
	port->info.port_store = drvdata->channel_devices;
	port->info.port_store[port->idx] = port;
	port->info.rpdev = rpdev;

	gc = &port->gc;
	gc->owner = THIS_MODULE;
	gc->parent = &rpdev->dev;
	gc->fwnode = of_fwnode_handle(np);
	gc->ngpio = port->ngpios;
	gc->base = -1;
	gc->label = devm_kasprintf(&rpdev->dev, GFP_KERNEL, "%s-gpio%d",
				   drvdata->rproc_name, port->idx);

	gc->direction_input = rpmsg_gpio_direction_input;
	gc->direction_output = rpmsg_gpio_direction_output;
	gc->get_direction = rpmsg_gpio_get_direction;
	gc->get = rpmsg_gpio_get;
	gc->set = rpmsg_gpio_set;

	girq = &gc->irq;
	gpio_irq_chip_set_chip(girq, &gpio_rpmsg_irq_chip);
	girq->parent_handler = NULL;
	girq->num_parents = 0;
	girq->parents = NULL;
	girq->chip->name = devm_kasprintf(&rpdev->dev, GFP_KERNEL, "%s-gpio%d",
					  drvdata->rproc_name, port->idx);

	ret = devm_add_action_or_reset(&rpdev->dev, rpmsg_gpio_remove_action, port);
	if (ret)
		return ret;

	return devm_gpiochip_add_data(&rpdev->dev, gc, port);
}

static const char *rpmsg_get_rproc_node_name(struct rpmsg_device *rpdev)
{
	const char *name = NULL;
	struct device_node *np;
	struct rproc *rproc;

	rproc = rproc_get_by_child(&rpdev->dev);
	if (!rproc)
		return NULL;

	np = of_node_get(rproc->dev.of_node);
	if (!np && rproc->dev.parent)
		np = of_node_get(rproc->dev.parent->of_node);

	if (np) {
		name = devm_kstrdup(&rpdev->dev, np->name, GFP_KERNEL);
		of_node_put(np);
	}

	return name;
}

static struct device_node *
rpmsg_get_channel_ofnode(struct rpmsg_device *rpdev, char *chan_name)
{
	struct device_node *np_chan = NULL, *np;
	struct rproc *rproc;

	rproc = rproc_get_by_child(&rpdev->dev);
	if (!rproc)
		return NULL;

	np = of_node_get(rproc->dev.of_node);
	if (!np && rproc->dev.parent)
		np = of_node_get(rproc->dev.parent->of_node);

	/* The of_node_put() is performed by of_find_node_by_name(). */
	if (np)
		np_chan = of_find_node_by_name(np, chan_name);

	return np_chan;
}

static int rpmsg_gpio_channel_callback(struct rpmsg_device *rpdev, void *data,
				       int len, void *priv, u32 src)
{
	struct rpmsg_gpio_packet *msg = data;
	struct rpmsg_gpio_port *port = NULL;
	struct rpdev_drvdata *drvdata;

	drvdata = dev_get_drvdata(&rpdev->dev);
	if (!msg || !drvdata)
		return -EINVAL;

	if (msg->port_idx < MAX_PORT_PER_CHANNEL)
		port = drvdata->channel_devices[msg->port_idx];

	if (!port || msg->line >= port->ngpios) {
		dev_err(&rpdev->dev, "wrong port index or line number. port:%d line:%d\n",
			msg->port_idx, msg->line);
		return -EINVAL;
	}

	if (msg->type == GPIO_RPMSG_REPLY) {
		*port->info.reply_msg = *msg;
		complete(&port->info.cmd_complete);
	} else if (msg->type == GPIO_RPMSG_NOTIFY) {
		generic_handle_domain_irq_safe(port->gc.irq.domain, msg->line);
	} else {
		dev_err(&rpdev->dev, "wrong command type (0x%x)\n", msg->type);
	}

	return 0;
}

static int rpmsg_gpio_channel_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct rpdev_drvdata *drvdata;
	struct device_node *np;
	int ret = -ENODEV;

	if (!dev->of_node) {
		np = rpmsg_get_channel_ofnode(rpdev, rpdev->id.name);
		if (np) {
			dev->of_node = np;
			set_primary_fwnode(dev, of_fwnode_handle(np));
		}
		return -EPROBE_DEFER;
	}

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->rproc_name = rpmsg_get_rproc_node_name(rpdev);
	dev_set_drvdata(dev, drvdata);

	for_each_child_of_node_scoped(dev->of_node, child) {
		if (!of_device_is_available(child))
			continue;

		if (!of_match_node(dev->driver->of_match_table, child))
			continue;

		ret = rpmsg_gpiochip_register(rpdev, child);
		if (ret < 0)
			break;
	}

	return ret;
}

static const struct of_device_id rpmsg_gpio_dt_ids[] = {
	{ .compatible = "rpmsg-gpio" },
	{ /* sentinel */ }
};

static struct rpmsg_device_id rpmsg_gpio_channel_id_table[] = {
	{ .name = "rpmsg-io" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_gpio_channel_id_table);

static struct rpmsg_driver rpmsg_gpio_channel_client = {
	.callback	= rpmsg_gpio_channel_callback,
	.id_table	= rpmsg_gpio_channel_id_table,
	.probe		= rpmsg_gpio_channel_probe,
	.drv		= {
		.name	= KBUILD_MODNAME,
		.of_match_table = rpmsg_gpio_dt_ids,
	},
};
module_rpmsg_driver(rpmsg_gpio_channel_client);

MODULE_AUTHOR("Shenwei Wang <shenwei.wang@nxp.com>");
MODULE_DESCRIPTION("generic rpmsg gpio driver");
MODULE_LICENSE("GPL");
