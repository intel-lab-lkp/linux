// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <uapi/linux/uleds.h>

#include "fbnic.h"
#include "fbnic_csr.h"
#include "fbnic_mac.h"
#include "fbnic_netdev.h"

#define led_classdev_to_fbnic_led(cdev)  \
	container_of(cdev, struct fbnic_led_cdev, led)

static const char *fbnic_led_names[FBNIC_NUM_LEDS] = {
	"activity",
	"link-amber",
	"link-blue"
};

static void fbnic_led_get_name(struct fbnic_dev *fbd,
			       int led_idx, char *name)
{
	snprintf(name, LED_MAX_NAME_SIZE, "%s-%s",
		 fbd->netdev->name, fbnic_led_names[led_idx]);
}

static int fbnic_led_get_idx(struct fbnic_dev *fbd,
			     struct fbnic_led_cdev *ldev)
{
	return ldev - &fbd->leds[0];
}

static struct fbnic_dev *led_cdev_to_fbd(struct led_classdev *led_cdev)
{
	struct fbnic_led_cdev *ldev = led_classdev_to_fbnic_led(led_cdev);

	return ldev->fbd;
}

static u32 fbnic_led_get_supported_modes(struct fbnic_dev *fbd, int led_idx)
{
	u32 modes = 0;

	if (led_idx == FBNIC_LED_ACTIVITY) {
		modes = BIT(TRIGGER_NETDEV_TX) | BIT(TRIGGER_NETDEV_RX);

		return modes;
	}

	/* Set modes for link LEDs - note 40G not supported */
	modes = BIT(TRIGGER_NETDEV_LINK) |
		BIT(TRIGGER_NETDEV_LINK_25000) |
		BIT(TRIGGER_NETDEV_LINK_50000) |
		BIT(TRIGGER_NETDEV_LINK_100000);

	return modes;
}

static int fbnic_led_hw_ctl_set(struct led_classdev *led_cdev,
				unsigned long flags)
{
	struct fbnic_led_cdev *ldev = led_classdev_to_fbnic_led(led_cdev);
	struct fbnic_dev *fbd = led_cdev_to_fbd(led_cdev);
	int led_idx = fbnic_led_get_idx(fbd, ldev);
	u32 supported_modes;

	supported_modes = fbnic_led_get_supported_modes(fbd, led_idx);

	if (flags & ~supported_modes)
		return -EINVAL;

	/* Turn on activity LED only when both the TX and RX flags are set. */
	if (led_idx == FBNIC_LED_ACTIVITY && (flags & supported_modes) &&
	    flags != supported_modes) {
		dev_warn(fbd->dev,
			 "Invalid activity LED mode(s): 0x%lx\n", flags);
		return -EINVAL;
	}

	/* Preserve the configured modes for restoration after LED strobe */
	mutex_lock(&fbd->led_mutex);
	ldev->enabled_modes = flags;

	if (ldev->strobe_mode)
		dev_warn(fbd->dev,
			 "LED config takes effect after strobe completes.\n");

	fbnic_led_update_csr(fbd);
	mutex_unlock(&fbd->led_mutex);

	return 0;
}

static int fbnic_led_hw_ctl_get(struct led_classdev *led_cdev,
				unsigned long *flags)
{
	struct fbnic_led_cdev *ldev = led_classdev_to_fbnic_led(led_cdev);
	struct fbnic_dev *fbd = led_cdev_to_fbd(led_cdev);

	mutex_lock(&fbd->led_mutex);

	*flags = ldev->enabled_modes;

	mutex_unlock(&fbd->led_mutex);

	return 0;
}

static struct device *fbnic_led_hw_ctl_get_device(struct led_classdev *led_cdev)
{
	struct fbnic_dev *fbd = led_cdev_to_fbd(led_cdev);
	struct net_device *netdev = fbd->netdev;

	return &netdev->dev;
}

