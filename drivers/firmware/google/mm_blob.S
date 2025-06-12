/* SPDX-License-Identifier: GPL-2.0 */
/* Derived from rmpiggy.S.
 *
 * Wrapper script for the MM payload binary as a transport object
 * before copying to SMRAM memory.
 */
#include <linux/linkage.h>
#include <asm/page_types.h>

	.section ".init.data","aw"

	.balign PAGE_SIZE

SYM_DATA_START(mm_blob)
	.incbin	"drivers/firmware/google/mm_handler/handler.bin"
SYM_DATA_END_LABEL(mm_blob, SYM_L_GLOBAL, mm_blob_end)

SYM_DATA_START(mm_relocs)
	.incbin	"drivers/firmware/google/mm_handler/handler.relocs"
SYM_DATA_END(mm_relocs)
