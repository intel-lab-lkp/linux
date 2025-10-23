/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __USB_TYPEC_NOTIFY
#define __USB_TYPEC_NOTIFY

#include <linux/notifier.h>

enum usb_typec_event {
	TYPEC_ALTMODE_REGISTERED
};

int typec_register_notify(struct notifier_block *nb);
int typec_unregister_notify(struct notifier_block *nb);

void typec_notify_event(unsigned long event, void *data);

#endif /* __USB_TYPEC_NOTIFY */
