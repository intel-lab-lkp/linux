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
 * sleep.c is built-in and calls this helper. Use IS_REACHABLE so that when
 * CONFIG_ACPI_BUTTON=m the call site gets an inline stub instead of an
 * unresolved vmlinux link reference (0day/LKP ERROR).
 */
#if IS_REACHABLE(CONFIG_ACPI_BUTTON)
extern void acpi_button_power_wakeup_pending(void);
#else
static inline void acpi_button_power_wakeup_pending(void)
{
}
#endif /* IS_REACHABLE(CONFIG_ACPI_BUTTON) */

#endif /* ACPI_BUTTON_H */
