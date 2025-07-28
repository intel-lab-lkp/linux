/*
 * Common module linker script, always used when linking a module.
 * Archs are free to supply their own linker scripts.  ld will
 * combine them automatically.
 */
#ifdef CONFIG_UNWIND_TABLES
#define DISCARD_EH_FRAME
#else
#define DISCARD_EH_FRAME	*(.eh_frame)
#endif

#include <asm-generic/codetag.lds.h>

SECTIONS {
	/DISCARD/ : {
		*(.discard)
		*(.discard.*)
		*(.export_symbol)
		*(.no_trim_symbol)
	}

	__ksymtab		0 : ALIGN(8) { *(SORT(___ksymtab+*)) }
	__ksymtab_gpl		0 : ALIGN(8) { *(SORT(___ksymtab_gpl+*)) }
	__kcrctab		0 : ALIGN(4) { *(SORT(___kcrctab+*)) }
	__kcrctab_gpl		0 : ALIGN(4) { *(SORT(___kcrctab_gpl+*)) }

	.ctors			0 : ALIGN(8) { *(SORT(.ctors.*)) *(.ctors) }
	.init_array		0 : ALIGN(8) { *(SORT(.init_array.*)) *(.init_array) }

	.altinstructions	0 : ALIGN(8) { KEEP(*(.altinstructions)) }
	__bug_table		0 : ALIGN(8) { KEEP(*(__bug_table)) }
	__jump_table		0 : ALIGN(8) { KEEP(*(__jump_table)) }
	__ex_table		0 : ALIGN(4) { KEEP(*(__ex_table)) }

	__patchable_function_entries : { *(__patchable_function_entries) }

#ifdef CONFIG_ARCH_USES_CFI_TRAPS
	__kcfi_traps 		: { KEEP(*(.kcfi_traps)) }
#endif

#ifdef CONFIG_LTO_CLANG
	/*
	 * With CONFIG_LTO_CLANG, LLD always enables -fdata-sections and
	 * -ffunction-sections, which increases the size of the final module.
	 * Merge the split sections in the final binary.
	 */
	.bss : {
		*(.bss .bss.[0-9a-zA-Z_]*)
		*(.bss..L*)
		*(__sancov_guards)
	}

	.data : {
		*(.data .data.[0-9a-zA-Z_]*)
		*(.data..L*)
		MOD_CODETAG_SECTIONS()
	}

	.rodata : {
		*(.rodata .rodata.[0-9a-zA-Z_]*)
		*(.rodata..L*)
	}
#else
	.data : {
		MOD_CODETAG_SECTIONS()
	}
#endif

#ifdef CONFIG_KCOV_UNIQUE
	/*
	 * CONFIG_KCOV_UNIQUE creates COMDAT groups for instrumented functions,
	 * which has the following consequences in the presence of
	 * -ffunction-sections:
	 *  - Separate .init.text and .exit.text sections in the modules are not
	 *    merged together, which results in errors trying to create
	 *    duplicate entries in /sys/module/MODNAME/sections/ at module load
	 *    time.
	 *  - Each function is placed in a separate .text.funcname section, so
	 *    there is no .text section anymore. Collecting them together here
	 *    has mostly aesthetic purpose, although some tools may be expecting
	 *    it to be present.
	 */
	.text : {
		*(.text .text.[0-9a-zA-Z_]*)
		*(.text..L*)
	}
	.init.text : {
		*(.init.text .init.text.[0-9a-zA-Z_]*)
		*(.init.text..L*)
	}
	.exit.text : {
		*(.exit.text .exit.text.[0-9a-zA-Z_]*)
		*(.exit.text..L*)
	}
	.bss : {
		*(.bss .bss.[0-9a-zA-Z_]*)
		*(.bss..L*)
		*(__sancov_guards)
	}
#endif

	MOD_SEPARATE_CODETAG_SECTIONS()
}

/* bring in arch-specific sections */
#include <asm/module.lds.h>
