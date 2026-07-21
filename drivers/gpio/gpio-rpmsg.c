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

#define GPIOS_PER_PORT_MAX		32
#define RPMSG_TIMEOUT			1000

/* GPIO Receive MSG Type */
#define GPIO_RPMSG_REPLY	0
#define GPIO_RPMSG_NOTIFY	1

#define CHAN_NAME_PREFIX	"rpmsg-io-"
#define GPIO_COMPAT_STR		"rpmsg-gpio"

struct rpmsg_gpio_response {
	__u8 type;
	union {
		/* command reply */
		struct {
			__u8 status;
			__u8 value;
		};

		/* interrupt notification */
		struct {
			__le16 line;
		};
	};
};

struct rpmsg_gpio_line {
	u8 irq_shutdown;
	u8 irq_unmask;
	u8 irq_mask;
	u32 irq_type;
};

struct rpmsg_gpio_port {
	struct gpio_chip gc;
	struct rpmsg_device *rpdev;
	struct virtio_gpio_request *send_msg;
	struct rpmsg_gpio_response *recv_msg;
	struct completion cmd_complete;
	struct mutex lock;
	struct work_struct eoi_work;
	DECLARE_BITMAP(pending_eoi, GPIOS_PER_PORT_MAX);
	u32 ngpios;
	u32 idx;
	struct rpmsg_gpio_line lines[GPIOS_PER_PORT_MAX];
};

static int rpmsg_gpio_send_message(struct rpmsg_gpio_port *port)
{
	int ret;

	reinit_completion(&port->cmd_complete);

	ret = rpmsg_send(port->rpdev->ept, port->send_msg, sizeof(*port->send_msg));
	if (ret) {
		dev_err(&port->rpdev->dev, "rpmsg_send failed: cmd=%d ret=%d\n",
			port->send_msg->type, ret);
		return ret;
	}

	ret = wait_for_completion_timeout(&port->cmd_complete,
					  msecs_to_jiffies(RPMSG_TIMEOUT));
	if (ret == 0) {
		dev_err(&port->rpdev->dev, "rpmsg_send timeout! cmd=%d\n",
			port->send_msg->type);
		return -ETIMEDOUT;
	}

	if (port->recv_msg->status != VIRTIO_GPIO_STATUS_OK) {
		dev_err(&port->rpdev->dev, "remote core replies an error: cmd=%d!\n",
			port->send_msg->type);
		return -EINVAL;
	}

	return 0;
}

static struct virtio_gpio_request *
rpmsg_gpio_msg_prepare(struct rpmsg_gpio_port *port, u16 line, u16 cmd, u32 val)
{
	struct virtio_gpio_request *msg = port->send_msg;

	msg->type = cpu_to_le16(cmd);
	msg->gpio = cpu_to_le16(line);
	msg->value = cpu_to_le32(val);

	return msg;
}

static int rpmsg_gpio_get(struct gpio_chip *gc, unsigned int line)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	int ret;

	guard(mutex)(&port->lock);

	rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_GET_VALUE, 0);

	ret = rpmsg_gpio_send_message(port);
	return ret ? ret : port->recv_msg->value;
}

static int rpmsg_gpio_get_direction(struct gpio_chip *gc, unsigned int line)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	int ret;

	guard(mutex)(&port->lock);

	rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_GET_DIRECTION, 0);

	ret = rpmsg_gpio_send_message(port);
	if (ret)
		return ret;

	switch (port->recv_msg->value) {
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

	guard(mutex)(&port->lock);

	rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_SET_DIRECTION,
			       VIRTIO_GPIO_DIRECTION_IN);

	return rpmsg_gpio_send_message(port);
}

static int rpmsg_gpio_set(struct gpio_chip *gc, unsigned int line, int val)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);

	guard(mutex)(&port->lock);

	rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_SET_VALUE, val);

	return rpmsg_gpio_send_message(port);
}

static int rpmsg_gpio_direction_output(struct gpio_chip *gc, unsigned int line, int val)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	int ret;

	guard(mutex)(&port->lock);

	rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_SET_VALUE, val);
	ret = rpmsg_gpio_send_message(port);
	if (ret)
		return ret;

	rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_SET_DIRECTION,
			       VIRTIO_GPIO_DIRECTION_OUT);
	return rpmsg_gpio_send_message(port);
}

