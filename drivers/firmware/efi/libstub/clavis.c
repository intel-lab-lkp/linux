// SPDX-License-Identifier: GPL-2.0
#include <linux/efi.h>
#include <asm/efi.h>
#include "efistub.h"

#define MAX_PARAM_LENGTH 64
static const efi_char16_t clavis_param_name[] = L"Clavis";
static const efi_guid_t clavis_guid = LINUX_EFI_CLAVIS_GUID;
static unsigned char param_data[MAX_PARAM_LENGTH];
static size_t param_len;

void efi_parse_clavis(char *option)
{
	if (!option)
		return;

	param_len = strnlen(option, MAX_PARAM_LENGTH);
	memcpy(param_data, option, param_len);
}

void efi_setup_clavis(void)
{
	efi_status_t error;

	if (param_len) {
		error = set_efi_var(clavis_param_name, &clavis_guid,
				    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
				    param_len, &param_data);
	}

	if (error)
		efi_err("Failed to set Clavis\n");
}
