// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 GPIO controller driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>

#define DRVNAME "nct6694-gpio"

/* Host interface */
#define REQUEST_GPIO_MOD		0xFF
#define REQUEST_GPIO_LEN		0x01

/* Report Channel */
#define GPIO_VER_REG			0x90
#define GPIO_VALID_REG			0x110
#define GPI_DATA_REG			0x120
#define GPO_DIR_REG			0x170
#define GPO_TYPE_REG			0x180
#define GPO_DATA_REG			0x190

#define GPI_STS_REG			0x130
#define GPI_CLR_REG			0x140
#define GPI_FALLING_REG			0x150
#define GPI_RISING_REG			0x160

struct nct6694_gpio_data {
	struct nct6694 *nct6694;
	struct gpio_chip gpio;
	struct work_struct irq_work;
	struct work_struct irq_trig_work;

	/* Protect irq operation */
	struct mutex irq_lock;

	unsigned char irq_trig_falling;
	unsigned char irq_trig_rising;

	/* Current gpio group */
	unsigned char group;
};

static int nct6694_get_direction(struct gpio_chip *gpio, unsigned int offset)
{
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);
	unsigned char buf;
	int ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPO_DIR_REG + data->group,
			       REQUEST_GPIO_LEN, 0, 1, &buf);
	if (ret < 0)
		return ret;

	return !(BIT(offset) & buf);
}

static int nct6694_direction_input(struct gpio_chip *gpio, unsigned int offset)
{
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);
	unsigned char buf;
	int ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPO_DIR_REG + data->group,
			       REQUEST_GPIO_LEN, 0, 1, &buf);
	if (ret < 0)
		return ret;

	buf &= ~(1 << offset);

	ret = nct6694_write_msg(data->nct6694, REQUEST_GPIO_MOD,
				GPO_DIR_REG + data->group,
				REQUEST_GPIO_LEN, &buf);

	return ret;
}

static int nct6694_direction_output(struct gpio_chip *gpio,
				    unsigned int offset, int val)
{
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);
	unsigned char buf;
	int ret;

	/* Set direction to output */
	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPO_DIR_REG + data->group,
			       REQUEST_GPIO_LEN, 0, 1, &buf);
	if (ret < 0)
		return ret;

	buf |= (1 << offset);
	ret = nct6694_write_msg(data->nct6694, REQUEST_GPIO_MOD,
				GPO_DIR_REG + data->group,
				REQUEST_GPIO_LEN, &buf);
	if (ret < 0)
		return ret;

	/* Then set output level */
	ret = nct6694_read_msg(data->nct6694, 0xFF,
			       GPO_DATA_REG + data->group,
			       REQUEST_GPIO_LEN, 0, 1, &buf);
	if (ret < 0)
		return ret;

	if (val)
		buf |= (1 << offset);
	else
		buf &= ~(1 << offset);

	ret = nct6694_write_msg(data->nct6694, REQUEST_GPIO_MOD,
				GPO_DATA_REG + data->group,
				REQUEST_GPIO_LEN, &buf);

	return ret;
}

static int nct6694_get_value(struct gpio_chip *gpio, unsigned int offset)
{
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);
	unsigned char buf;
	int ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPO_DIR_REG + data->group,
			       REQUEST_GPIO_LEN, 0, 1, &buf);
	if (ret < 0)
		return ret;

	if (BIT(offset) & buf) {
		ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
				       GPO_DATA_REG + data->group,
				       REQUEST_GPIO_LEN, 0, 1, &buf);
		if (ret < 0)
			return ret;

		return !!(BIT(offset) & buf);
	}

	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPI_DATA_REG + data->group,
			       REQUEST_GPIO_LEN, 0, 1, &buf);
	if (ret < 0)
		return ret;

	return !!(BIT(offset) & buf);
}

static void nct6694_set_value(struct gpio_chip *gpio, unsigned int offset,
			      int val)
{
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);
	unsigned char buf;

	nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			 GPO_DATA_REG + data->group,
			 REQUEST_GPIO_LEN, 0, 1, &buf);

	if (val)
		buf |= (1 << offset);
	else
		buf &= ~(1 << offset);

	nct6694_write_msg(data->nct6694, REQUEST_GPIO_MOD,
			  GPO_DATA_REG + data->group,
			  REQUEST_GPIO_LEN, &buf);
}

