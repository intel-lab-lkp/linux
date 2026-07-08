// SPDX-License-Identifier: GPL-2.0
/*
 * tlob_sym.c - ELF symbol-to-file-offset utility for tlob selftests
 *
 * Usage: tlob_sym sym_offset <binary> <symbol>
 *
 *   Prints the ELF file offset of <symbol> in <binary> to stdout.
 *
 * Exit: 0 = found, 1 = error / not found.
 */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int sym_offset(const char *binary, const char *symname)
{
	int fd;
	struct stat st;
	void *map;
	Elf64_Ehdr *ehdr;
	Elf32_Ehdr *ehdr32;
	int is64;
	uint64_t sym_vaddr = 0;
	int found = 0;
	uint64_t file_offset = 0;

	fd = open(binary, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", binary, strerror(errno));
		return 1;
	}
	if (fstat(fd, &st) < 0) {
		close(fd);
		return 1;
	}
	map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}

	ehdr = (Elf64_Ehdr *)map;
	ehdr32 = (Elf32_Ehdr *)map;
	if (st.st_size < 4 ||
	    ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3) {
		fprintf(stderr, "%s: not an ELF file\n", binary);
		munmap(map, (size_t)st.st_size);
		return 1;
	}
	is64 = (ehdr->e_ident[EI_CLASS] == ELFCLASS64);

	if (is64) {
		Elf64_Shdr *shdrs;
		Elf64_Shdr *shstrtab_hdr;

		if (ehdr->e_shnum == 0 || ehdr->e_shstrndx >= ehdr->e_shnum ||
		    (uint64_t)ehdr->e_shoff +
		    (uint64_t)ehdr->e_shnum * sizeof(Elf64_Shdr) > (uint64_t)st.st_size) {
			fprintf(stderr, "%s: malformed ELF section table\n", binary);
			munmap(map, (size_t)st.st_size);
			return 1;
		}
		shdrs = (Elf64_Shdr *)((char *)map + ehdr->e_shoff);
		shstrtab_hdr = &shdrs[ehdr->e_shstrndx];
		const char *shstrtab = (char *)map + shstrtab_hdr->sh_offset;
		int si;

		for (int pass = 0; pass < 2 && !found; pass++) {
			const char *target = pass ? ".dynsym" : ".symtab";

			for (si = 0; si < ehdr->e_shnum && !found; si++) {
				Elf64_Shdr *sh = &shdrs[si];
				const char *name = shstrtab + sh->sh_name;

				if (strcmp(name, target) != 0)
					continue;

				Elf64_Shdr *strtab_sh = &shdrs[sh->sh_link];
				const char *strtab = (char *)map + strtab_sh->sh_offset;
				Elf64_Sym *syms = (Elf64_Sym *)((char *)map + sh->sh_offset);
				uint64_t nsyms = sh->sh_size / sizeof(Elf64_Sym);
				uint64_t j;

				for (j = 0; j < nsyms; j++) {
					if (strcmp(strtab + syms[j].st_name, symname) == 0) {
						sym_vaddr = syms[j].st_value;
						found = 1;
						break;
					}
				}
			}
		}

		if (!found) {
			fprintf(stderr, "symbol '%s' not found in %s\n", symname, binary);
			munmap(map, (size_t)st.st_size);
			return 1;
		}

		Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)map + ehdr->e_phoff);
		int pi;

		for (pi = 0; pi < ehdr->e_phnum; pi++) {
			Elf64_Phdr *ph = &phdrs[pi];

			if (ph->p_type != PT_LOAD)
				continue;
			if (sym_vaddr >= ph->p_vaddr &&
			    sym_vaddr < ph->p_vaddr + ph->p_filesz) {
				file_offset = sym_vaddr - ph->p_vaddr + ph->p_offset;
				break;
			}
		}
	} else {
		Elf32_Shdr *shdrs;
		Elf32_Shdr *shstrtab_hdr;

		if (ehdr32->e_shnum == 0 || ehdr32->e_shstrndx >= ehdr32->e_shnum ||
		    (uint64_t)ehdr32->e_shoff +
		    (uint64_t)ehdr32->e_shnum * sizeof(Elf32_Shdr) > (uint64_t)st.st_size) {
			fprintf(stderr, "%s: malformed ELF section table\n", binary);
			munmap(map, (size_t)st.st_size);
			return 1;
		}
		shdrs = (Elf32_Shdr *)((char *)map + ehdr32->e_shoff);
		shstrtab_hdr = &shdrs[ehdr32->e_shstrndx];
		const char *shstrtab = (char *)map + shstrtab_hdr->sh_offset;
		int si;
		uint32_t sym_vaddr32 = 0;

		for (int pass = 0; pass < 2 && !found; pass++) {
			const char *target = pass ? ".dynsym" : ".symtab";

			for (si = 0; si < ehdr32->e_shnum && !found; si++) {
				Elf32_Shdr *sh = &shdrs[si];
				const char *name = shstrtab + sh->sh_name;

				if (strcmp(name, target) != 0)
					continue;

				Elf32_Shdr *strtab_sh = &shdrs[sh->sh_link];
				const char *strtab = (char *)map + strtab_sh->sh_offset;
				Elf32_Sym *syms = (Elf32_Sym *)((char *)map + sh->sh_offset);
				uint32_t nsyms = sh->sh_size / sizeof(Elf32_Sym);
				uint32_t j;

				for (j = 0; j < nsyms; j++) {
					if (strcmp(strtab + syms[j].st_name, symname) == 0) {
						sym_vaddr32 = syms[j].st_value;
						found = 1;
						break;
					}
				}
			}
		}

		if (!found) {
			fprintf(stderr, "symbol '%s' not found in %s\n", symname, binary);
			munmap(map, (size_t)st.st_size);
			return 1;
		}

		Elf32_Phdr *phdrs = (Elf32_Phdr *)((char *)map + ehdr32->e_phoff);
		int pi;

		for (pi = 0; pi < ehdr32->e_phnum; pi++) {
			Elf32_Phdr *ph = &phdrs[pi];

			if (ph->p_type != PT_LOAD)
				continue;
			if (sym_vaddr32 >= ph->p_vaddr &&
			    sym_vaddr32 < ph->p_vaddr + ph->p_filesz) {
				file_offset = sym_vaddr32 - ph->p_vaddr + ph->p_offset;
				break;
			}
		}
		sym_vaddr = sym_vaddr32;
	}

	munmap(map, (size_t)st.st_size);

	if (!file_offset && sym_vaddr) {
		fprintf(stderr, "could not map vaddr 0x%lx to file offset\n",
			(unsigned long)sym_vaddr);
		return 1;
	}

	printf("0x%lx\n", (unsigned long)file_offset);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc != 4 || strcmp(argv[1], "sym_offset") != 0) {
		fprintf(stderr, "Usage: %s sym_offset <binary> <symbol>\n", argv[0]);
		return 1;
	}
	return sym_offset(argv[2], argv[3]);
}
