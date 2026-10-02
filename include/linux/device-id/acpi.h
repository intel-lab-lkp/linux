/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_DEVICE_ID_ACPI_H
#define LINUX_DEVICE_ID_ACPI_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef unsigned long kernel_ulong_t;
#endif

#define ACPI_ID_LEN	16

/**
 * struct acpi_device_id - ACPI device matching table entry
 * @id: NUL-terminated hardware ID (_HID) or compatible ID (_CID) to match.
 * @driver_data: Data private to the driver.
 * @cls: PCI-defined class, subclass and programming interface code (_CLS).
 * @cls_msk: Mask selecting the class-code fields to match.
 *
 * Use @id for ID matching or ACPI_DEVICE_CLASS() for class-code matching.
 * Terminate the table with an entry whose @id is empty and @cls is zero.
 */
struct acpi_device_id {
	__u8 id[ACPI_ID_LEN];
	kernel_ulong_t driver_data;
	__u32 cls;
	__u32 cls_msk;
};

/**
 * ACPI_DEVICE_CLASS - macro used to describe an ACPI device with
 * the PCI-defined class-code information
 *
 * @_cls : the class, subclass, prog-if triple for this device
 * @_msk : the class mask for this device
 *
 * This macro is used to create a struct acpi_device_id that matches a
 * specific PCI class. The .id and .driver_data fields will be left
 * initialized with the default value.
 */
#define ACPI_DEVICE_CLASS(_cls, _msk)	.cls = (_cls), .cls_msk = (_msk),

#endif /* ifndef LINUX_DEVICE_ID_ACPI_H */
