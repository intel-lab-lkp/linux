/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_IMA_EFI_H
#define _ASM_X86_IMA_EFI_H

#include <asm/bootparam.h>

#define arch_ima_efi_boot_mode	\
	({ extern struct boot_params boot_params; boot_params.secure_boot; })

#include <asm-generic/ima-efi.h>

#endif /* _ASM_X86_IMA_EFI_H */
