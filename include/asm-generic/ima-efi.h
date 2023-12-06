/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __ASM_GENERIC_IMA_EFI_H_
#define __ASM_GENERIC_IMA_EFI_H_

#include <linux/efi.h>

/*
 * Only include this header file from your architecture's <asm/ima-efi.h>.
 */

#ifndef arch_ima_efi_boot_mode
#define arch_ima_efi_boot_mode efi_secureboot_mode_unset
#endif

#endif /* __ASM_GENERIC_FB_H_ */
