// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 NVIDIA Corporation
 * Copyright (C) 2023 Svyatoslav Ryhel <clamor95@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/devm-helpers.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/rfkill.h>
#include <linux/usb.h>

#define BASEBAND_XMM_INIT_DELAY		5000

#define BASEBAND_PRODUCT_ID_XMM6260	0x0020
#define BASEBAND_VENDOR_ID_COMNEON	0x1519

enum ipc_ap_wake_state {
	IPC_AP_WAKE_IRQ_READY,
	IPC_AP_WAKE_INIT1,
	IPC_AP_WAKE_INIT2,
	IPC_AP_WAKE_L,
	IPC_AP_WAKE_H,
	IPC_AP_WAKE_UNINIT,
};

struct baseband_xmm_data {
	struct device *dev;
	struct rfkill *rfkill_dev;
	struct phy *mphy;

	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;

	struct gpio_desc *ipc_cp_gpio;
	struct gpio_desc *ipc_ap_gpio;

	struct regulator *vbat_supply;

	struct delayed_work modem_work;
	struct notifier_block nb;

	enum ipc_ap_wake_state ap_state;

	bool powered; /* tracks usb bus state */
	bool inited; /* tracks modem state */
};

static int baseband_xmm_usb_notifier_call(struct notifier_block *nb,
					  unsigned long action, void *data)
{
	struct baseband_xmm_data *priv =
		container_of(nb, struct baseband_xmm_data, nb);
	struct usb_device *udev = data;
	u16 product = le16_to_cpu(udev->descriptor.idProduct);
	u16 vendor = le16_to_cpu(udev->descriptor.idVendor);

	switch (action) {
	case USB_DEVICE_ADD:
		/* Infineon XMM6260 ID 1519:0020 */
		if (vendor == BASEBAND_VENDOR_ID_COMNEON &&
		    product == BASEBAND_PRODUCT_ID_XMM6260) {
			cancel_delayed_work_sync(&priv->modem_work);
			priv->inited = true;
		}
		break;
	}

	return NOTIFY_OK;
}

static void baseband_xmm_reset(struct baseband_xmm_data *priv)
{
	int ret;

	ret = regulator_enable(priv->vbat_supply);
	if (ret)
		dev_err(priv->dev, "failed to enable vbat power supply\n");

	gpiod_set_value_cansleep(priv->enable_gpio, 0);
	msleep(50);

	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	msleep(200);
	gpiod_set_value_cansleep(priv->reset_gpio, 0);

	msleep(50);

	/* falling edge trigger to CP */
	gpiod_set_value_cansleep(priv->enable_gpio, 1);
	usleep_range(50, 100);
	gpiod_set_value_cansleep(priv->enable_gpio, 0);
	msleep(20);
}

static int baseband_xmm_set_block(void *data, bool blocked)
{
	struct baseband_xmm_data *priv = data;

	if (blocked) {
		if (priv->inited && priv->powered) {
			phy_power_off(priv->mphy);

			msleep(500);

			gpiod_set_value_cansleep(priv->reset_gpio, 1);
			regulator_disable(priv->vbat_supply);

			priv->powered = false;
			priv->inited = false;
		}
	} else {
		if (priv->inited)
			return 0;

		priv->ap_state = IPC_AP_WAKE_IRQ_READY;
		baseband_xmm_reset(priv);

		priv->powered = false;
		priv->inited = false;
	}

	return 0;
}

static const struct rfkill_ops baseband_xmm_rfkill_ops = {
	.set_block = baseband_xmm_set_block,
};

static void baseband_xmm_work(struct work_struct *work)
{
	struct baseband_xmm_data *priv =
		container_of(work, struct baseband_xmm_data, modem_work.work);

	switch (priv->ap_state) {
	case IPC_AP_WAKE_INIT1:
		if (priv->powered)
			return;

		phy_power_on(priv->mphy);
		priv->powered = true;
		break;

	case IPC_AP_WAKE_INIT2:
		priv->ap_state = IPC_AP_WAKE_IRQ_READY;

		phy_power_off(priv->mphy);

		priv->powered = false;
		priv->inited = false;

		msleep(500);
		break;

	default:
		break;
	}
};

static irqreturn_t baseband_hostwake_interrupt(int irq, void *dev_id)
{
	struct baseband_xmm_data *priv = dev_id;
	int state = gpiod_get_value(priv->ipc_ap_gpio);

	switch (priv->ap_state) {
	case IPC_AP_WAKE_IRQ_READY:
		if (!state) {
			priv->ap_state = IPC_AP_WAKE_INIT1;
			schedule_delayed_work(&priv->modem_work, 0);
		}

		break;

	case IPC_AP_WAKE_INIT1:
		if (state) {
			priv->ap_state = IPC_AP_WAKE_INIT2;
			schedule_delayed_work(&priv->modem_work,
					      msecs_to_jiffies(BASEBAND_XMM_INIT_DELAY));
		}

		break;

	default:
		break;
	}

	return IRQ_HANDLED;
}

