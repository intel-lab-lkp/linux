// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the PF1550 ON_KEY
 * Copyright (C) 2016 Freescale Semiconductor, Inc. All Rights Reserved.
 */

#include <linux/err.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/pf1550.h>
#include <linux/platform_device.h>

#define PF1550_ONKEY_IRQ_NR	6

struct onkey_drv_data {
	struct device *dev;
	struct pf1550_dev *pf1550;
	unsigned int irq;
	int keycode;
	int wakeup;
	struct input_dev *input;
};

static irqreturn_t pf1550_onkey_irq_handler(int irq, void *data)
{
	struct onkey_drv_data *onkey = data;
	struct irq_domain *domain;
	int i, state, irq_type = -1;
	unsigned int virq;

	domain = regmap_irq_get_domain(onkey->pf1550->irq_data_onkey);
	onkey->irq = irq;

	for (i = 0; i < PF1550_ONKEY_IRQ_NR; i++) {
		virq = irq_find_mapping(domain, i);
		if (onkey->irq == virq)
			irq_type = i;
	}

	switch (irq_type) {
	case PF1550_ONKEY_IRQ_PUSHI:
		state = 0;
		break;
	case PF1550_ONKEY_IRQ_1SI:
	case PF1550_ONKEY_IRQ_2SI:
	case PF1550_ONKEY_IRQ_3SI:
	case PF1550_ONKEY_IRQ_4SI:
	case PF1550_ONKEY_IRQ_8SI:
		state = 1;
		break;
	default:
		dev_err(onkey->dev, "onkey interrupt: irq %d occurred\n",
			irq_type);
		return IRQ_HANDLED;
	}

	input_event(onkey->input, EV_KEY, onkey->keycode, state);
	input_sync(onkey->input);

	return IRQ_HANDLED;
}

static int pf1550_onkey_probe(struct platform_device *pdev)
{
	struct onkey_drv_data *onkey;
	struct input_dev *input;
	struct pf1550_dev *pf1550 = dev_get_drvdata(pdev->dev.parent);
	struct irq_domain *domain;
	int i, error;

	onkey = devm_kzalloc(&pdev->dev, sizeof(*onkey), GFP_KERNEL);
	if (!onkey)
		return -ENOMEM;

	if (!pf1550->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "failed to get regmap\n");

	onkey->wakeup = device_property_read_bool(pdev->dev.parent,
						  "wakeup-source");

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "failed to allocate the input device\n");

	onkey->input = input;
	onkey->keycode = KEY_POWER;

	input->name = pdev->name;
	input->phys = "pf1550-onkey/input0";
	input->id.bustype = BUS_HOST;

	input_set_capability(input, EV_KEY, onkey->keycode);

	domain = regmap_irq_get_domain(pf1550->irq_data_onkey);

	for (i = 0; i < PF1550_ONKEY_IRQ_NR; i++) {
		unsigned int virq = irq_find_mapping(domain, i);

		error = devm_request_threaded_irq(&pdev->dev, virq, NULL,
						  pf1550_onkey_irq_handler,
						  IRQF_NO_SUSPEND,
						  "pf1550-onkey", onkey);
		if (error)
			return dev_err_probe(&pdev->dev, error,
					     "failed: irq request (IRQ: %d)\n",
					     i);
	}

	error = input_register_device(input);
	if (error < 0)
		return dev_err_probe(&pdev->dev, error,
				     "failed to register input device\n");

	onkey->pf1550 = pf1550;
	platform_set_drvdata(pdev, onkey);

	device_init_wakeup(&pdev->dev, onkey->wakeup);

	return 0;
}

static int pf1550_onkey_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct onkey_drv_data *onkey = platform_get_drvdata(pdev);
	struct irq_domain *domain;
	unsigned int virq;
	int i;

	domain = regmap_irq_get_domain(onkey->pf1550->irq_data_onkey);

	if (!device_may_wakeup(&pdev->dev)) {
		regmap_write(onkey->pf1550->regmap,
			     PF1550_PMIC_REG_ONKEY_INT_MASK0,
			     ONKEY_IRQ_PUSHI | ONKEY_IRQ_1SI | ONKEY_IRQ_2SI |
			     ONKEY_IRQ_3SI | ONKEY_IRQ_4SI | ONKEY_IRQ_8SI);
	} else {
		for (i = 0; i < PF1550_ONKEY_IRQ_NR; i++) {
			virq = irq_find_mapping(domain, i);

			if (virq)
				enable_irq_wake(virq);
		}
	}

	return 0;
}

static int pf1550_onkey_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct onkey_drv_data *onkey = platform_get_drvdata(pdev);
	struct irq_domain *domain;
	unsigned int virq;
	int i;

	domain = regmap_irq_get_domain(onkey->pf1550->irq_data_onkey);

	if (!device_may_wakeup(&pdev->dev)) {
		regmap_write(onkey->pf1550->regmap,
			     PF1550_PMIC_REG_ONKEY_INT_MASK0,
			     ~((u8)(ONKEY_IRQ_PUSHI | ONKEY_IRQ_1SI |
			     ONKEY_IRQ_2SI | ONKEY_IRQ_3SI | ONKEY_IRQ_4SI |
			     ONKEY_IRQ_8SI)));
	} else {
		for (i = 0; i < PF1550_ONKEY_IRQ_NR; i++) {
			virq = irq_find_mapping(domain, i);

			if (virq)
				disable_irq_wake(virq);
		}
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(pf1550_onkey_pm_ops, pf1550_onkey_suspend,
			 pf1550_onkey_resume);

static const struct platform_device_id pf1550_onkey_id[] = {
	{ "pf1550-onkey", PF1550 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, pf1550_onkey_id);

static struct platform_driver pf1550_onkey_driver = {
	.driver = {
		.name = "pf1550-onkey",
		.pm   = &pf1550_onkey_pm_ops,
	},
	.probe = pf1550_onkey_probe,
	.id_table = pf1550_onkey_id,
};
module_platform_driver(pf1550_onkey_driver);

MODULE_AUTHOR("Freescale Semiconductor");
MODULE_DESCRIPTION("PF1550 onkey Driver");
MODULE_LICENSE("GPL");
