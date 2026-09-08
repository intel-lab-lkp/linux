// SPDX-License-Identifier: GPL-2.0
/*
 * Obtain symbols from vmlinux for usage by kallsyms. Replaces mksysmap.
 *
 * To retain compatibility, it provides the same output as nm, only faster.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xalloc.h>

#include "elf-parse.h"
#include "kallsyms.h"

/* The mapped file and its symbol table. */
struct elf_file {
	void *base;
	size_t size;
	const char *shdrs;
	unsigned int shnum, shentsize;
	const char *shstrtab;
	Elf_Shdr *symtab;
	const char *strtab;
	size_t nr_syms;
};

/* What mksysmap dropped from System.map, by name. */
static const char *const sysmap_omit_prefixes[] = {
	"$", ".L", "__efistub_", "__pi_$", "__pi_.L", "__kvm_nvhe_$",
	"__kvm_nvhe_.L", "__kcfi_typeid_", "__kvm_nvhe___kcfi_typeid_",
	"__pi___kcfi_typeid_", "__crc_", "__kstrtab_", "__kstrtabns_",
	"__mod_device_table__",
};
static const char *const sysmap_omit_suffixes[] = {
	"_from_arm", "_from_thumb", "_veneer",
};
static const char *const sysmap_omit_names[] = {
	"L0", "_SDA_BASE_", "_SDA2_BASE_",
};

/* __<alnum>*Thunk_: the linker's range extension thunks on arm. */
static bool is_range_thunk(const char *name)
{
	const char *p;

	if (!string_starts_with(name, "__"))
		return false;
	for (p = name + 2; isalnum((unsigned char)*p); p++)
		;
	return p - name >= 7 && *p == '_' && strncmp(p - 5, "Thunk", 5) == 0;
}

/* __UNIQUE_ID_modinfo_<n>: the MODULE_INFO() strings of built-in code. */
static bool is_modinfo_id(const char *name)
{
	static const char prefix[] = "__UNIQUE_ID_modinfo_";
	const char *p;

	if (!string_starts_with(name, prefix))
		return false;
	for (p = name + strlen(prefix); isdigit((unsigned char)*p); p++)
		;
	return !*p;
}

static bool sysmap_omits(const char *name, char type)
{
	size_t i;

	/* Absolute, undefined and debugging symbols. */
	if (type == 'a' || type == 'N' || type == 'U' || type == 'w')
		return true;

	for (i = 0; i < ARRAY_SIZE(sysmap_omit_prefixes); i++)
		if (string_starts_with(name, sysmap_omit_prefixes[i]))
			return true;
	for (i = 0; i < ARRAY_SIZE(sysmap_omit_suffixes); i++)
		if (string_ends_with(name, sysmap_omit_suffixes[i]))
			return true;
	for (i = 0; i < ARRAY_SIZE(sysmap_omit_names); i++)
		if (strcmp(name, sysmap_omit_names[i]) == 0)
			return true;

	return is_range_thunk(name) || is_modinfo_id(name) ||
	       strstr(name, ".long_branch.") || strstr(name, ".plt_branch.");
}

static Elf_Shdr *elf_section(const struct elf_file *elf, unsigned int index)
{
	return (Elf_Shdr *)(elf->shdrs + (size_t)index * elf->shentsize);
}

static const char *elf_section_name(const struct elf_file *elf, Elf_Shdr *shdr)
{
	return elf->shstrtab + shdr_name(shdr);
}

static Elf_Sym *elf_symbol(const struct elf_file *elf, size_t index)
{
	const char *base = elf->base;

	return (Elf_Sym *)(base + shdr_offset(elf->symtab) +
			   index * shdr_entsize(elf->symtab));
}

/* nm's letter for a symbol defined in a section, as BFD classifies it. */
static char section_symbol_type(Elf_Shdr *shdr, const char *secname)
{
	static const char *const debug_prefixes[] = {
		".debug", ".zdebug", ".gnu.debuglto_.debug_",
		".gnu.linkonce.wi.", ".line", ".stab",
	};
	const uint64_t flags = shdr_flags(shdr);
	size_t i;

	if (flags & SHF_EXECINSTR)
		return 't';
	if (flags & SHF_ALLOC) {
		if (shdr_type(shdr) == SHT_NOBITS)
			return 'b';
		return flags & SHF_WRITE ? 'd' : 'r';
	}
	for (i = 0; i < ARRAY_SIZE(debug_prefixes); i++)
		if (string_starts_with(secname, debug_prefixes[i]))
			return 'N';
	if (shdr_type(shdr) != SHT_NOBITS && !(flags & SHF_WRITE))
		return 'n';
	return '?';
}

