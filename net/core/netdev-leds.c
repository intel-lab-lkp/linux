// SPDX-License-Identifier: GPL-2.0+

#include <linux/device.h>
#include <linux/err.h>
#include <linux/leds.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <net/netdev_leds.h>

struct netdev_led {
	struct list_head led_list;
	struct led_classdev led_cdev;
	struct netdev_leds_ops *ops;
	struct net_device *ndev;
	u8 index;
};

#define to_netdev_led(d) container_of(d, struct netdev_led, led_cdev)

static int netdev_brightness_set(struct led_classdev *led_cdev,
				 enum led_brightness value)
{
	struct netdev_led *netdev_led = to_netdev_led(led_cdev);

	return netdev_led->ops->brightness_set(netdev_led->ndev,
					       netdev_led->index,
					       value);
}

static int netdev_blink_set(struct led_classdev *led_cdev,
			    unsigned long *delay_on, unsigned long *delay_off)
{
	struct netdev_led *netdev_led = to_netdev_led(led_cdev);

	return netdev_led->ops->blink_set(netdev_led->ndev,
					  netdev_led->index,
					  delay_on, delay_off);
}

static __maybe_unused int
netdev_hw_control_is_supported(struct led_classdev *led_cdev,
			       unsigned long flags)
{
	struct netdev_led *netdev_led = to_netdev_led(led_cdev);

	return netdev_led->ops->hw_control_is_supported(netdev_led->ndev,
							netdev_led->index,
							flags);
}

static __maybe_unused int netdev_hw_control_set(struct led_classdev *led_cdev,
						unsigned long flags)
{
	struct netdev_led *netdev_led = to_netdev_led(led_cdev);

	return netdev_led->ops->hw_control_set(netdev_led->ndev,
					       netdev_led->index,
					       flags);
}

static __maybe_unused int netdev_hw_control_get(struct led_classdev *led_cdev,
						unsigned long *flags)
{
	struct netdev_led *netdev_led = to_netdev_led(led_cdev);

	return netdev_led->ops->hw_control_get(netdev_led->ndev,
					       netdev_led->index,
					       flags);
}

static struct device *
netdev_hw_control_get_device(struct led_classdev *led_cdev)
{
	struct netdev_led *netdev_led = to_netdev_led(led_cdev);

	return &netdev_led->ndev->dev;
}

static int netdev_led_setup(struct net_device *ndev, struct device_node *led,
			    struct list_head *list, struct netdev_leds_ops *ops)
{
	struct led_init_data init_data = {};
	struct device *dev = &ndev->dev;
	struct netdev_led *netdev_led;
	struct led_classdev *cdev;
	u32 index;
	int err;

	netdev_led = devm_kzalloc(dev, sizeof(*netdev_led), GFP_KERNEL);
	if (!netdev_led)
		return -ENOMEM;

	netdev_led->ndev = ndev;
	netdev_led->ops = ops;
	cdev = &netdev_led->led_cdev;

	err = of_property_read_u32(led, "reg", &index);
	if (err)
		return err;

	if (index > 255)
		return -EINVAL;

	netdev_led->index = index;

	if (ops->brightness_set)
		cdev->brightness_set_blocking = netdev_brightness_set;
	if (ops->blink_set)
		cdev->blink_set = netdev_blink_set;
#ifdef CONFIG_LEDS_TRIGGERS
	if (ops->hw_control_is_supported)
		cdev->hw_control_is_supported = netdev_hw_control_is_supported;
	if (ops->hw_control_set)
		cdev->hw_control_set = netdev_hw_control_set;
	if (ops->hw_control_get)
		cdev->hw_control_get = netdev_hw_control_get;
	cdev->hw_control_trigger = "netdev";
#endif
	cdev->hw_control_get_device = netdev_hw_control_get_device;
	cdev->max_brightness = 1;
	init_data.fwnode = of_fwnode_handle(led);
	init_data.devname_mandatory = true;

	init_data.devicename = dev_name(dev);
	err = devm_led_classdev_register_ext(dev, cdev, &init_data);
	if (err)
		return err;

	INIT_LIST_HEAD(&netdev_led->led_list);
	list_add(&netdev_led->led_list, list);

	return 0;
}

/**
 * netdev_leds_setup - Parse DT node and create LEDs for netdev
 *
 * @ndev: struct netdev for the MAC
 * @np: ethernet-node in device tree
 * @list: list to add LEDs to
 * @ops: structure of ops to manipulate the LED.
 *
 * Parse the device tree node, as described in ethernet-controller.yaml,
 * and find any LEDs. For each LED found, create an LED and register
 * it with the LED subsystem. The LED will be added to the list, which can
 * be shared by all netdevs of the device. The ops structure contains the
 * callbacks needed to control the LEDs.
 *
 * Return 0 in success, otherwise an negative error code.
 */
int netdev_leds_setup(struct net_device *ndev, struct device_node *np,
		      struct list_head *list, struct netdev_leds_ops *ops)
{
	struct device_node *leds, *led;
	int err;

	leds = of_get_child_by_name(np, "leds");
	if (!leds)
		return 0;

	for_each_available_child_of_node(leds, led) {
		err = netdev_led_setup(ndev, led, list, ops);
		if (err) {
			of_node_put(led);
			return err;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(netdev_leds_setup);

/**
 * netdev_leds_teardown - Remove LEDs for a netdev
 *
 * @list: list to add LEDs to teardown
 * @ndev: The netdev for which LEDs should be removed
 *
 * Unregister all LEDs for a given netdev, freeing up any allocated
 * memory.
 */
void netdev_leds_teardown(struct list_head *list, struct net_device *ndev)
{
	struct netdev_led *netdev_led;
	struct led_classdev *cdev;
	struct device *dev;

	list_for_each_entry(netdev_led, list, led_list) {
		if (netdev_led->ndev != ndev)
			continue;
		dev = &netdev_led->ndev->dev;
		cdev = &netdev_led->led_cdev;
		devm_led_classdev_unregister(dev, cdev);
	}
}
EXPORT_SYMBOL_GPL(netdev_leds_teardown);
