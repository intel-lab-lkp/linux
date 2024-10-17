// SPDX-License-Identifier: GPL-2.0
#include <keys/asymmetric-type.h>
#include <linux/efi.h>
#include "clavis.h"

static efi_char16_t clavis_param_name[] = L"Clavis";
static efi_guid_t clavis_guid = LINUX_EFI_CLAVIS_GUID;

int __init clavis_efi_param(struct asymmetric_key_id *kid, int len)
{
	unsigned char buf[64];
	unsigned long ascii_len = sizeof(buf);
	efi_status_t error;
	int hex_len;
	u32 attr;

	if (!efi_enabled(EFI_BOOT)) {
		pr_debug("efi_enabled(EFI_BOOT) not set");
		return -EPERM;
	}

	if (!efi_enabled(EFI_RUNTIME_SERVICES)) {
		pr_debug("%s : EFI runtime services are not enabled\n", __func__);
		return -EPERM;
	}

	error = efi.get_variable(clavis_param_name, &clavis_guid, &attr, &ascii_len, &buf);

	if (error) {
		pr_debug("Error reading clavis parm or not found\n");
		return -EINVAL;
	}

	if (attr & EFI_VARIABLE_NON_VOLATILE)  {
		pr_debug("Error: NV access set\n");
		return -EINVAL;
	} else if (ascii_len > 0) {
		hex_len = ascii_len / 2;

		if (hex_len > len) {
			pr_debug("invalid length\n");
			return -EINVAL;
		}
		kid->len = hex_len;
		return hex2bin(kid->data, buf, kid->len);
	}

	pr_debug("Error: invalid size\n");
	return -EINVAL;
}
