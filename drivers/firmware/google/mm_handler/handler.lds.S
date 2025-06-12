/* SPDX-License-Identifier: GPL-2.0 */
/*
 * blob.lds.S
 *
 * Linker script for the MM handler. Based on realmode.lds.S
 */

#include <asm/page_types.h>

#undef i386

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(pa_text_start)

SECTIONS
{
	. = 0;
	.header : {
		*(.header)
	}

	pa_text_start = .;
	.text32 : {
		*(.text32)
		*(.text32.*)
	}

	.text64 : {
		*(.text64)
		*(.text64.*)
	}

	. = ALIGN(128);
	.bss : {
		*(.bss*)
	}

	/DISCARD/ : {
		*(.data*)
		*(.note*)
		*(.debug*)
	}

#include "pasyms.h"
}
