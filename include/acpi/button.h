/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ACPI_BUTTON_H
#define ACPI_BUTTON_H

#define ACPI_BUTTON_HID_POWER	"PNP0C0C"
#define ACPI_BUTTON_HID_LID	"PNP0C0D"
#define ACPI_BUTTON_HID_SLEEP	"PNP0C0E"

#if IS_ENABLED(CONFIG_ACPI_BUTTON)
extern int acpi_lid_open(void);
#else
static inline int acpi_lid_open(void)
{
	return 1;
}
#endif /* IS_ENABLED(CONFIG_ACPI_BUTTON) */

/*
 * Set in acpi_suspend_enter() when the fixed power-button status bit is
 * observed (IRQ-disabled context). Consumed by the button driver on resume
 * so KEY_WAKEUP can be reported without calling into the button driver from
 * that early path. Implemented in sleep.c (built-in); safe when
 * CONFIG_ACPI_BUTTON=m.
 */
#ifdef CONFIG_ACPI_SYSTEM_POWER_STATES_SUPPORT
bool acpi_sleep_pwr_btn_wakeup_pending(void);
#else
static inline bool acpi_sleep_pwr_btn_wakeup_pending(void)
{
	return false;
}
#endif

#endif /* ACPI_BUTTON_H */