static int gpio_rpmsg_irq_set_type(struct irq_data *d, u32 type)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 line = d->hwirq;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		type = VIRTIO_GPIO_IRQ_TYPE_EDGE_RISING;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		type = VIRTIO_GPIO_IRQ_TYPE_EDGE_FALLING;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		type = VIRTIO_GPIO_IRQ_TYPE_EDGE_BOTH;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		type = VIRTIO_GPIO_IRQ_TYPE_LEVEL_LOW;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		type = VIRTIO_GPIO_IRQ_TYPE_LEVEL_HIGH;
		break;
	default:
		dev_err(&port->rpdev->dev, "unsupported irq type: %u\n", type);
		return -EINVAL;
	}

	port->lines[line].irq_type = type;

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

static void gpio_rpmsg_eoi_irq(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);

	set_bit(d->hwirq, port->pending_eoi);
	schedule_work(&port->eoi_work);
}

static void gpio_rpmsg_irq_bus_lock(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);

	mutex_lock(&port->lock);
}

static void gpio_rpmsg_irq_bus_sync_unlock(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
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
		mutex_unlock(&port->lock);
		return;
	}

	if (port->lines[line].irq_shutdown) {
		rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_IRQ_TYPE,
				       VIRTIO_GPIO_IRQ_TYPE_NONE);
		port->lines[line].irq_shutdown = 0;
	} else {
		rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_IRQ_TYPE,
				       port->lines[line].irq_type);

		if (port->lines[line].irq_unmask)
			port->lines[line].irq_unmask = 0;
	}

	rpmsg_gpio_send_message(port);
	mutex_unlock(&port->lock);
}

static const struct irq_chip gpio_rpmsg_irq_chip = {
	.name = "rpmsg-gpio",
	.irq_mask = gpio_rpmsg_mask_irq,
	.irq_unmask = gpio_rpmsg_unmask_irq,
	.irq_eoi = gpio_rpmsg_eoi_irq,
	.irq_set_type = gpio_rpmsg_irq_set_type,
	.irq_shutdown = gpio_rpmsg_irq_shutdown,
	.irq_bus_lock = gpio_rpmsg_irq_bus_lock,
	.irq_bus_sync_unlock = gpio_rpmsg_irq_bus_sync_unlock,
	.flags = IRQCHIP_IMMUTABLE,
};

static void gpio_rpmsg_eoi_work(struct work_struct *work)
{
	struct rpmsg_gpio_port *port =
		container_of(work, struct rpmsg_gpio_port, eoi_work);
	unsigned long pending[BITS_TO_LONGS(GPIOS_PER_PORT_MAX)];
	unsigned int line;

	guard(mutex)(&port->lock);

	bitmap_copy(pending, port->pending_eoi, port->ngpios);
	bitmap_zero(port->pending_eoi, port->ngpios);

	for_each_set_bit(line, pending, port->ngpios) {
		rpmsg_gpio_msg_prepare(port, line, VIRTIO_GPIO_MSG_IRQ_TYPE,
				       port->lines[line].irq_type);

		if (rpmsg_gpio_send_message(port))
			dev_err(&port->rpdev->dev, "EOI error for line %u\n", line);
	}
}

static int rpmsg_gpiochip_register(struct rpmsg_device *rpdev, u32 idx,
				   struct device_node *np, const char *name)
{
	struct rpmsg_gpio_port *port;
	struct gpio_irq_chip *girq;
	struct gpio_chip *gc;
	int ret;

	port = devm_kzalloc(&rpdev->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	ret = devm_mutex_init(&rpdev->dev, &port->lock);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "ngpios", &port->ngpios);
	if (ret || port->ngpios > GPIOS_PER_PORT_MAX)
		port->ngpios = GPIOS_PER_PORT_MAX;

	port->send_msg = devm_kzalloc(&rpdev->dev,
				      sizeof(*port->send_msg),
				      GFP_KERNEL);

	port->recv_msg = devm_kzalloc(&rpdev->dev,
				      sizeof(*port->recv_msg),
				      GFP_KERNEL);
	if (!port->send_msg || !port->recv_msg)
		return -ENOMEM;

	INIT_WORK(&port->eoi_work, gpio_rpmsg_eoi_work);
	init_completion(&port->cmd_complete);
	port->rpdev = rpdev;
	port->idx = idx;

	gc = &port->gc;
	gc->owner = THIS_MODULE;
	gc->parent = &rpdev->dev;
	gc->fwnode = of_fwnode_handle(np);
	gc->ngpio = port->ngpios;
	gc->base = -1;
	gc->label = devm_kasprintf(&rpdev->dev, GFP_KERNEL, "%s-gpio%d",
				   name, port->idx);

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
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_fasteoi_irq;

