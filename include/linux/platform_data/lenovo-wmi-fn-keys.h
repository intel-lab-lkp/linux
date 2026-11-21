/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Lenovo WMI Fn keys driver, notifier interface for drivers that want to act
 * on the vendor specific Fn key events before a key is reported.
 */

#ifndef _LENOVO_WMI_FN_KEYS_H_
#define _LENOVO_WMI_FN_KEYS_H_

struct device;
struct notifier_block;

/**
 * enum lwmi_fn_keys_event - notifier chain actions
 * @LWMI_FN_KEYS_EVENT_ESC: Fn+Esc from one of the Esc event GUIDs, which
 *                          carry no event data
 * @LWMI_FN_KEYS_EVENT_FN_KEY: a key from the Fn key event GUID, the notifier
 *                             data points to the u32 key code
 *
 * A callback that handles the event itself returns NOTIFY_STOP to suppress
 * the input report; NOTIFY_OK or NOTIFY_DONE let it through.
 */
enum lwmi_fn_keys_event {
	LWMI_FN_KEYS_EVENT_ESC,
	LWMI_FN_KEYS_EVENT_FN_KEY,
};

int lwmi_fn_keys_register_notifier(struct notifier_block *nb);
int lwmi_fn_keys_unregister_notifier(struct notifier_block *nb);
int devm_lwmi_fn_keys_register_notifier(struct device *dev, struct notifier_block *nb);

#endif /* _LENOVO_WMI_FN_KEYS_H_ */
