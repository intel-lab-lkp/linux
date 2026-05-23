// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 NVIDIA Corporation
 * Copyright (C) 2023 Svyatoslav Ryhel <clamor95@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pwrseq/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/rfkill.h>
#include <linux/usb.h>

#define BASEBAND_XMM_INIT_DELAY		5000

#define BASEBAND_PRODUCT_ID_XMM6260	0x0020
#define BASEBAND_VENDOR_ID_COMNEON	0x1519

/*
 * Starting from ver 1145 modem starts in the IPC_AP_WAKE_IRQ_READY state.
 * AP wake interrupt keeps low util CP starts to initiate its HSIC hw. AP wake
 * interrupt goes up during CP HSIC init (BASEBAND_XMM_IPC_AP_WAKE_INIT1 state)
 * at this point Host USB bus must be configured in order for modem to set
 * properly. Then interrupt goes down when CP HSIC is ready
 * (BASEBAND_XMM_IPC_AP_WAKE_INIT2 state) and at this point XMM6260 USB device
 * should appear and be accessible for further work. In case it does not, the
 * cycle must repeat.
 */

/* bits [0:2] */
enum baseband_xmm_ipc_ap_wake_state {
	BASEBAND_XMM_IPC_AP_WAKE_IRQ_READY,
	BASEBAND_XMM_IPC_AP_WAKE_INIT1,
	BASEBAND_XMM_IPC_AP_WAKE_INIT2,
	BASEBAND_XMM_IPC_AP_WAKE_L,
	BASEBAND_XMM_IPC_AP_WAKE_H,
	BASEBAND_XMM_IPC_AP_WAKE_UNINIT,
	BASEBAND_XMM_IPC_AP_WAKE_MASK = 7,
};

#define BASEBAND_XMM_IPC_AP_WAKE_MAX	3

#define BASEBAND_XMM_STATE_POWERED	3 /* Tracks regulator state */
#define BASEBAND_XMM_STATE_PROTECTED	4 /* Prevents rfkill from access */
#define BASEBAND_XMM_STATE_PRESENT	5 /* Tracks USB device presence */
#define BASEBAND_XMM_STATE_POWEROFF	6 /* Prevents poweroff recursive call */
#define BASEBAND_XMM_STATE_MAX		8

struct baseband_xmm_data {
	struct device *dev;
	struct rfkill *rfkill_dev;
	struct pwrseq_desc *pwrseq;

	DECLARE_BITMAP(state, BASEBAND_XMM_STATE_MAX);
	int irq;

	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;

	struct gpio_desc *ipc_cp_gpio;
	struct gpio_desc *ipc_ap_gpio;

	struct regulator *vbat_supply;

	struct delayed_work modem_work;
	struct notifier_block nb;
};

static int get_ipc_ap_wake(struct baseband_xmm_data *priv)
{
	int i, ret = 0;

	for (i = 0; i < BASEBAND_XMM_IPC_AP_WAKE_MAX; i++)
		ret |= (test_bit(i, priv->state) << i);

	return ret;
}

static void set_ipc_ap_wake(struct baseband_xmm_data *priv,
			    enum baseband_xmm_ipc_ap_wake_state state)
{
	for (int i = 0; i < BASEBAND_XMM_IPC_AP_WAKE_MAX; i++)
		if (state & BIT(i))
			set_bit(i, priv->state);
		else
			clear_bit(i, priv->state);
}

