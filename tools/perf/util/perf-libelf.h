/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_LIBELF_H
#define __PERF_LIBELF_H

#ifdef HAVE_LIBELF_SUPPORT

#include <libelf.h>
#include <gelf.h>

/*
 * libelf 0.8.x and earlier do not support ELF_C_READ_MMAP;
 * for newer versions we can use mmap to reduce memory usage:
 */
#ifdef ELF_C_READ_MMAP
# define PERF_ELF_C_READ_MMAP ELF_C_READ_MMAP
#else
# define PERF_ELF_C_READ_MMAP ELF_C_READ
#endif

Elf_Scn *elf_section_by_name(Elf *elf, GElf_Ehdr *ep, GElf_Shdr *shp, const char *name,
			     size_t *idx);

#endif // defined(HAVE_LIBELF_SUPPORT)

#endif /* __PERF_LIBELF_H */