	dev_set_drvdata(&rpdev->dev, port);

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
rpmsg_find_child_by_compat_reg(struct device_node *parent, const char *compat, u32 idx)
{
	struct device_node *child;
	u32 reg;

	for_each_available_child_of_node(parent, child) {
		if (!of_device_is_compatible(child, compat))
			continue;

		if (of_property_read_u32(child, "reg", &reg))
			continue;

		if (reg == idx)
			return child;
	}

	return NULL;
}

static struct device_node *
rpmsg_get_gpio_ofnode(struct rpmsg_device *rpdev, const char *compat, u32 idx)
{
	struct device_node *np_chan, *np_rproc, *np_rpmsg, *np_io;
	struct rproc *rproc;

	rproc = rproc_get_by_child(&rpdev->dev);
	if (!rproc)
		return NULL;

	np_rproc = of_node_get(rproc->dev.of_node);
	if (!np_rproc && rproc->dev.parent)
		np_rproc = of_node_get(rproc->dev.parent->of_node);

	if (!np_rproc)
		return NULL;

	np_rpmsg = of_get_child_by_name(np_rproc, "rpmsg");
	of_node_put(np_rproc);
	if (!np_rpmsg)
		return NULL;

	np_io = of_get_child_by_name(np_rpmsg, "rpmsg-io");
	of_node_put(np_rpmsg);
	if (!np_io)
		return NULL;

	np_chan = rpmsg_find_child_by_compat_reg(np_io, compat, idx);
	of_node_put(np_io);

	return np_chan;
}

static int rpmsg_get_gpio_index(const char *name, const char *prefix)
{
	const char *p;
	int base = 10;
	int val;

	if (!name)
		return -EINVAL;

	/* Ensure correct prefix */
	if (!str_has_prefix(name, prefix))
		return -EINVAL;

	/* Find last '-' */
	p = strrchr(name, '-');

	if (!p || *(p + 1) == '\0')
		return -EINVAL;

	if (p[1] == '0' && (p[2] == 'x' || p[2] == 'X'))
		base = 16;

	if (kstrtoint(p + 1, base, &val))
		return -EINVAL;

	return val;
}

static int rpmsg_gpio_channel_callback(struct rpmsg_device *rpdev, void *data,
				       int len, void *priv, u32 src)
{
	struct rpmsg_gpio_response *msg = data;
	struct rpmsg_gpio_port *port = NULL;
	u32 line;

	port = dev_get_drvdata(&rpdev->dev);

	if (!port) {
		dev_err(&rpdev->dev, "port is null\n");
		return -EINVAL;
	}

	if (msg->type == GPIO_RPMSG_REPLY) {
		*port->recv_msg = *msg;
		complete(&port->cmd_complete);
	} else if (msg->type == GPIO_RPMSG_NOTIFY) {
		line = le16_to_cpu(msg->line);
		generic_handle_domain_irq_safe(port->gc.irq.domain, line);
	} else {
		dev_err(&rpdev->dev, "wrong message type (0x%x)\n", msg->type);
	}

	return 0;
}

static int rpmsg_gpio_channel_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct device_node *np;
	const char *rproc_name;
	int idx;

	idx = rpmsg_get_gpio_index(rpdev->id.name, CHAN_NAME_PREFIX);
	if (idx < 0)
		return -EINVAL;

	if (!dev->of_node) {
		np = rpmsg_get_gpio_ofnode(rpdev, GPIO_COMPAT_STR, idx);
		if (np) {
			dev->of_node = np;
			set_primary_fwnode(dev, of_fwnode_handle(np));
			return -EPROBE_DEFER;
		}
	}

	rproc_name = rpmsg_get_rproc_node_name(rpdev);

	return rpmsg_gpiochip_register(rpdev, idx, dev->of_node, rproc_name);
}

static void rpmsg_gpio_channel_remove(struct rpmsg_device *rpdev)
{
	struct rpmsg_gpio_port *port = dev_get_drvdata(&rpdev->dev);

	cancel_work_sync(&port->eoi_work);
}

static struct rpmsg_device_id rpmsg_gpio_channel_id_table[] = {
	{ .name = CHAN_NAME_PREFIX },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_gpio_channel_id_table);

static struct rpmsg_driver rpmsg_gpio_channel_client = {
	.callback	= rpmsg_gpio_channel_callback,
	.id_table	= rpmsg_gpio_channel_id_table,
	.probe		= rpmsg_gpio_channel_probe,
	.remove		= rpmsg_gpio_channel_remove,
	.drv.name	= KBUILD_MODNAME,
};
module_rpmsg_driver(rpmsg_gpio_channel_client);

MODULE_AUTHOR("Shenwei Wang <shenwei.wang@nxp.com>");
MODULE_DESCRIPTION("generic rpmsg gpio driver");
MODULE_LICENSE("GPL");
