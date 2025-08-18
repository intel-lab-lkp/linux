// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 NXP
 *
 * The driver exports a standard gpiochip interface to control
 * the GPIO controllers via RPMSG on a remote processor.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/imx_rpmsg.h>
#include <linux/init.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/rpmsg.h>
#include <linux/virtio.h>
#include <linux/workqueue.h>

#define IMX_RPMSG_GPIO_PER_PORT	32
#define RPMSG_TIMEOUT	1000

enum gpio_input_trigger_type {
	GPIO_RPMSG_TRI_IGNORE,
	GPIO_RPMSG_TRI_RISING,
	GPIO_RPMSG_TRI_FALLING,
	GPIO_RPMSG_TRI_BOTH_EDGE,
	GPIO_RPMSG_TRI_LOW_LEVEL,
	GPIO_RPMSG_TRI_HIGH_LEVEL,
};

enum gpio_rpmsg_header_type {
	GPIO_RPMSG_SETUP,
	GPIO_RPMSG_REPLY,
	GPIO_RPMSG_NOTIFY,
};

enum gpio_rpmsg_header_cmd {
	GPIO_RPMSG_INPUT_INIT,
	GPIO_RPMSG_OUTPUT_INIT,
	GPIO_RPMSG_INPUT_GET,
};

struct gpio_rpmsg_data {
	struct imx_rpmsg_head header;
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

struct imx_rpmsg_gpio_pin {
	u8 irq_shutdown;
	u8 irq_unmask;
	u8 irq_mask;
	u32 irq_wake_enable;
	u32 irq_type;
	struct gpio_rpmsg_data msg;
};

struct imx_gpio_rpmsg_info {
	struct rpmsg_device *rpdev;
	struct gpio_rpmsg_data *notify_msg;
	struct gpio_rpmsg_data *reply_msg;
	struct pm_qos_request pm_qos_req;
	struct completion cmd_complete;
	struct mutex lock;
	struct imx_rpmsg_gpio_port **port_store;
};

struct imx_rpmsg_gpio_port {
	struct gpio_chip gc;
	struct irq_chip chip;
	struct device *dev;
	struct irq_domain *domain;
	struct imx_rpmsg_gpio_pin gpio_pins[IMX_RPMSG_GPIO_PER_PORT];
	struct imx_gpio_rpmsg_info info;
	int idx;
};

static int gpio_send_message(struct imx_rpmsg_gpio_port *port,
			     struct gpio_rpmsg_data *msg,
			     bool sync)
{
	struct imx_gpio_rpmsg_info *info = &port->info;
	int err;

	if (!info->rpdev) {
		dev_dbg(&info->rpdev->dev,
			"rpmsg channel not ready, m4 image ready?\n");
		return -EINVAL;
	}

	cpu_latency_qos_add_request(&info->pm_qos_req, 0);
	reinit_completion(&info->cmd_complete);
	err = rpmsg_send(info->rpdev->ept, (void *)msg,
			 sizeof(struct gpio_rpmsg_data));
	if (err) {
		dev_err(&info->rpdev->dev, "rpmsg_send failed: %d\n", err);
		goto err_out;
	}

	if (sync) {
		err = wait_for_completion_timeout(&info->cmd_complete,
						  msecs_to_jiffies(RPMSG_TIMEOUT));
		if (!err) {
			dev_err(&info->rpdev->dev, "rpmsg_send timeout!\n");
			err = -ETIMEDOUT;
			goto err_out;
		}

		if (info->reply_msg->out.retcode != 0) {
			dev_err(&info->rpdev->dev, "rpmsg not ack %d!\n",
				info->reply_msg->out.retcode);
			err = -EINVAL;
			goto err_out;
		}

		/* copy the reply message */
		memcpy(&port->gpio_pins[info->reply_msg->pin_idx].msg,
		       info->reply_msg, sizeof(*info->reply_msg));

		err = 0;
	}

err_out:
	cpu_latency_qos_remove_request(&info->pm_qos_req);

	return err;
}

static struct gpio_rpmsg_data *gpio_get_pin_msg(struct imx_rpmsg_gpio_port *port,
						unsigned int offset)
{
	struct gpio_rpmsg_data *msg = &port->gpio_pins[offset].msg;

	memset(msg, 0, sizeof(struct gpio_rpmsg_data));

	return msg;
};

static int imx_rpmsg_gpio_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct imx_rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct gpio_rpmsg_data *msg = NULL;
	int ret;