static int baseband_xmm_probe(struct platform_device *pdev)
{
	struct baseband_xmm_data *priv;
	struct device *dev = &pdev->dev;
	unsigned long irqflags;
	int irq, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	priv->vbat_supply = devm_regulator_get(dev, "vbat");
	if (IS_ERR(priv->vbat_supply))
		return dev_err_probe(dev, PTR_ERR(priv->vbat_supply),
				     "failed to get vbat regulator\n");

	/* Own modem gpios */
	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						   GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				     "failed to get reset GPIO\n");

	priv->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						    GPIOD_OUT_LOW);
	if (IS_ERR(priv->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->enable_gpio),
				     "failed to get enable GPIO\n");

	/* CP - AP connections */
	priv->ipc_cp_gpio = devm_gpiod_get_optional(dev, "cp-wake",
						    GPIOD_OUT_LOW);
	if (IS_ERR(priv->ipc_cp_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->ipc_cp_gpio),
				     "failed to get CP wake GPIO\n");

	priv->ipc_ap_gpio = devm_gpiod_get_optional(dev, "ap-wake", GPIOD_IN);
	if (IS_ERR(priv->ipc_ap_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->ipc_ap_gpio),
				     "failed to get AP wake GPIO\n");

	/* Modem PHY */
	priv->mphy = devm_phy_optional_get(dev, NULL);
	if (IS_ERR(priv->mphy))
		return dev_err_probe(dev, PTR_ERR(priv->mphy),
				     "failed to get modem PHY");

	/*
	 * Strting from ver 1145 modem starts in READY state. AP wake
	 * interrupt keeps low util CP starts to initiate HSIC hw. AP
	 * wake interrupt goes up during CP HSIC init and then it goes
	 * down when CP HSIC is ready.
	 */
	priv->ap_state = IPC_AP_WAKE_IRQ_READY;
	priv->inited = false;

	devm_delayed_work_autocancel(dev, &priv->modem_work, baseband_xmm_work);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return dev_err_probe(dev, irq, "failed to get IRQ\n");

	/*
	 * Systems using device tree should set up interrupt via DT,
	 * the rest will use the default edge both interrupt.
	 */
	irqflags = dev->of_node ? 0 : IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;

	ret = devm_request_threaded_irq(dev, irq, NULL,
					&baseband_hostwake_interrupt,
					IRQF_ONESHOT | irqflags,
					"modem-hostwake", priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IRQ %d\n", irq);

	priv->rfkill_dev = rfkill_alloc("xmm-modem", dev, RFKILL_TYPE_WWAN,
					&baseband_xmm_rfkill_ops, priv);
	if (priv->rfkill_dev) {
		ret = rfkill_register(priv->rfkill_dev);
		if (ret < 0) {
			rfkill_destroy(priv->rfkill_dev);
			return dev_err_probe(dev, ret,
					     "failed to register WWAN rfkill\n");
		}
	} else {
		return dev_err_probe(dev, PTR_ERR(priv->rfkill_dev),
				     "failed to allocate WWAN rfkill\n");
	}

	priv->nb.notifier_call = baseband_xmm_usb_notifier_call;
	usb_register_notify(&priv->nb);

	baseband_xmm_reset(priv);
	priv->powered = false;

	return 0;
}

static void baseband_xmm_remove(struct platform_device *pdev)
{
	struct baseband_xmm_data *priv = platform_get_drvdata(pdev);

	rfkill_unregister(priv->rfkill_dev);
	rfkill_destroy(priv->rfkill_dev);

	usb_unregister_notify(&priv->nb);
	phy_power_off(priv->mphy);

	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	regulator_disable(priv->vbat_supply);
}

static const struct of_device_id baseband_xmm_match[] = {
	{ .compatible = "infineon,xmm6260" },
	{ }
};
MODULE_DEVICE_TABLE(of, baseband_xmm_match);

static struct platform_driver baseband_xmm_driver = {
	.driver = {
		.name = "baseband-xmm6260",
		.of_match_table = baseband_xmm_match,
	},
	.probe = baseband_xmm_probe,
	.remove = baseband_xmm_remove,
};
module_platform_driver(baseband_xmm_driver);

MODULE_AUTHOR("Svyatolsav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Baseband Infineon XMM6260 driver");
MODULE_LICENSE("GPL");
