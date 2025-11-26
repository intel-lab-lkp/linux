// SPDX-License-Identifier: GPL-2.0

#include <linux/efi.h>
#include <linux/sysfb.h>

#include <asm/efi.h>

#include "efistub.h"

static efi_guid_t primary_display_guid = LINUX_EFI_PRIMARY_DISPLAY_TABLE_GUID;

struct sysfb_display_info *alloc_primary_display(void)
{
	struct sysfb_display_info *dpy;
	efi_status_t status;

	status = efi_bs_call(allocate_pool, EFI_ACPI_RECLAIM_MEMORY,
			     sizeof(*dpy), (void **)&dpy);

	if (status != EFI_SUCCESS)
		return NULL;

	memset(dpy, 0, sizeof(*dpy));

	status = efi_bs_call(install_configuration_table,
			     &primary_display_guid, dpy);
	if (status == EFI_SUCCESS)
		return dpy;

	efi_bs_call(free_pool, dpy);
	return NULL;
}

void free_primary_display(struct sysfb_display_info *dpy)
{
	if (!dpy)
		return;

	efi_bs_call(install_configuration_table, &primary_display_guid, NULL);
	efi_bs_call(free_pool, dpy);
}
