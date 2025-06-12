/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MM blob header; this should match mm_payload.h
 */

#include <linux/linkage.h>
#include <asm/page_types.h>
#include <asm/segment.h>

	.section ".header", "a"

	.balign	16
SYM_DATA_START(mm_header)
	.long	pa_text_start
	.long	pa_mm_entry_32
	.long	pa_mm_entry_64
	.long	pa_mm_signature
	.long	pa_mm_blob_size
SYM_DATA_END(mm_header)