	mutex_lock(&port->info.lock);

	msg = gpio_get_pin_msg(port, gpio);
	msg->header.cate = IMX_RPMSG_GPIO;
	msg->header.major = IMX_RMPSG_MAJOR;
	msg->header.minor = IMX_RMPSG_MINOR;
	msg->header.type = GPIO_RPMSG_SETUP;
	msg->header.cmd = GPIO_RPMSG_INPUT_GET;
	msg->pin_idx = gpio;
	msg->port_idx = port->idx;

	ret = gpio_send_message(port, msg, true);
	if (!ret)
		ret = !!port->gpio_pins[gpio].msg.in.value;

	mutex_unlock(&port->info.lock);

	return ret;
}

static int imx_rpmsg_gpio_direction_input(struct gpio_chip *gc,
					  unsigned int gpio)
{
	struct imx_rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct gpio_rpmsg_data *msg = NULL;
	int ret;

	mutex_lock(&port->info.lock);

	msg = gpio_get_pin_msg(port, gpio);
	msg->header.cate = IMX_RPMSG_GPIO;
	msg->header.major = IMX_RMPSG_MAJOR;
	msg->header.minor = IMX_RMPSG_MINOR;
	msg->header.type = GPIO_RPMSG_SETUP;
	msg->header.cmd = GPIO_RPMSG_INPUT_INIT;
	msg->pin_idx = gpio;
	msg->port_idx = port->idx;

	msg->out.event = GPIO_RPMSG_TRI_IGNORE;
	msg->in.wakeup = 0;

	ret = gpio_send_message(port, msg, true);

	mutex_unlock(&port->info.lock);

	return ret;
}

static inline void imx_rpmsg_gpio_direction_output_init(struct gpio_chip *gc,
		unsigned int gpio, int val, struct gpio_rpmsg_data *msg)
{
	struct imx_rpmsg_gpio_port *port = gpiochip_get_data(gc);

	msg->header.cate = IMX_RPMSG_GPIO;
	msg->header.major = IMX_RMPSG_MAJOR;
	msg->header.minor = IMX_RMPSG_MINOR;
	msg->header.type = GPIO_RPMSG_SETUP;
	msg->header.cmd = GPIO_RPMSG_OUTPUT_INIT;
	msg->pin_idx = gpio;
	msg->port_idx = port->idx;
	msg->out.value = val;
}

static int imx_rpmsg_gpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct imx_rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct gpio_rpmsg_data *msg = NULL;
	int ret;

	mutex_lock(&port->info.lock);

	msg = gpio_get_pin_msg(port, gpio);
	imx_rpmsg_gpio_direction_output_init(gc, gpio, val, msg);
	ret = gpio_send_message(port, msg, true);

	mutex_unlock(&port->info.lock);

	return ret;
}

static int imx_rpmsg_gpio_direction_output(struct gpio_chip *gc,
					unsigned int gpio, int val)
{
	struct imx_rpmsg_gpio_port *port = gpiochip_get_data(gc);
	struct gpio_rpmsg_data *msg = NULL;
	int ret;

	mutex_lock(&port->info.lock);

	msg = gpio_get_pin_msg(port, gpio);
	imx_rpmsg_gpio_direction_output_init(gc, gpio, val, msg);
	ret = gpio_send_message(port, msg, true);

	mutex_unlock(&port->info.lock);

	return ret;
}

static int imx_rpmsg_irq_set_type(struct irq_data *d, u32 type)
{
	struct imx_rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;
	int edge = 0;
	int ret = 0;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		edge = GPIO_RPMSG_TRI_RISING;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		edge = GPIO_RPMSG_TRI_FALLING;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		edge = GPIO_RPMSG_TRI_BOTH_EDGE;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		edge = GPIO_RPMSG_TRI_LOW_LEVEL;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		edge = GPIO_RPMSG_TRI_HIGH_LEVEL;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	port->gpio_pins[gpio_idx].irq_type = edge;

	return ret;
}

static int imx_rpmsg_irq_set_wake(struct irq_data *d, u32 enable)
{
	struct imx_rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;

	port->gpio_pins[gpio_idx].irq_wake_enable = enable;

	return 0;
}

