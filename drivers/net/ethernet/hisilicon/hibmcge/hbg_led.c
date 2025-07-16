// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2025 Hisilicon Limited.

#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/phy.h>
#include "hbg_common.h"
#include "hbg_led.h"

#define PHY_ID_YT8521		0x0000011a

#define to_hbg_led(lcdev) container_of(lcdev, struct hbg_led_classdev, led)
#define to_hbg_phy_dev(lcdev) \
	(((struct hbg_led_classdev *)to_hbg_led(lcdev))->priv->mac.phydev)

static int hbg_led_hw_control_set(struct led_classdev *led_cdev,
				  unsigned long rules)
{
	struct hbg_led_classdev *hbg_led = to_hbg_led(led_cdev);
	struct phy_device *phydev = to_hbg_phy_dev(led_cdev);
	int ret;

	mutex_lock(&phydev->lock);
	ret = phydev->drv->led_hw_control_set(phydev, hbg_led->index, rules);
	mutex_unlock(&phydev->lock);

	return ret;
}

static int hbg_led_hw_control_get(struct led_classdev *led_cdev,
				  unsigned long *rules)
{
	struct hbg_led_classdev *hbg_led = to_hbg_led(led_cdev);
	struct phy_device *phydev = to_hbg_phy_dev(led_cdev);
	int ret;

	mutex_lock(&phydev->lock);
	ret = phydev->drv->led_hw_control_get(phydev, hbg_led->index, rules);
	mutex_unlock(&phydev->lock);

	return ret;
}

static int hbg_led_hw_is_supported(struct led_classdev *led_cdev,
				   unsigned long rules)
{
	struct hbg_led_classdev *hbg_led = to_hbg_led(led_cdev);
	struct phy_device *phydev = to_hbg_phy_dev(led_cdev);
	int ret;

	mutex_lock(&phydev->lock);
	ret = phydev->drv->led_hw_is_supported(phydev, hbg_led->index, rules);
	mutex_unlock(&phydev->lock);

	return ret;
}

static struct device *
	hbg_led_hw_control_get_device(struct led_classdev *led_cdev)
{
	struct hbg_led_classdev *hbg_led = to_hbg_led(led_cdev);

	return &hbg_led->priv->netdev->dev;
}

static int hbg_setup_ldev(struct hbg_led_classdev *hbg_led)
{
	struct led_classdev *ldev = &hbg_led->led;
	struct hbg_priv *priv = hbg_led->priv;
	struct device *dev = &priv->pdev->dev;

	ldev->name = devm_kasprintf(dev, GFP_KERNEL, "%s-%s-%d",
				    dev_driver_string(dev),
				    pci_name(priv->pdev), hbg_led->index);
	if (!ldev->name)
		return -ENOMEM;

	ldev->hw_control_trigger = "netdev";
	ldev->hw_control_set = hbg_led_hw_control_set;
	ldev->hw_control_get = hbg_led_hw_control_get;
	ldev->hw_control_is_supported = hbg_led_hw_is_supported;
	ldev->hw_control_get_device = hbg_led_hw_control_get_device;

	return devm_led_classdev_register(dev, ldev);
}

static u32 hbg_get_phy_max_led_count(struct hbg_priv *priv)
{
	struct phy_device *phydev = priv->mac.phydev;

	if (!phydev->drv->led_hw_is_supported ||
	    !phydev->drv->led_hw_control_set ||
	    !phydev->drv->led_hw_control_get)
		return 0;

	/* YT8521, support 3 leds */
	if (phydev->drv->phy_id == PHY_ID_YT8521)
		return 3;

	return 0;
}

int hbg_leds_init(struct hbg_priv *priv)
{
	u32 led_count = hbg_get_phy_max_led_count(priv);
	struct phy_device *phydev = priv->mac.phydev;
	struct hbg_led_classdev *leds;
	int ret;
	int i;

	if (!led_count)
		return 0;

	leds = devm_kcalloc(&priv->pdev->dev, led_count,
			    sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	for (i = 0; i < led_count; i++) {
		/* for YT8521, we only have two lights, 0 and 2. */
		if (phydev->drv->phy_id == PHY_ID_YT8521 && i == 1)
			continue;

		leds[i].priv = priv;
		leds[i].index = i;
		ret = hbg_setup_ldev(&leds[i]);
		if (ret)
			return ret;
	}

	return 0;
}
