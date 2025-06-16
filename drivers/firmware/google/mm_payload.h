/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mm_payload.h
 *
 * Internal header for MM payload driver.
 *
 * Copyright 2025 9elements gmbh
 * Copyright 2025 Michal Gorlas <michal.gorlas@9elements.com>
 */

#ifndef __MM_PAYLOAD_H
#define __MM_PAYLOAD_H

#define PAYLOAD_MM_RET_SUCCESS 0
#define PAYLOAD_MM_RET_FAILURE 1
#define PAYLOAD_MM_REGISTER_ENTRY 2

#define REALMODE_END_SIGNATURE	0x65a22c82

struct mm_info {
	u8 revision;
	u8 requires_long_mode_call;
	u8 register_mm_entry_command;
};

extern struct mm_info *mm_info;

#ifndef __ASSEMBLY__

#include <linux/types.h>

/* This must match data at mm_handler/mm_header.S */
struct mm_header {
	u32	text_start;
	u32	mm_entry_32;
	u32	mm_entry_64;
	u32	mm_signature;
	u32	mm_blob_size;
};

extern struct mm_header *mm_header;
extern unsigned char mm_blob_end[];

extern unsigned char mm_blob[];
extern unsigned char mm_relocs[];

/*
 * This has to be under 1MB (see tseg_region.c in coreboot source tree).
 * The actual check for this is made in coreboot after we fill the header
 * (see above) with the blob size.
 */
static inline size_t mm_payload_size_needed(void)
{
	return mm_blob_end - mm_blob;
}

#endif /* __ASSEMBLER__ */
#endif /* __MM_PAYLOAD_H */
