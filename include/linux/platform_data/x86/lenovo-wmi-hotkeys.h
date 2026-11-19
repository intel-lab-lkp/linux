/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Lenovo WMI hotkeys driver, notifier interface for drivers that want to act
 * on the vendor specific Fn key events before a key is reported.
 */

#ifndef _LENOVO_WMI_HOTKEYS_H_
#define _LENOVO_WMI_HOTKEYS_H_

struct device;
struct notifier_block;

/* Code reported for the Fn+Esc event GUIDs, as ideapad-laptop always did */
#define LENOVO_WMI_HOTKEYS_ESC_CODE	128

/*
 * Notifier callbacks receive the WMI event code as the action and no data:
 * the Fn key code from the Fn key event GUID, or LENOVO_WMI_HOTKEYS_ESC_CODE
 * for the Fn+Esc event GUIDs, which carry none. A callback that handles the
 * event itself returns NOTIFY_STOP to suppress the input report; NOTIFY_OK or
 * NOTIFY_DONE let it through.
 */
int lenovo_wmi_hotkeys_register_notifier(struct notifier_block *nb);
int lenovo_wmi_hotkeys_unregister_notifier(struct notifier_block *nb);
int devm_lenovo_wmi_hotkeys_register_notifier(struct device *dev,
					      struct notifier_block *nb);

#endif /* _LENOVO_WMI_HOTKEYS_H_ */
