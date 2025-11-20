// SPDX-License-Identifier: GPL-2.0+
#include <linux/of.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>

#include <drm/bridge/aux-bridge.h>

static int drm_typec_bus_event(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	struct typec_altmode *alt = (struct typec_altmode *)data;

	if (action != TYPEC_ALTMODE_REGISTERED)
		goto done;

	if (is_typec_partner(&alt->dev) || alt->svid != USB_TYPEC_DP_SID)
		goto done;

	/*
	 * alt->dev.parent->parent : USB-C controller device
	 * alt->dev.parent         : USB-C connector device
	 */
	drm_dp_hpd_bridge_register(alt->dev.parent->parent,
				   to_of_node(alt->dev.parent->fwnode));

done:
	return NOTIFY_OK;
}

static struct notifier_block drm_typec_event_nb = {
	.notifier_call = drm_typec_bus_event,
};

static void drm_aux_hpd_typec_dp_bridge_module_exit(void)
{
	typec_altmode_unregister_notify(&drm_typec_event_nb);
}

static int __init drm_aux_hpd_typec_dp_bridge_module_init(void)
{
	typec_altmode_register_notify(&drm_typec_event_nb);

	return 0;
}

module_init(drm_aux_hpd_typec_dp_bridge_module_init);
module_exit(drm_aux_hpd_typec_dp_bridge_module_exit);

MODULE_DESCRIPTION("DRM TYPEC DP HPD BRIDGE");
MODULE_LICENSE("GPL");