/* The letter nm prints for a symbol, or 0 for one it leaves out. */
static char elf_symbol_type(const struct elf_file *elf, Elf_Sym *sym)
{
	unsigned int bind = sym_bind(sym), type = sym_type(sym);
	unsigned int shndx = sym_shndx(sym);
	Elf_Shdr *shdr;
	char c;

	if (type == STT_SECTION || type == STT_FILE)
		return 0;
	if (shndx == SHN_COMMON)
		return 'C';
	if (shndx == SHN_UNDEF) {
		if (bind == STB_WEAK)
			return type == STT_OBJECT ? 'v' : 'w';
		return 'U';
	}
	if (type == STT_GNU_IFUNC)
		return 'i';
	if (bind == STB_WEAK)
		return type == STT_OBJECT ? 'V' : 'W';
	if (bind == STB_GNU_UNIQUE)
		return 'u';
	if (bind != STB_GLOBAL && bind != STB_LOCAL)
		return '?';

	if (shndx == SHN_ABS) {
		c = 'a';
	} else if (shndx < elf->shnum) {
		shdr = elf_section(elf, shndx);
		c = section_symbol_type(shdr, elf_section_name(elf, shdr));
	} else {
		return '?';
	}

	return bind == STB_GLOBAL ? toupper(c) : c;
}

/* nm -n order: by address, then by name. */
static int compare_symbols(const void *a, const void *b)
{
	const struct sysmap_symbol *sa = a, *sb = b;

	if (sa->addr != sb->addr)
		return sa->addr < sb->addr ? -1 : 1;
	return strcmp(sa->name, sb->name);
}

static void elf_open(struct elf_file *elf, const char *path)
{
	Elf_Ehdr *ehdr;
	unsigned int i;

	elf->base = elf_map_ro(path, &elf->size, (1 << ET_EXEC) | (1 << ET_DYN));
	if (!elf->base)
		exit(EXIT_FAILURE);

	ehdr = elf->base;
	elf->shdrs = (const char *)elf->base + ehdr_shoff(ehdr);
	elf->shnum = ehdr_shnum(ehdr);
	elf->shentsize = ehdr_shentsize(ehdr);
	elf->shstrtab = (const char *)elf->base +
			shdr_offset(elf_section(elf, ehdr_shstrndx(ehdr)));

	for (i = 0; i < elf->shnum && !elf->symtab; i++)
		if (shdr_type(elf_section(elf, i)) == SHT_SYMTAB)
			elf->symtab = elf_section(elf, i);

	if (!elf->symtab) {
		fprintf(stderr, "%s: no symbol table\n", path);
		exit(EXIT_FAILURE);
	}

	elf->strtab = (const char *)elf->base +
		      shdr_offset(elf_section(elf, shdr_link(elf->symtab)));
	elf->nr_syms = shdr_size(elf->symtab) / shdr_entsize(elf->symtab);
}

/* The symbols "nm -n | mksysmap" would list, in that order. */
static struct sysmap_symbol *elf_read_symbols(const struct elf_file *elf,
					   size_t *nr_kept)
{
	struct sysmap_symbol *syms = xmalloc(elf->nr_syms * sizeof(*syms));
	size_t i, n = 0;

	for (i = 1; i < elf->nr_syms; i++) {
		Elf_Sym *sym = elf_symbol(elf, i);
		const char *name = elf->strtab + sym_name(sym);
		char type = elf_symbol_type(elf, sym);

		if (!type || sysmap_omits(name, type))
			continue;

		syms[n].addr = sym_value(sym);
		syms[n].name = name;
		syms[n].type = type;
		n++;
	}

	qsort(syms, n, sizeof(*syms), compare_symbols);
	*nr_kept = n;
	return syms;
}

struct sysmap *sysmap_read(const char *path)
{
	struct sysmap *map = xcalloc(1, sizeof(*map));
	struct elf_file elf = {};

	elf_open(&elf, path);
	map->syms = elf_read_symbols(&elf, &map->nr_syms);
	map->addr_width = elf_map_long_size(elf.base) * 2;
	/* The names point into the mapping; keep it until the map is freed. */
	map->file = elf.base;
	map->file_size = elf.size;

	return map;
}

void sysmap_write(const struct sysmap *map, FILE *out)
{
	size_t i;

	for (i = 0; i < map->nr_syms; i++) {
		const struct sysmap_symbol *s = &map->syms[i];

		fprintf(out, "%0*llx %c %s\n", map->addr_width, s->addr, s->type,
			s->name);
	}
}

void sysmap_free(struct sysmap *map)
{
	free(map->syms);
	elf_unmap(map->file, map->file_size);
	free(map);
}
