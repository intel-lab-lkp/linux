// SPDX-License-Identifier: GPL-2.0-only

#include <linux/efi.h>
#include <linux/pci.h>
#include <linux/stddef.h>
#include <linux/slr_efi.h>
#include <linux/slaunch.h>

#include <asm/boot.h>
#include <asm/bootparam.h>
#include <asm/efi.h>

#include "efistub.h"

static struct efi_slaunch_protocol *slaunch;

efi_status_t efi_secure_launch_init(efi_handle_t image_handle)
{
	return efi_bs_call(handle_protocol, image_handle,
			   &EFI_SLAUNCH_PROTOCOL_GUID, (void **)&slaunch);
}

efi_status_t efi_secure_launch_prepare(struct boot_params *boot_params,
				       phys_addr_t base)
{
	if (!slaunch)
		return EFI_SUCCESS;

	return slaunch->setup_dlme(slaunch, base, mle_header_offset, (u64)boot_params);
}

void efi_secure_launch(void)
{
	if (!slaunch)
		return;

	slaunch->launch(slaunch);
}