static int nct6694_set_config(struct gpio_chip *gpio, unsigned int offset,
			      unsigned long config)
{
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);
	unsigned char buf;
	int ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPO_TYPE_REG + data->group,
			       REQUEST_GPIO_LEN, 0, 1, &buf);
	if (ret < 0)
		return ret;

	switch (pinconf_to_config_param(config)) {
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		buf |= (1 << offset);
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		buf &= ~(1 << offset);
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = nct6694_write_msg(data->nct6694, REQUEST_GPIO_MOD,
				GPO_TYPE_REG + data->group,
				REQUEST_GPIO_LEN, &buf);

	return ret;
}

static int nct6694_init_valid_mask(struct gpio_chip *gpio,
				   unsigned long *valid_mask,
				   unsigned int ngpios)
{
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);
	unsigned char buf;
	int ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPIO_VALID_REG + data->group,
			       REQUEST_GPIO_LEN, 0, 1, &buf);
	if (ret < 0)
		return ret;

	*valid_mask = buf;

	return ret;
}

static void nct6694_irq(struct work_struct *irq_work)
{
	struct nct6694_gpio_data *data;
	unsigned char status;

	data = container_of(irq_work, struct nct6694_gpio_data, irq_work);

	nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			 GPI_STS_REG + data->group, 1,
			 0, 1, &status);

	while (status) {
		int bit = __ffs(status);
		unsigned char val = BIT(bit);

		handle_nested_irq(irq_find_mapping(data->gpio.irq.domain, bit));
		status &= ~(1 << bit);
		nct6694_write_msg(data->nct6694, REQUEST_GPIO_MOD,
				  GPI_CLR_REG + data->group,
				  REQUEST_GPIO_LEN, &val);
	}
}

static void nct6694_gpio_handler(void *private_data)
{
	struct nct6694_gpio_data *data = private_data;
	struct nct6694 *nct6694 = data->nct6694;

	queue_work(nct6694->async_workqueue, &data->irq_work);
}

static int nct6694_get_irq_trig(struct nct6694_gpio_data *data)
{
	int ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPI_FALLING_REG + data->group,
			       1, 0, 1, &data->irq_trig_falling);
	if (ret)
		return ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_GPIO_MOD,
			       GPI_RISING_REG + data->group,
			       1, 0, 1, &data->irq_trig_rising);

	return ret;
}

static void nct6694_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gpio = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	gpiochip_disable_irq(gpio, hwirq);
}

static void nct6694_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gpio = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	gpiochip_enable_irq(gpio, hwirq);
}

static void nct6694_irq_trig(struct work_struct *irq_trig_work)
{
	struct nct6694_gpio_data *data;

	data = container_of(irq_trig_work, struct nct6694_gpio_data,
			    irq_trig_work);

	nct6694_write_msg(data->nct6694, REQUEST_GPIO_MOD,
			  GPI_FALLING_REG + data->group,
			  1, &data->irq_trig_falling);

	nct6694_write_msg(data->nct6694, REQUEST_GPIO_MOD,
			  GPI_RISING_REG + data->group,
			  1, &data->irq_trig_rising);
}

static int nct6694_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gpio = irq_data_get_irq_chip_data(d);
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);
	struct nct6694 *nct6694 = data->nct6694;
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		data->irq_trig_rising |= BIT(hwirq);
		break;

	case IRQ_TYPE_EDGE_FALLING:
		data->irq_trig_falling |= BIT(hwirq);
		break;

	case IRQ_TYPE_EDGE_BOTH:
		data->irq_trig_rising |= BIT(hwirq);
		data->irq_trig_falling |= BIT(hwirq);
		break;

	default:
		return -EOPNOTSUPP;
	}

	queue_work(nct6694->async_workqueue, &data->irq_trig_work);

	return 0;
}

static void nct6694_irq_bus_lock(struct irq_data *d)
{
	struct gpio_chip *gpio = irq_data_get_irq_chip_data(d);
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);

	mutex_lock(&data->irq_lock);
}

static void nct6694_irq_bus_sync_unlock(struct irq_data *d)
{
	struct gpio_chip *gpio = irq_data_get_irq_chip_data(d);
	struct nct6694_gpio_data *data = gpiochip_get_data(gpio);

	mutex_unlock(&data->irq_lock);
}

