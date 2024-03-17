/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Helpers used for creating and managing LEDs on a netdev MAC
 * driver.
 */

#ifndef _NET_NETDEV_LEDS_H
#define _NET_NETDEV_LEDS_H

struct netdev_leds_ops {
	int (*brightness_set)(struct net_device *ndev, u8 led,
			      enum led_brightness brightness);
	int (*blink_set)(struct net_device *ndev, u8 led,
			 unsigned long *delay_on,  unsigned long *delay_off);
	int (*hw_control_is_supported)(struct net_device *ndev, u8 led,
				       unsigned long flags);
	int (*hw_control_set)(struct net_device *ndev, u8 led,
			      unsigned long flags);
	int (*hw_control_get)(struct net_device *ndev, u8 led,
			      unsigned long *flags);
};

#ifdef CONFIG_NETDEV_LEDS
int netdev_leds_setup(struct net_device *ndev, struct device_node *np,
		      struct list_head *list, struct netdev_leds_ops *ops);

void netdev_leds_teardown(struct list_head *list, struct net_device *ndev);

#else
static inline int netdev_leds_setup(struct net_device *ndev,
				    struct device_node *np,
				    struct list_head *list,
				    struct netdev_leds_ops *ops)
{
	return 0;
}

static inline void netdev_leds_teardown(struct list_head *list,
					struct net_device *ndev)
{
}
#endif /* CONFIG_NETDEV_LEDS */

#endif /* _NET_PORT_LEDS_H */
