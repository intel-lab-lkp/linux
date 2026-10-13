/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ACPI_BUTTON_H
#define ACPI_BUTTON_H

#define ACPI_BUTTON_HID_POWER	"PNP0C0C"
#define ACPI_BUTTON_HID_LID	"PNP0C0D"
#define ACPI_BUTTON_HID_SLEEP	"PNP0C0E"

#define ACPI_BUTTON_NOTIFY_STATUS	0x80
#define ACPI_BUTTON_NOTIFY_PRESS	0x81
#define ACPI_BUTTON_NOTIFY_RELEASE	0x82

#if IS_ENABLED(CONFIG_ACPI_BUTTON)
extern int acpi_lid_open(void);
#else
static inline int acpi_lid_open(void)
{
	return 1;
}
#endif /* IS_ENABLED(CONFIG_ACPI_BUTTON) */

#endif /* ACPI_BUTTON_H */