static const struct irq_chip nct6694_irq_chip = {
	.name			= "nct6694",
	.irq_mask		= nct6694_irq_mask,
	.irq_unmask		= nct6694_irq_unmask,
	.irq_set_type		= nct6694_irq_set_type,
	.irq_bus_lock		= nct6694_irq_bus_lock,
	.irq_bus_sync_unlock	= nct6694_irq_bus_sync_unlock,
	.flags			= IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static const char * const nct6694_gpio_name[] = {
	"NCT6694-GPIO0",
	"NCT6694-GPIO1",
	"NCT6694-GPIO2",
	"NCT6694-GPIO3",
	"NCT6694-GPIO4",
	"NCT6694-GPIO5",
	"NCT6694-GPIO6",
	"NCT6694-GPIO7",
	"NCT6694-GPIO8",
	"NCT6694-GPIO9",
	"NCT6694-GPIOA",
	"NCT6694-GPIOB",
	"NCT6694-GPIOC",
	"NCT6694-GPIOD",
	"NCT6694-GPIOE",
	"NCT6694-GPIOF",
};

static int nct6694_gpio_probe(struct platform_device *pdev)
{
	const struct mfd_cell *cell = mfd_get_cell(pdev);
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	struct nct6694_gpio_data *data;
	struct gpio_irq_chip *girq;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->nct6694 = nct6694;
	data->group = cell->id;

	data->gpio.label		= nct6694_gpio_name[cell->id];
	data->gpio.direction_input	= nct6694_direction_input;
	data->gpio.get			= nct6694_get_value;
	data->gpio.direction_output	= nct6694_direction_output;
	data->gpio.set			= nct6694_set_value;
	data->gpio.get_direction	= nct6694_get_direction;
	data->gpio.set_config		= nct6694_set_config;
	data->gpio.init_valid_mask	= nct6694_init_valid_mask;
	data->gpio.base			= -1;
	data->gpio.can_sleep		= false;
	data->gpio.owner		= THIS_MODULE;
	data->gpio.ngpio		= 8;

	INIT_WORK(&data->irq_work, nct6694_irq);
	INIT_WORK(&data->irq_trig_work, nct6694_irq_trig);
	mutex_init(&data->irq_lock);

	ret = nct6694_register_handler(nct6694, GPIO_IRQ_STATUS,
				       nct6694_gpio_handler, data);
	if (ret) {
		dev_err(&pdev->dev, "%s:  Failed to register handler: %pe\n",
			__func__, ERR_PTR(ret));
		return ret;
	}

	platform_set_drvdata(pdev, data);

	ret = nct6694_get_irq_trig(data);
	if (ret)
		return ret;

	/* Register gpio chip to GPIO framework */
	girq = &data->gpio.irq;
	gpio_irq_chip_set_chip(girq, &nct6694_irq_chip);
	girq->parent_handler = NULL;
	girq->num_parents = 0;
	girq->parents = NULL;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_level_irq;
	girq->threaded = true;

	ret = gpiochip_add_data(&data->gpio, data);
	if (ret) {
		dev_err(&pdev->dev, "%s: Failed to register GPIO chip: %pe",
			__func__, ERR_PTR(ret));
		return ret;
	}

	return 0;
}

static void nct6694_gpio_remove(struct platform_device *pdev)
{
	struct nct6694_gpio_data *data = platform_get_drvdata(pdev);

	gpiochip_remove(&data->gpio);
	cancel_work(&data->irq_work);
	cancel_work(&data->irq_trig_work);
}

static struct platform_driver nct6694_gpio_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_gpio_probe,
	.remove		= nct6694_gpio_remove,
};

static int __init nct6694_init(void)
{
	int err;

	err = platform_driver_register(&nct6694_gpio_driver);
	if (!err) {
		if (err)
			platform_driver_unregister(&nct6694_gpio_driver);
	}

	return err;
}
subsys_initcall(nct6694_init);

static void __exit nct6694_exit(void)
{
	platform_driver_unregister(&nct6694_gpio_driver);
}
module_exit(nct6694_exit);

MODULE_DESCRIPTION("USB-GPIO controller driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
