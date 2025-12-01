/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_LIBELF_H
#define __PERF_LIBELF_H

struct build_id;

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

/*
 * Align offset to 4 bytes as needed for note name and descriptor data.
 */
#define NOTE_ALIGN(n) (((n) + 3) & -4U)

Elf_Scn *elf_section_by_name(Elf *elf, GElf_Ehdr *ep, GElf_Shdr *shp, const char *name,
			     size_t *idx);

int __libelf__read_build_id(Elf *elf, void *bf, size_t size);

int libelf__read_build_id(int _fd, const char *filename, struct build_id *bid);

#else // !defined(HAVE_LIBELF_SUPPORT)

static inline int libelf__read_build_id(int fd __always_unused,
					const char *filename __always_unused,
					struct build_id *bid __always_unused)
{
	return -1;
}

#endif // defined(HAVE_LIBELF_SUPPORT)

#endif /* __PERF_LIBELF_H */