/*
 * This function will be called at:
 *  - one interrupt setup.
 *  - the end of one interrupt happened
 * The gpio over rpmsg driver will not write the real register, so save
 * all infos before this function and then send all infos to M core in this
 * step.
 */
static void imx_rpmsg_unmask_irq(struct irq_data *d)
{
	struct imx_rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;

	port->gpio_pins[gpio_idx].irq_unmask = 1;
}

static void imx_rpmsg_mask_irq(struct irq_data *d)
{
	struct imx_rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;
	/*
	 * No need to implement the callback at A core side.
	 * M core will mask interrupt after a interrupt occurred, and then
	 * sends a notify to A core.
	 * After A core dealt with the notify, A core will send a rpmsg to
	 * M core to unmask this interrupt again.
	 */
	port->gpio_pins[gpio_idx].irq_mask = 1;
}

static void imx_rpmsg_irq_shutdown(struct irq_data *d)
{
	struct imx_rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	u32 gpio_idx = d->hwirq;

	port->gpio_pins[gpio_idx].irq_shutdown = 1;
}

static void imx_rpmsg_irq_bus_lock(struct irq_data *d)
{
	struct imx_rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);

	mutex_lock(&port->info.lock);
}

static void imx_rpmsg_irq_bus_sync_unlock(struct irq_data *d)
{
	struct imx_rpmsg_gpio_port *port = irq_data_get_irq_chip_data(d);
	struct gpio_rpmsg_data *msg = NULL;
	u32 gpio_idx = d->hwirq;

	if (port == NULL) {
		mutex_unlock(&port->info.lock);
		return;
	}

	/*
	 * For mask irq, do nothing here.
	 * M core will mask interrupt after a interrupt occurred, and then
	 * sends a notify to A core.
	 * After A core dealt with the notify, A core will send a rpmsg to
	 * M core to unmask this interrupt again.
	 */

	if (port->gpio_pins[gpio_idx].irq_mask && !port->gpio_pins[gpio_idx].irq_unmask) {
		port->gpio_pins[gpio_idx].irq_mask = 0;
		mutex_unlock(&port->info.lock);
		return;
	}

	msg = gpio_get_pin_msg(port, gpio_idx);
	msg->header.cate = IMX_RPMSG_GPIO;
	msg->header.major = IMX_RMPSG_MAJOR;
	msg->header.minor = IMX_RMPSG_MINOR;
	msg->header.type = GPIO_RPMSG_SETUP;
	msg->header.cmd = GPIO_RPMSG_INPUT_INIT;
	msg->pin_idx = gpio_idx;
	msg->port_idx = port->idx;

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

static struct irq_chip imx_rpmsg_irq_chip = {
	.irq_mask = imx_rpmsg_mask_irq,
	.irq_unmask = imx_rpmsg_unmask_irq,
	.irq_set_wake = imx_rpmsg_irq_set_wake,
	.irq_set_type = imx_rpmsg_irq_set_type,
	.irq_shutdown = imx_rpmsg_irq_shutdown,
	.irq_bus_lock = imx_rpmsg_irq_bus_lock,
	.irq_bus_sync_unlock = imx_rpmsg_irq_bus_sync_unlock,
};

static int imx_rpmsg_gpio_callback(struct rpmsg_device *rpdev,
	void *data, int len, void *priv, u32 src)
{
	struct gpio_rpmsg_data *msg = (struct gpio_rpmsg_data *)data;
	unsigned long flags;
	struct imx_rpmsg_gpio_port *port;
	struct imx_rpmsg_driver_data *drvdata;

	drvdata = dev_get_drvdata(&rpdev->dev);
	if (msg)
		port = drvdata->channel_devices[msg->port_idx];
	if (!port)
		return -ENODEV;

	if (msg->header.type == GPIO_RPMSG_REPLY) {
		port->info.reply_msg = msg;
		complete(&port->info.cmd_complete);
	} else if (msg->header.type == GPIO_RPMSG_NOTIFY) {
		port->info.notify_msg = msg;
		local_irq_save(flags);
		generic_handle_domain_irq(port->domain, msg->pin_idx);
		local_irq_restore(flags);
	} else
		dev_err(&rpdev->dev, "wrong command type!\n");

	return 0;
}

static int imx_rpmsg_gpio_to_irq(struct gpio_chip *gc, unsigned int gpio)
{
	struct imx_rpmsg_gpio_port *port = gpiochip_get_data(gc);
	int irq;

	irq = irq_find_mapping(port->domain, gpio);
	if (irq > 0) {
		irq_set_chip_data(irq, port);
		irq_set_chip_and_handler(irq, &port->chip, handle_level_irq);
	}

	return irq;
}

static void imx_rpmsg_gpio_remove_action(void *data)
{
	struct imx_rpmsg_gpio_port *port = data;

	irq_domain_remove(port->domain);
	port->info.port_store[port->idx] = 0;
}

static int imx_rpmsg_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imx_rpmsg_driver_data *pltdata = dev->platform_data;
	struct device_node *np = dev->of_node;
	struct imx_rpmsg_gpio_port *port;
	struct gpio_chip *gc;
	int irq_base;
	int ret;

	if (!pltdata)
		return -EPROBE_DEFER;

	port = devm_kzalloc(&pdev->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	ret = of_property_read_u32(np, "reg", &port->idx);
	if (ret)
		return ret;

	if (port->idx > MAX_DEV_PER_CHANNEL)
		return -EINVAL;

	mutex_init(&port->info.lock);
	init_completion(&port->info.cmd_complete);
	port->info.rpdev = pltdata->rpdev;
	port->info.port_store = (struct imx_rpmsg_gpio_port **)pltdata->channel_devices;
	port->info.port_store[port->idx] = port;
	if (!pltdata->rx_callback)
		pltdata->rx_callback = imx_rpmsg_gpio_callback;

	gc = &port->gc;
	gc->parent = &pltdata->rpdev->dev;
	gc->label = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s-gpio%d",
				   pltdata->rproc_name, port->idx);
	gc->ngpio = IMX_RPMSG_GPIO_PER_PORT;
	gc->base = -1;

	gc->to_irq = imx_rpmsg_gpio_to_irq;
	gc->direction_input = imx_rpmsg_gpio_direction_input;
	gc->direction_output = imx_rpmsg_gpio_direction_output;
	gc->get = imx_rpmsg_gpio_get;
	gc->set = imx_rpmsg_gpio_set;

	platform_set_drvdata(pdev, port);
	ret = devm_gpiochip_add_data(dev, gc, port);
	if (ret < 0)
		return ret;

	devm_add_action_or_reset(&pdev->dev, imx_rpmsg_gpio_remove_action, port);

	/* create an irq domain */
	port->chip = imx_rpmsg_irq_chip;
	port->chip.name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s-gpio%d",
					 pltdata->rproc_name, port->idx);
	port->dev = &pdev->dev;

	irq_base = devm_irq_alloc_descs(&pdev->dev, -1, 0, IMX_RPMSG_GPIO_PER_PORT,
				   numa_node_id());
	if (irq_base < 0) {
		dev_err(&pdev->dev, "Failed to alloc irq_descs\n");
		return irq_base;
	}

	port->domain = irq_domain_create_legacy(of_node_to_fwnode(np),
						IMX_RPMSG_GPIO_PER_PORT,
						irq_base, 0,
						&irq_domain_simple_ops, port);
	if (!port->domain) {
		dev_err(&pdev->dev, "Failed to allocate IRQ domain\n");
		return -EINVAL;
	}

	return 0;
}

static const struct of_device_id imx_rpmsg_gpio_dt_ids[] = {
	{ .compatible = "fsl,imx-rpmsg-gpio" },
	{ /* sentinel */ }
};

static struct platform_driver imx_rpmsg_gpio_driver = {
	.driver	= {
		.name = "gpio-imx-rpmsg",
		.of_match_table = imx_rpmsg_gpio_dt_ids,
	},
	.probe = imx_rpmsg_gpio_probe,
};

static int __init gpio_imx_rpmsg_init(void)
{
	return platform_driver_register(&imx_rpmsg_gpio_driver);
}

device_initcall(gpio_imx_rpmsg_init);

MODULE_AUTHOR("NXP Semiconductor");
MODULE_DESCRIPTION("NXP i.MX SoC rpmsg gpio driver");
MODULE_LICENSE("GPL");
