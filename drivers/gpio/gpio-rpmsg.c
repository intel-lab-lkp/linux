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

#define RPMSG_GPIO_ID		5
#define RPMSG_VENDOR		1
#define RPMSG_VERSION		0

#define GPIOS_PER_PORT		32
#define RPMSG_TIMEOUT		1000

/* GPIO RPMSG header type */
#define GPIO_RPMSG_SETUP	0
#define GPIO_RPMSG_REPLY	1
#define GPIO_RPMSG_NOTIFY	2

/* GPIO Interrupt trigger type */
#define GPIO_RPMSG_TRI_IGNORE		0
#define GPIO_RPMSG_TRI_RISING		1
#define GPIO_RPMSG_TRI_FALLING		2
#define GPIO_RPMSG_TRI_BOTH_EDGE	3
#define GPIO_RPMSG_TRI_LOW_LEVEL	4
#define GPIO_RPMSG_TRI_HIGH_LEVEL	5

/* GPIO RPMSG commands */
#define GPIO_RPMSG_INPUT_INIT		0
#define GPIO_RPMSG_OUTPUT_INIT		1
#define GPIO_RPMSG_INPUT_GET		2
#define GPIO_RPMSG_DIRECTION_GET	3

#define MAX_PORT_PER_CHANNEL    10

/*
 * @rproc_name: the name of the remote proc.
 * @channel_devices: an array of the devices related to the rpdev.
 */
struct rpdev_drvdata {
	const char *rproc_name;
	void *channel_devices[MAX_PORT_PER_CHANNEL];
};

struct gpio_rpmsg_head {
	u8 id;		/* Message ID Code */
	u8 vendor;	/* Vendor ID number */
	u8 version;	/* Vendor-specific version number */
	u8 type;	/* Message type */
	u8 cmd;		/* Command code */
	u8 reserved[5];
} __packed;

struct gpio_rpmsg_packet {
	struct gpio_rpmsg_head header;
	u8 pin_idx;
	u8 port_idx;
	union {
		u8 event;
		u8 retcode;
		u8 value;
	} out;
	union {
		u8 wakeup;
		u8 value;
	} in;
} __packed __aligned(8);

struct gpio_rpmsg_pin {
	u8 irq_shutdown;
	u8 irq_unmask;
	u8 irq_mask;
	u32 irq_wake_enable;
	u32 irq_type;
	struct gpio_rpmsg_packet msg;
};

struct gpio_rpmsg_info {
	struct rpmsg_device *rpdev;
	struct gpio_rpmsg_packet *reply_msg;
	struct completion cmd_complete;
	struct mutex lock;
	void **port_store;
};

struct rpmsg_gpio_port {
	struct gpio_chip gc;
	struct gpio_rpmsg_pin gpio_pins[GPIOS_PER_PORT];
	struct gpio_rpmsg_info info;
	int idx;
};

static int gpio_send_message(struct rpmsg_gpio_port *port,
			     struct gpio_rpmsg_packet *msg,
			     bool sync)
{
	struct gpio_rpmsg_info *info = &port->info;
	int err;

	reinit_completion(&info->cmd_complete);
	err = rpmsg_send(info->rpdev->ept, msg, sizeof(struct gpio_rpmsg_packet));
	if (err) {
		dev_err(&info->rpdev->dev, "rpmsg_send failed: %d\n", err);
		return err;
	}

	if (sync) {
		err = wait_for_completion_timeout(&info->cmd_complete,
						  msecs_to_jiffies(RPMSG_TIMEOUT));
		if (!err) {
			dev_err(&info->rpdev->dev, "rpmsg_send timeout!\n");
			return -ETIMEDOUT;
		}

		if (info->reply_msg->out.retcode != 0) {
			dev_err(&info->rpdev->dev, "remote core replies an error: %d!\n",
				info->reply_msg->out.retcode);
			return -EINVAL;
		}

		/* copy the reply message */
		memcpy(&port->gpio_pins[info->reply_msg->pin_idx].msg,
		       info->reply_msg, sizeof(*info->reply_msg));
	}

	return 0;
}

static struct gpio_rpmsg_packet *gpio_setup_msg_header(struct rpmsg_gpio_port *port,
						       unsigned int offset,
						       u8 cmd)
{
	struct gpio_rpmsg_packet *msg = &port->gpio_pins[offset].msg;

	memset(msg, 0, sizeof(struct gpio_rpmsg_packet));
	msg->header.id = RPMSG_GPIO_ID;
	msg->header.vendor = RPMSG_VENDOR;
	msg->header.version = RPMSG_VERSION;
	msg->header.type = GPIO_RPMSG_SETUP;
	msg->header.cmd = cmd;
	msg->pin_idx = offset;
	msg->port_idx = port->idx;

	return msg;
}

