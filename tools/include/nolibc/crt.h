/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * C Run Time support for NOLIBC
 * Copyright (C) 2023 Zhangjin Wu <falcon@tinylab.org>
 */

#ifndef _NOLIBC_CRT_H
#define _NOLIBC_CRT_H

#ifndef NOLIBC_NO_RUNTIME

#include "compiler.h"
#include "elf.h"

char **environ __attribute__((weak));
const unsigned long *_auxv __attribute__((weak));

void _start(void);
static void __stack_chk_init(void);
static void exit(int);

extern void (*const __preinit_array_start[])(int, char **, char**) __attribute__((weak));
extern void (*const __preinit_array_end[])(int, char **, char**) __attribute__((weak));

extern void (*const __init_array_start[])(int, char **, char**) __attribute__((weak));
extern void (*const __init_array_end[])(int, char **, char**) __attribute__((weak));

extern void (*const __fini_array_start[])(void) __attribute__((weak));
extern void (*const __fini_array_end[])(void) __attribute__((weak));

void _start_c(long *sp);
__attribute__((weak,used))
#if __nolibc_has_feature(undefined_behavior_sanitizer)
	__attribute__((no_sanitize("function")))
#endif
void _start_c(long *sp)
{
	long argc;
	char **argv;
	char **envp;
	int exitcode;
	void (* const *ctor_func)(int, char **, char **);
	void (* const *dtor_func)(void);
	const unsigned long *auxv;
	/* silence potential warning: conflicting types for 'main' */
	int _nolibc_main(int, char **, char **) __asm__ ("main");

	/* initialize stack protector */
	__stack_chk_init();

#ifdef NOLIBC_STATIC_PIE
#define R_68K_RELATIVE	22
{
	void *base = (void *) 0x6d8000; // TODO: how to actually get this?
	unsigned int rela_count = 0;
	unsigned int rela_off = 0;
	unsigned long dyn_addr;
	Elf32_Rela *rela;
	Elf32_Addr *addr;
	Elf32_Dyn *dyn;
	int i;

	/* For m68k with the FDPIC loader d5 contains the offset to the DYNAMIC segment */
	__asm__ volatile (
		"move.l %%d5, %0\n"
		: "=r" (dyn_addr)
	);
	dyn = (Elf32_Dyn *) dyn_addr;

	/* Go through the DYNAMIC segment and get the offset to rela and the number of relocations */
	for (; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
		case DT_RELA:
			rela_off = dyn->d_un.d_ptr;
			break;
		case DT_RELACOUNT:
			rela_count = dyn->d_un.d_val;
			break;
		}
	}

	if (!rela_off || !rela_count)
		exit(42); //TODO nonsense error

	rela = base + rela_off;

	/* Do the relocations, only R_68K_RELATIVE for now */
	for (i = 0; i < rela_count; i++) {
		Elf32_Rela *entry = &rela[i];

		switch (ELF32_R_TYPE(entry->r_info)) {
		case R_68K_RELATIVE:
		{
			addr = (Elf32_Addr *)(base + entry->r_offset);
			*addr = (Elf32_Addr) (base + entry->r_addend);
		}
			break;
		default:
			exit(43); //TODO nonsense error
			break;
		}
	}
}
#endif

	/*
	 * sp  :    argc          <-- argument count, required by main()
	 * argv:    argv[0]       <-- argument vector, required by main()
	 *          argv[1]
	 *          ...
	 *          argv[argc-1]
	 *          null
	 * environ: environ[0]    <-- environment variables, required by main() and getenv()
	 *          environ[1]
	 *          ...
	 *          null
	 * _auxv:   _auxv[0]      <-- auxiliary vector, required by getauxval()
	 *          _auxv[1]
	 *          ...
	 *          null
	 */

	/* assign argc and argv */
	argc = *sp;
	argv = (void *)(sp + 1);

	/* find environ */
	environ = envp = argv + argc + 1;

	/* find _auxv */
	for (auxv = (void *)envp; *auxv++;)
		;
	_auxv = auxv;

	for (ctor_func = __preinit_array_start; ctor_func < __preinit_array_end; ctor_func++)
		(*ctor_func)(argc, argv, envp);
	for (ctor_func = __init_array_start; ctor_func < __init_array_end; ctor_func++)
		(*ctor_func)(argc, argv, envp);

	/* go to application */
	exitcode = _nolibc_main(argc, argv, envp);

	for (dtor_func = __fini_array_end; dtor_func > __fini_array_start;)
		(*--dtor_func)();

	exit(exitcode);
}

#endif /* NOLIBC_NO_RUNTIME */
#endif /* _NOLIBC_CRT_H */
