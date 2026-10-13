/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_EFI_BGRT_H
#define _LINUX_EFI_BGRT_H

#include <linux/acpi.h>

#ifdef CONFIG_ACPI_BGRT

void efi_bgrt_init(struct acpi_table_header *table);
int __init acpi_parse_bgrt(struct acpi_table_header *table);

bool efi_bgrt_has_image(void);
phys_addr_t efi_bgrt_image_address(void);
size_t efi_bgrt_image_size(void);
u8 efi_bgrt_status(void);
u32 efi_bgrt_image_offset_x(void);
u32 efi_bgrt_image_offset_y(void);

/* The BGRT data itself; only valid if efi_bgrt_has_image(). */
extern size_t bgrt_image_size;
extern struct acpi_table_bgrt bgrt_tab;

#else /* !CONFIG_ACPI_BGRT */

static inline void efi_bgrt_init(struct acpi_table_header *table) {}
static inline int __init acpi_parse_bgrt(struct acpi_table_header *table)
{
	return 0;
}

static inline bool efi_bgrt_has_image(void)
{
	return false;
}

static inline phys_addr_t efi_bgrt_image_address(void)
{
	return 0;
}

static inline size_t efi_bgrt_image_size(void)
{
	return 0;
}

static inline u8 efi_bgrt_status(void)
{
	return 0;
}

static inline u32 efi_bgrt_image_offset_x(void)
{
	return 0;
}

static inline u32 efi_bgrt_image_offset_y(void)
{
	return 0;
}

#endif /* !CONFIG_ACPI_BGRT */

#endif /* _LINUX_EFI_BGRT_H */