static int rpmsg_gpio_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct gpio_rpmsg_packet *msg;
	int ret;

	guard(mutex)(&port->info.lock);

	msg = gpio_setup_msg_header(port, gpio, GPIO_RPMSG_INPUT_GET);

	ret = gpio_send_message(port, msg, true);
	if (!ret)
		ret = !!port->gpio_pins[gpio].msg.in.value;

	return ret;
}

static int rpmsg_gpio_get_direction(struct gpio_chip *gc, unsigned int gpio)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct gpio_rpmsg_packet *msg;
	int ret;

	guard(mutex)(&port->info.lock);

	msg = gpio_setup_msg_header(port, gpio, GPIO_RPMSG_DIRECTION_GET);

	ret = gpio_send_message(port, msg, true);
	if (!ret)
		ret = !!port->gpio_pins[gpio].msg.in.value;

	return ret;
}

static int rpmsg_gpio_direction_input(struct gpio_chip *gc, unsigned int gpio)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct gpio_rpmsg_packet *msg;

	guard(mutex)(&port->info.lock);

	msg = gpio_setup_msg_header(port, gpio, GPIO_RPMSG_INPUT_INIT);

	return gpio_send_message(port, msg, true);
}

static int rpmsg_gpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct gpio_rpmsg_packet *msg;

	guard(mutex)(&port->info.lock);

	msg = gpio_setup_msg_header(port, gpio, GPIO_RPMSG_OUTPUT_INIT);
	msg->out.value = val;

	return gpio_send_message(port, msg, true);
}

static int rpmsg_gpio_direction_output(struct gpio_chip *gc,
				       unsigned int gpio,
				       int val)
{
	return rpmsg_gpio_set(gc, gpio, val);
}

static int gpio_rpmsg_irq_set_type(struct irq_data *d, u32 type)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;
	int edge = 0;
	int ret = 0;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		edge = GPIO_RPMSG_TRI_RISING;
		irq_set_handler_locked(d, handle_simple_irq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		edge = GPIO_RPMSG_TRI_FALLING;
		irq_set_handler_locked(d, handle_simple_irq);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		edge = GPIO_RPMSG_TRI_BOTH_EDGE;
		irq_set_handler_locked(d, handle_simple_irq);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		edge = GPIO_RPMSG_TRI_LOW_LEVEL;
		irq_set_handler_locked(d, handle_level_irq);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		edge = GPIO_RPMSG_TRI_HIGH_LEVEL;
		irq_set_handler_locked(d, handle_level_irq);
		break;
	default:
		ret = -EINVAL;
		irq_set_handler_locked(d, handle_bad_irq);
		break;
	}

	port->gpio_pins[gpio_idx].irq_type = edge;

	return ret;
}

static int gpio_rpmsg_irq_set_wake(struct irq_data *d, u32 enable)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;

	port->gpio_pins[gpio_idx].irq_wake_enable = enable;

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
	u32 gpio_idx = d->hwirq;

	port->gpio_pins[gpio_idx].irq_unmask = 1;
}

static void gpio_rpmsg_mask_irq(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;

	/*
	 * When an interrupt occurs, the remote system masks the interrupt
	 * and then sends a notification to Linux. After Linux processes
	 * that notification, it sends an RPMsg command back to the remote
	 * system to unmask the interrupt again.
	 */
	port->gpio_pins[gpio_idx].irq_mask = 1;
}

static void gpio_rpmsg_irq_shutdown(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;

	port->gpio_pins[gpio_idx].irq_shutdown = 1;
}

static void gpio_rpmsg_irq_bus_lock(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);

	mutex_lock(&port->info.lock);
}

