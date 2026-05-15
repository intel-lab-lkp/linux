/* SPDX-License-Identifier: GPL-2.0 */
/*
 * EFI Definitions for Secure Launch Resource Table
 *
 * See TrenchBoot Secure Launch kernel documentation for details.
 *
 * Copyright (c) 2026 Apertus Solutions, LLC
 * Copyright (c) 2026, Oracle and/or its affiliates.
 */

#ifndef _LINUX_SLR_EFI_H
#define _LINUX_SLR_EFI_H

#include <linux/slr_table.h>

#ifndef __ASSEMBLER__

/* EFI Support */

/* SLR table GUID for registering as an EFI Configuration Table (put this in efi.h if it becomes a standard) */
#define SLR_TABLE_GUID			EFI_GUID(0x877a9b2a, 0x0385, 0x45d1, 0xa0, 0x34, 0x9d, 0xac, 0x9c, 0x9e, 0x56, 0x5f)

/* Secure Launch EFI runtime protocol */
#define EFI_SLAUNCH_PROTOCOL_GUID	EFI_GUID(0x534189e0, 0x6fde, 0x413d,  0xbe, 0x91, 0xcd, 0x4e, 0x8d, 0x67, 0x2f, 0xea)

struct efi_slaunch_protocol {
	efi_status_t
	(__efiapi *setup_dlme)(struct efi_slaunch_protocol *this,
			       u64 dlme_base,
			       u64 dlme_header_offset,
			       u64 dlme_table);

	efi_status_t
	(__efiapi *launch)(struct efi_slaunch_protocol *this);
};
typedef struct efi_slaunch_protocol efi_slaunch_protocol_t;

#endif /* !__ASSEMBLER__ */

#endif /* _LINUX_SLR_EFI_H */
