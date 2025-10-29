/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __USB_TYPEC_NOTIFY
#define __USB_TYPEC_NOTIFY

#include <linux/notifier.h>

enum usb_typec_event {
	TYPEC_ALTMODE_REGISTERED,
	TYPEC_ALTMODE_UNREGISTERED,
};

int typec_altmode_register_notify(struct notifier_block *nb);
int typec_altmode_unregister_notify(struct notifier_block *nb);

#endif /* __USB_TYPEC_NOTIFY */
