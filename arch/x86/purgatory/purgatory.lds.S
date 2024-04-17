/* SPDX-License-Identifier: GPL-2.0 */

#include <asm-generic/vmlinux.lds.h>
#include <asm/cache.h>

OUTPUT_FORMAT(CONFIG_OUTPUT_FORMAT)

#undef i386

#ifdef CONFIG_X86_64
OUTPUT_ARCH(i386:x86-64)
#else
OUTPUT_ARCH(i386)
#endif

ENTRY(purgatory_start)

SECTIONS
{
	. = 0;

	.kexec-purgatory : {
		*(.kexec-purgatory)
	}

	.text : {
		_text = .;
		*(.text .text.*)
		*(.ltext .ltext.*)
		_etext = .;
	}

	.rodata : {
		_rodata = .;
		*(.rodata .rodata.*)
		*(.lrodata .lrodata.*)
		_erodata = .;
	}

	.data : {
		_data = .;
		*(.data .data.*)
		_edata = .;
	}

	. = ALIGN(L1_CACHE_BYTES);
	.bss : {
		_bss = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(8);	/* For convenience during zeroing */
		_ebss = .;
	}
	_end = .;

	ELF_DETAILS

	DISCARDS
	/DISCARD/ : {
		*(.note.GNU-stack .note.gnu.property)
		*(.llvm_addrsig)
	}
}
