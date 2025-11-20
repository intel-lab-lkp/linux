// SPDX-License-Identifier: GPL-2.0
#include <linux/notifier.h>

#include "bus.h"

static BLOCKING_NOTIFIER_HEAD(typec_notifier_list);

int typec_altmode_register_notify(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&typec_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(typec_altmode_register_notify);

int typec_altmode_unregister_notify(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&typec_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(typec_altmode_unregister_notify);

void typec_notify_event(unsigned long event, void *data)
{
	blocking_notifier_call_chain(&typec_notifier_list, event, data);
}