static void gpio_rpmsg_irq_bus_sync_unlock(struct irq_data *d)
{
	struct rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	struct gpio_rpmsg_packet *msg = NULL;
	u32 gpio_idx = d->hwirq;

	/*
	 * For mask irq, do nothing here.
	 * The remote system will mask interrupt after an interrupt occurs,
	 * and then send a notify to Linux system.
	 * After Linux system dealt with the notify, it will send an rpmsg to
	 * the remote system to unmask this interrupt again.
	 */
	if (port->gpio_pins[gpio_idx].irq_mask && !port->gpio_pins[gpio_idx].irq_unmask) {
		port->gpio_pins[gpio_idx].irq_mask = 0;
		mutex_unlock(&port->info.lock);
		return;
	}

	msg = gpio_setup_msg_header(port, gpio_idx, GPIO_RPMSG_INPUT_INIT);

	if (port->gpio_pins[gpio_idx].irq_shutdown) {
		msg->out.event = GPIO_RPMSG_TRI_IGNORE;
		msg->in.wakeup = 0;
		port->gpio_pins[gpio_idx].irq_shutdown = 0;
	} else {
		/* if not set irq type, then use low level as trigger type */
		msg->out.event = port->gpio_pins[gpio_idx].irq_type;
		if (!msg->out.event)
			msg->out.event = GPIO_RPMSG_TRI_LOW_LEVEL;
		if (port->gpio_pins[gpio_idx].irq_unmask) {
			msg->in.wakeup = 0;
			port->gpio_pins[gpio_idx].irq_unmask = 0;
		} else /* irq set wake */
			msg->in.wakeup = port->gpio_pins[gpio_idx].irq_wake_enable;
	}

	gpio_send_message(port, msg, false);
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

	init_completion(&port->info.cmd_complete);
	port->info.reply_msg = devm_kzalloc(&rpdev->dev,
					    sizeof(struct gpio_rpmsg_packet),
					    GFP_KERNEL);
	port->info.port_store = drvdata->channel_devices;
	port->info.port_store[port->idx] = port;
	port->info.rpdev = rpdev;

	gc = &port->gc;
	gc->owner = THIS_MODULE;
	gc->parent = &rpdev->dev;
	gc->fwnode = of_fwnode_handle(np);
	gc->ngpio = GPIOS_PER_PORT;
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

	if (np) {
		/* Balance the of_node_put() performed by of_find_node_by_name(). */
		of_node_get(np);
		np_chan = of_find_node_by_name(np, chan_name);
		of_node_put(np);
	}

	return np_chan;
}

static int
rpmsg_gpio_channel_callback(struct rpmsg_device *rpdev, void *data,
			    int len, void *priv, u32 src)
{
	struct gpio_rpmsg_packet *msg = data;
	struct rpmsg_gpio_port *port = NULL;
	struct rpdev_drvdata *drvdata;

	drvdata = dev_get_drvdata(&rpdev->dev);
	if (drvdata && msg && (msg->port_idx < MAX_PORT_PER_CHANNEL))
		port = drvdata->channel_devices[msg->port_idx];

	if (!port)
		return -ENODEV;

	if (msg->header.type == GPIO_RPMSG_REPLY) {
		*port->info.reply_msg = *msg;
		complete(&port->info.cmd_complete);
	} else if (msg->header.type == GPIO_RPMSG_NOTIFY) {
		generic_handle_domain_irq_safe(port->gc.irq.domain, msg->pin_idx);
	} else
		dev_err(&rpdev->dev, "wrong command type!\n");

	return 0;
}

static int rpmsg_gpio_channel_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct rpdev_drvdata *drvdata;
	struct device_node *np;
	int ret;

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
			dev_err(dev, "Failed to register: %pOF\n", child);
	}

	return 0;
}

static void rpmsg_gpio_channel_remove(struct rpmsg_device *rpdev)
{
	dev_info(&rpdev->dev, "rpmsg gpio channel driver is removed\n");
}

static const struct of_device_id rpmsg_gpio_dt_ids[] = {
	{ .compatible = "rpmsg-gpio" },
	{ /* sentinel */ }
};

static struct rpmsg_device_id rpmsg_gpio_channel_id_table[] = {
	{ .name	= "rpmsg-io-channel" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_gpio_channel_id_table);

static struct rpmsg_driver rpmsg_gpio_channel_client = {
	.drv.name	= KBUILD_MODNAME,
	.drv.of_match_table = rpmsg_gpio_dt_ids,
	.id_table	= rpmsg_gpio_channel_id_table,
	.probe		= rpmsg_gpio_channel_probe,
	.callback	= rpmsg_gpio_channel_callback,
	.remove		= rpmsg_gpio_channel_remove,
};
module_rpmsg_driver(rpmsg_gpio_channel_client);

MODULE_AUTHOR("Shenwei Wang <shenwei.wang@nxp.com>");
MODULE_DESCRIPTION("generic rpmsg gpio driver");
MODULE_LICENSE("GPL");