static int fbnic_led_hw_ctl_is_supported(struct led_classdev *led_cdev,
					 unsigned long flags)
{
	struct fbnic_led_cdev *ldev = led_classdev_to_fbnic_led(led_cdev);
	struct fbnic_dev *fbd = led_cdev_to_fbd(led_cdev);
	int led_idx = fbnic_led_get_idx(fbd, ldev);
	u32 modes;

	modes = fbnic_led_get_supported_modes(fbd, led_idx);

	if (led_idx == FBNIC_LED_ACTIVITY && (flags & modes) && flags != modes)
		return -EOPNOTSUPP;
	if (flags & ~modes)
		return -EOPNOTSUPP;

	return 0;
}

static int fbnic_led_brightness_set_blocking(struct led_classdev *led_cdev,
					     enum led_brightness brightness)
{
	struct fbnic_led_cdev *ldev = led_classdev_to_fbnic_led(led_cdev);
	struct fbnic_dev *fbd = led_cdev_to_fbd(led_cdev);
	int led_idx = fbnic_led_get_idx(fbd, ldev);

	mutex_lock(&fbd->led_mutex);
	if (!brightness) {
		fbd->leds[led_idx].enabled_modes = 0;
		fbd->leds[led_idx].strobe_mode = 0;
	} else {
		u32 mode;

		switch (led_idx) {
		case FBNIC_LED_ACTIVITY:
			fbd->leds[led_idx].enabled_modes =
				BIT(TRIGGER_NETDEV_TX) | BIT(TRIGGER_NETDEV_RX);
			break;
		default:
			mode = fbnic_led_get_link_speed_mode(fbd);
			fbd->leds[led_idx].enabled_modes = mode;
			break;
		}
	}

	fbnic_led_update_csr(fbd);

	mutex_unlock(&fbd->led_mutex);

	return 0;
}

static int fbnic_led_setup(struct fbnic_dev *fbd, int led_idx)
{
	struct pci_dev *pdev = to_pci_dev(fbd->dev);
	struct led_classdev *led_cdev;

	fbd->leds[led_idx].fbd = fbd;
	led_cdev = &fbd->leds[led_idx].led;
	led_cdev->name = fbd->leds[led_idx].name;
	fbnic_led_get_name(fbd, led_idx, fbd->leds[led_idx].name);
	led_cdev->max_brightness = 1;
	led_cdev->hw_control_trigger = "netdev";
	led_cdev->flags |= LED_RETAIN_AT_SHUTDOWN;
	led_cdev->hw_control_set = fbnic_led_hw_ctl_set;
	led_cdev->hw_control_get = fbnic_led_hw_ctl_get;
	led_cdev->hw_control_get_device = fbnic_led_hw_ctl_get_device;
	led_cdev->hw_control_is_supported = fbnic_led_hw_ctl_is_supported;
	led_cdev->brightness_set_blocking = fbnic_led_brightness_set_blocking;

	return led_classdev_register(&pdev->dev, led_cdev);
}

/**
 * fbnic_led_init - initialize the linux led interface for fbnic
 *
 * @fbd: FBNIC device structure
 *
 * Return: zero on success, negative value on failure
 *
 * This function creates three led devices for the fbnic device. One for the
 * activity LED and two for the color LEDs. The successful initialization
 * creates <netdev>-activity, <netdev>-link-amber and <netdev>-link-blue
 * under /sys/class/leds/
 */
int fbnic_led_init(struct fbnic_dev *fbd)
{
	int i, ret;

	for (i = 0; i < FBNIC_NUM_LEDS; i++) {
		ret = fbnic_led_setup(fbd, i);

		if (ret)
			goto err_led_setup;
	}

	return 0;

err_led_setup:
	while (i--)
		led_classdev_unregister(&fbd->leds[i].led);

	return ret;
}

void fbnic_led_exit(struct fbnic_dev *fbd)
{
	int i;

	for (i = 0; i < FBNIC_NUM_LEDS; i++)
		led_classdev_unregister(&fbd->leds[i].led);
}