static void baseband_xmm_reset(struct baseband_xmm_data *priv)
{
	int ret;

	set_bit(BASEBAND_XMM_STATE_PROTECTED, priv->state);

	if (!test_bit(BASEBAND_XMM_STATE_POWERED, priv->state)) {
		ret = regulator_enable(priv->vbat_supply);
		if (ret)
			dev_err(priv->dev,
				"failed to enable vbat power supply\n");

		set_bit(BASEBAND_XMM_STATE_POWERED, priv->state);
	}

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

static void baseband_xmm_poweroff(struct baseband_xmm_data *priv)
{
	/*
	 * The test_bit check prevents poweroff from being recursively
	 * called during USB device deregistration. USB device
	 * deregistration can be triggered by the driver by calling this
	 * function or by some external event. The first case will cause
	 * a recursive call by the notifier if not handled, while the
	 * second case requires this call to handle the USB controller
	 * properly.
	 */
	if (test_bit(BASEBAND_XMM_STATE_POWEROFF, priv->state))
		return;

	set_bit(BASEBAND_XMM_STATE_PROTECTED, priv->state);
	set_bit(BASEBAND_XMM_STATE_POWEROFF, priv->state);

	pwrseq_power_off(priv->pwrseq);
	gpiod_set_value_cansleep(priv->reset_gpio, 1);

	if (test_bit(BASEBAND_XMM_STATE_POWERED, priv->state)) {
		regulator_disable(priv->vbat_supply);
		clear_bit(BASEBAND_XMM_STATE_POWERED, priv->state);
	}
	set_ipc_ap_wake(priv, BASEBAND_XMM_IPC_AP_WAKE_IRQ_READY);

	clear_bit(BASEBAND_XMM_STATE_PROTECTED, priv->state);
	clear_bit(BASEBAND_XMM_STATE_PRESENT, priv->state);
	clear_bit(BASEBAND_XMM_STATE_POWEROFF, priv->state);
}

static int baseband_xmm_usb_notifier_call(struct notifier_block *nb,
					  unsigned long action, void *data)
{
	struct baseband_xmm_data *priv =
		container_of(nb, struct baseband_xmm_data, nb);
	struct usb_device *udev;
	u16 product, vendor;

	if (action == USB_BUS_ADD || action == USB_BUS_REMOVE)
		return NOTIFY_OK;

	udev = data;
	product = le16_to_cpu(udev->descriptor.idProduct);
	vendor = le16_to_cpu(udev->descriptor.idVendor);

	switch (action) {
	case USB_DEVICE_ADD:
		/* Infineon XMM6260 ID 1519:0020 */
		if (vendor == BASEBAND_VENDOR_ID_COMNEON &&
		    product == BASEBAND_PRODUCT_ID_XMM6260) {
			cancel_delayed_work_sync(&priv->modem_work);
			clear_bit(BASEBAND_XMM_STATE_PROTECTED, priv->state);
			set_bit(BASEBAND_XMM_STATE_PRESENT, priv->state);
		}
		break;

	case USB_DEVICE_REMOVE:
		/* Infineon XMM6260 ID 1519:0020 */
		if (vendor == BASEBAND_VENDOR_ID_COMNEON &&
		    product == BASEBAND_PRODUCT_ID_XMM6260)
			baseband_xmm_poweroff(priv);
		break;

	default:
		break;
	}

	return NOTIFY_OK;
}

static int baseband_xmm_set_block(void *data, bool blocked)
{
	struct baseband_xmm_data *priv = data;

	if (test_bit(BASEBAND_XMM_STATE_PROTECTED, priv->state))
		return -EBUSY;

	if (blocked && test_bit(BASEBAND_XMM_STATE_PRESENT, priv->state))
		baseband_xmm_poweroff(priv);
	else if (!blocked && !test_bit(BASEBAND_XMM_STATE_PRESENT, priv->state))
		baseband_xmm_reset(priv);

	return 0;
}

static const struct rfkill_ops baseband_xmm_rfkill_ops = {
	.set_block = baseband_xmm_set_block,
};

static void baseband_xmm_work(struct work_struct *work)
{
	struct baseband_xmm_data *priv =
		container_of(work, struct baseband_xmm_data, modem_work.work);

	baseband_xmm_poweroff(priv);
};

static irqreturn_t baseband_hostwake_interrupt(int irq, void *dev_id)
{
	struct baseband_xmm_data *priv = dev_id;
	int state = gpiod_get_value(priv->ipc_ap_gpio);

	switch (get_ipc_ap_wake(priv)) {
	case BASEBAND_XMM_IPC_AP_WAKE_IRQ_READY:
		if (!state) {
			set_ipc_ap_wake(priv, BASEBAND_XMM_IPC_AP_WAKE_INIT1);
			pwrseq_power_on(priv->pwrseq);
		}
		break;

	case BASEBAND_XMM_IPC_AP_WAKE_INIT1:
		if (state) {
			set_ipc_ap_wake(priv, BASEBAND_XMM_IPC_AP_WAKE_INIT2);
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
	int ret;

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

	/* Modem power sequence */
	priv->pwrseq = devm_pwrseq_get(dev, "modem-power");
	if (IS_ERR(priv->pwrseq))
		return dev_err_probe(dev, PTR_ERR(priv->pwrseq),
				     "failed to get modem pwrseq");

	bitmap_zero(priv->state, BASEBAND_XMM_STATE_MAX);
	INIT_DELAYED_WORK(&priv->modem_work, baseband_xmm_work);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return dev_err_probe(dev, priv->irq, "failed to get IRQ\n");

	/*
	 * Systems using device tree should set up interrupt via DT,
	 * the rest will use the default edge both interrupt.
	 */
	irqflags = dev->of_node ? 0 : IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;

	ret = devm_request_threaded_irq(dev, priv->irq, NULL,
					&baseband_hostwake_interrupt,
					IRQF_ONESHOT | irqflags,
					"modem-hostwake", priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IRQ %d\n", priv->irq);

	priv->rfkill_dev = rfkill_alloc("xmm-modem", dev, RFKILL_TYPE_WWAN,
					&baseband_xmm_rfkill_ops, priv);
	if (!priv->rfkill_dev)
		return -ENOMEM;

	ret = rfkill_register(priv->rfkill_dev);
	if (ret) {
		rfkill_destroy(priv->rfkill_dev);
		return dev_err_probe(dev, ret,
				     "failed to register WWAN rfkill\n");
	}

	priv->nb.notifier_call = baseband_xmm_usb_notifier_call;
	usb_register_notify(&priv->nb);

	return 0;
}

static void baseband_xmm_remove(struct platform_device *pdev)
{
	struct baseband_xmm_data *priv = platform_get_drvdata(pdev);

	rfkill_unregister(priv->rfkill_dev);
	rfkill_destroy(priv->rfkill_dev);

	disable_irq(priv->irq);
	cancel_delayed_work_sync(&priv->modem_work);

	usb_unregister_notify(&priv->nb);
	baseband_xmm_poweroff(priv);
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
