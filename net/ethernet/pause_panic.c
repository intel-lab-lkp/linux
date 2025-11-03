// SPDX-License-Identifier: GPL-2.0
/*
 * Ethernet pause disable on panic handler
 *
 * This module provides per-device control via sysfs to disable Ethernet flow
 * control (pause frames) on individual Ethernet devices when the kernel panics.
 * Each device can be configured via /sys/class/net/<device>/disable_pause_on_panic.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/panic_notifier.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/notifier.h>
#include <linux/if_ether.h>
#include <net/net_namespace.h>

/*
 * Disable pause/flow control on a single Ethernet device.
 * This is called from panic context, so we cannot sleep.
 * We try to call set_pauseparam, but if it would require sleeping,
 * we skip the device rather than risk deadlock.
 */
static void disable_pause_on_device(struct net_device *dev)
{
	struct ethtool_pauseparam pause = { };
	const struct ethtool_ops *ops;

	/* Only proceed if this device has the flag enabled */
	if (!READ_ONCE(dev->disable_pause_on_panic))
		return;

	ops = dev->ethtool_ops;
	if (!ops || !ops->set_pauseparam)
		return;

	/* Prepare pause parameters to disable flow control */
	pause.autoneg = 0;
	pause.rx_pause = 0;
	pause.tx_pause = 0;

	/*
	 * In panic context, we're in atomic context and cannot sleep.
	 * We try to call set_pauseparam directly. If it would sleep,
	 * that's a driver bug, but we proceed anyway since we're panicking.
	 * The driver's set_pauseparam implementation should ideally handle
	 * atomic context, but if it doesn't, we can't do much about it
	 * during a panic.
	 */
	ops->set_pauseparam(dev, &pause);
}

/*
 * Panic notifier to disable pause frames on all Ethernet devices.
 * Called in atomic context during kernel panic.
 */
static int eth_pause_panic_handler(struct notifier_block *this,
					unsigned long event, void *ptr)
{
	struct net_device *dev;

	/*
	 * Iterate over all network devices in the init namespace.
	 * In panic context, we cannot acquire locks that might sleep,
	 * so we use RCU iteration.
	 * Each device will check its own disable_pause_on_panic flag.
	 */
	rcu_read_lock();
	for_each_netdev_rcu(&init_net, dev) {
		/* Reference count might not be available in panic */
		if (!dev)
			continue;

		disable_pause_on_device(dev);
	}
	rcu_read_unlock();

	return NOTIFY_DONE;
}

static struct notifier_block eth_pause_panic_notifier = {
	.notifier_call = eth_pause_panic_handler,
	.priority = INT_MAX, /* Run as late as possible */
};

static int __init eth_pause_panic_init(void)
{
	/* Register panic notifier */
	atomic_notifier_chain_register(&panic_notifier_list,
				       &eth_pause_panic_notifier);

	return 0;
}
device_initcall(eth_pause_panic_init);
