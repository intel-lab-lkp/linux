// SPDX-License-Identifier: GPL-2.0-only
/*
 * gen_lineinfo.c - Generate address-to-source-line lookup tables from DWARF
 *
 * Copyright (C) 2026 Sasha Levin <sashal@kernel.org>
 *
 * Reads DWARF .debug_line from a vmlinux ELF file and outputs an assembly
 * file containing sorted lookup tables that the kernel uses to annotate
 * stack traces with source file:line information.
 *
 * Requires libdw from elfutils.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <elfutils/libdw.h>
#include <dwarf.h>
#include <elf.h>
#include <gelf.h>
#include <limits.h>

#include "../include/linux/mod_lineinfo.h"

static int module_mode;

static unsigned int skipped_overflow;

/* Target ELF traits, captured once in main() and reused at emit time. */
static bool target_64bit;
static bool target_le;

/*
 * Vmlinux mode only: address range of the *invariant* .text region.
 * See find_text_end_addr() for why we cap on _etext.  text_end_addr == 0
 * means "no cap available; capture everything above text_addr" (v3
 * behavior, used as graceful fallback if _etext is absent).
 */
static unsigned long long text_end_addr;

/*
 * In module mode we cover several text-like sections, split across two
 * output blobs by lifecycle:
 *
 *   .mod_lineinfo      -- persistent code (.text, .exit.text); MOD_RODATA
 *   .init.mod_lineinfo -- init code (.init.text); freed with init memory
 *
 * In ET_REL .ko files .text/.init.text/.exit.text all have sh_addr == 0,
 * so DWARF line addresses (which become sh_addr + addend after relocation)
 * collide across sections.  We disambiguate by giving each *present*
 * covered section a unique synthetic "bias" — a u32 base address — and
 * adding that bias to relocated values inside apply_debug_line_relocations.
 * libdw then yields biased addresses that classify_address() can map back
 * to a single section unambiguously.  The bias is internal to gen_lineinfo
 * and never leaks into the emitted blob.
 */
enum mod_lineinfo_blob {
	BLOB_PERSISTENT,
	BLOB_INIT,
	NUM_BLOBS,
};

struct covered_section {
	const char *name;	/* ELF section name (e.g. ".text") */
	enum mod_lineinfo_blob blob;
	unsigned long long bias;/* synthetic base address (set in resolve_*) */
	unsigned long long size;
	bool present;		/* found in this .ko */
	unsigned int sec_index;	/* ELF section header index, for reloc matching */
	unsigned int n_entries;	/* DWARF line entries collected for this section */
};

static struct covered_section all_sections[] = {
	{ .name = ".text",         .blob = BLOB_PERSISTENT },
	{ .name = ".exit.text",    .blob = BLOB_PERSISTENT },
	{ .name = ".init.text",    .blob = BLOB_INIT },
	{ .name = ".noinstr.text", .blob = BLOB_PERSISTENT },
};
#define ALL_SECTIONS	(sizeof(all_sections) / sizeof(all_sections[0]))

/*
 * Executable sections present in the .ko but not covered by the blob
 * (e.g. .static_call.text, .altinstr_replacement, or the per-function
 * .text.* subsections parisc32 modules keep).  They get synthetic biases
 * from the same cursor as the covered sections so their DWARF sequences
 * classify unambiguously and can be dropped, instead of aliasing into
 * .text's [0, size) range.
 */
static struct covered_section *extra_sections;
static unsigned int num_extra_sections;
static unsigned long long skipped_uncovered;

struct line_entry {
	unsigned int offset;	/* offset from covered section's start */
	unsigned int section_id;/* index into covered_sections[] (module mode only) */
	unsigned int file_id;
	unsigned int line;
};

struct file_entry {
	char *name;
	unsigned int id;
	unsigned int str_offset;
};

static struct line_entry *entries;
static unsigned int num_entries;
static unsigned int entries_capacity;

static struct file_entry *files;
static unsigned int num_files;
static unsigned int files_capacity;

/*
 * Hash size must comfortably exceed the 65535-file cap below so the open
 * addressing in find_or_add_file() always has a free slot to land on.
 * 17 bits = 131072 entries gives ~50% max load factor.
 */
#define FILE_HASH_BITS 17
#define FILE_HASH_SIZE (1 << FILE_HASH_BITS)

struct file_hash_entry {
	const char *name;
	unsigned int id;
};

static struct file_hash_entry file_hash[FILE_HASH_SIZE];

static unsigned int hash_str(const char *s)
{
	unsigned int h = 5381;

	for (; *s; s++)
		h = h * 33 + (unsigned char)*s;
	return h & (FILE_HASH_SIZE - 1);
}

static void add_entry(unsigned int offset, unsigned int section_id,
		      unsigned int file_id, unsigned int line)
{
	if (num_entries >= entries_capacity) {
		entries_capacity = entries_capacity ? entries_capacity * 2 : 65536;
		entries = realloc(entries, entries_capacity * sizeof(*entries));
		if (!entries) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
	entries[num_entries].offset = offset;
	entries[num_entries].section_id = section_id;
	entries[num_entries].file_id = file_id;
	entries[num_entries].line = line;
	num_entries++;
}

static unsigned int find_or_add_file(const char *name)
{
	unsigned int h = hash_str(name);

	/* Open-addressing lookup with linear probing */
	while (file_hash[h].name) {
		if (!strcmp(file_hash[h].name, name))
			return file_hash[h].id;
		h = (h + 1) & (FILE_HASH_SIZE - 1);
	}

	if (num_files >= 65535) {
		fprintf(stderr,
			"gen_lineinfo: too many source files (%u > 65535)\n",
			num_files);
		exit(1);
	}

	if (num_files >= files_capacity) {
		files_capacity = files_capacity ? files_capacity * 2 : 4096;
		files = realloc(files, files_capacity * sizeof(*files));
		if (!files) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
	files[num_files].name = strdup(name);
	files[num_files].id = num_files;

	/* Insert into hash table (points to files[] entry) */
	file_hash[h].name = files[num_files].name;
	file_hash[h].id = num_files;

	num_files++;
	return num_files - 1;
}

/*
 * Well-known top-level directories in the kernel source tree.
 * Used as a fallback to recover relative paths from absolute DWARF paths
 * when comp_dir doesn't match (e.g. O= out-of-tree builds where comp_dir
 * is the build directory but source paths point into the source tree).
 */
static const char * const kernel_dirs[] = {
	"arch/", "block/", "certs/", "crypto/", "drivers/", "fs/",
	"include/", "init/", "io_uring/", "ipc/", "kernel/", "lib/",
	"mm/", "net/", "rust/", "samples/", "scripts/", "security/",
	"sound/", "tools/", "usr/", "virt/",
};

/*
 * Strip a filename to a kernel-relative path.
 *
 * For absolute paths, strip the comp_dir prefix (from DWARF) to get
 * a kernel-tree-relative path.  When that fails (e.g. O= builds where
 * comp_dir is the build directory), scan for a well-known kernel
 * top-level directory name in the path to recover the relative path.
 * Fall back to the basename as a last resort.
 *
 * For relative paths (common in modules), libdw may produce a bogus
 * doubled path like "net/foo/bar.c/net/foo/bar.c" due to ET_REL DWARF
 * quirks.  Detect and strip such duplicates.
 */
static const char *make_relative(const char *path, const char *comp_dir)
{
	const char *p;

	if (path[0] == '/') {
		/* Try comp_dir prefix from DWARF */
		if (comp_dir) {
			size_t len = strlen(comp_dir);

			if (!strncmp(path, comp_dir, len) && path[len] == '/') {
				const char *rel = path + len + 1;

				/*
				 * If comp_dir pointed to a subdirectory
				 * (e.g. arch/parisc/kernel) rather than
				 * the tree root, stripping it leaves a
				 * bare filename.  Fall through to the
				 * kernel_dirs scan so we recover the full
				 * relative path instead.
				 */
				if (strchr(rel, '/'))
					return rel;
			}
		}

		/*
		 * comp_dir prefix didn't help — either it didn't match
		 * or it was too specific and left a bare filename.
		 * Scan for a known kernel top-level directory component
		 * to find where the relative path starts.  This handles
		 * O= builds and arches where comp_dir is a subdirectory.
		 */
		for (p = path + 1; *p; p++) {
			if (*(p - 1) == '/') {
				for (unsigned int i = 0; i < sizeof(kernel_dirs) /
				     sizeof(kernel_dirs[0]); i++) {
					if (!strncmp(p, kernel_dirs[i],
						     strlen(kernel_dirs[i])))
						return p;
				}
			}
		}

		/* Fall back to basename */
		p = strrchr(path, '/');
		return p ? p + 1 : path;
	}

	/*
	 * Relative path — check for duplicated-path quirk from libdw
	 * on ET_REL files (e.g., "a/b.c/a/b.c" → "a/b.c").
	 */
	{
		size_t len = strlen(path);
		size_t mid = len / 2;

		if (len > 1 && path[mid] == '/' &&
		    !memcmp(path, path + mid + 1, mid))
			return path + mid + 1;
	}

	/*
	 * Bare filename with no directory component — try to recover the
	 * relative path using comp_dir.  Some toolchains/elfutils combos
	 * produce bare filenames where comp_dir holds the source directory.
	 * Construct the absolute path and run the kernel_dirs scan.
	 */
	if (!strchr(path, '/') && comp_dir && comp_dir[0] == '/') {
		static char buf[PATH_MAX];

		snprintf(buf, sizeof(buf), "%s/%s", comp_dir, path);
		for (p = buf + 1; *p; p++) {
			if (*(p - 1) == '/') {
				for (unsigned int i = 0; i < sizeof(kernel_dirs) /
				     sizeof(kernel_dirs[0]); i++) {
					if (!strncmp(p, kernel_dirs[i],
						     strlen(kernel_dirs[i])))
						return p;
				}
			}
		}
	}

	return path;
}

static int compare_entries(const void *a, const void *b)
{
	const struct line_entry *ea = a;
	const struct line_entry *eb = b;

	/* Group by section first so each per-section table is contiguous. */
	if (ea->section_id != eb->section_id)
		return ea->section_id < eb->section_id ? -1 : 1;
	if (ea->offset != eb->offset)
		return ea->offset < eb->offset ? -1 : 1;
	if (ea->file_id != eb->file_id)
		return ea->file_id < eb->file_id ? -1 : 1;
	if (ea->line != eb->line)
		return ea->line < eb->line ? -1 : 1;
	return 0;
}

/*
 * Look up a vmlinux symbol by exact name and return its st_value, or
 * @fallback if the symbol is absent (lets callers gracefully skip
 * optional bounds like _etext).
 */
static unsigned long long find_vmlinux_sym(Elf *elf, const char *name,
					   unsigned long long fallback,
					   bool required)
{
	size_t nsyms, i;
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		Elf_Data *data;

		if (!gelf_getshdr(scn, &shdr))
			continue;
		if (shdr.sh_type != SHT_SYMTAB)
			continue;

		data = elf_getdata(scn, NULL);
		if (!data)
			continue;

		nsyms = shdr.sh_size / shdr.sh_entsize;
		for (i = 0; i < nsyms; i++) {
			GElf_Sym sym;
			const char *sname;

			if (!gelf_getsym(data, i, &sym))
				continue;
			sname = elf_strptr(elf, shdr.sh_link, sym.st_name);
			if (sname && !strcmp(sname, name))
				return sym.st_value;
		}
	}

	if (required) {
		fprintf(stderr, "Cannot find %s symbol\n", name);
		exit(1);
	}
	return fallback;
}

static unsigned long long find_text_addr(Elf *elf)
{
	return find_vmlinux_sym(elf, "_text", 0, true);
}

/*
 * Vmlinux is linked in multiple passes: gen_lineinfo runs against
 * .tmp_vmlinux1 (which carries the empty lineinfo stub), and the resulting
 * tables are then linked into the final vmlinux.  Sections placed AFTER
 * .rodata (.init.text, .exit.text, ...) shift forward as the real lineinfo
 * tables replace the empty stub, so DWARF addresses we'd capture for them
 * here are stale by the time the kernel runs.
 *
 * Cap the captured range at _etext, the symbol that marks the end of the
 * .text section.  .text is placed BEFORE .rodata in every architecture's
 * vmlinux.lds.S, so its addresses are invariant across the relink.
 * Returns 0 on architectures or builds that don't expose _etext, in which
 * case the cap is disabled (preserving the v3 behavior — addresses past
 * .text remain captured but may be off in stack traces).
 */
static unsigned long long find_text_end_addr(Elf *elf)
{
	return find_vmlinux_sym(elf, "_etext", 0, false);
}

/*
 * Populate @sections[].present/sec_index/size/bias.  Sections that don't
 * exist stay marked absent.  Biases are assigned in array order: each
 * present section gets a base equal to the running total of preceding
 * present sections' sizes, rounded up to 16 to keep ranges sparse.  This
 * guarantees [bias, bias+size) ranges are pairwise disjoint and fit in
 * u32 as long as the sum of all covered text sizes is below 4 GiB.
 */
static void resolve_covered_sections(Elf *elf,
				     struct covered_section *sections,
				     unsigned int num_sections)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	size_t shstrndx;
	unsigned long long cursor = 0;

	if (elf_getshdrstrndx(elf, &shstrndx) != 0)
		return;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		const char *name;

		if (!gelf_getshdr(scn, &shdr))
			continue;
		name = elf_strptr(elf, shstrndx, shdr.sh_name);
		if (!name)
			continue;
		bool covered = false;

		for (unsigned int i = 0; i < num_sections; i++) {
			if (sections[i].present)
				continue;
			if (strcmp(name, sections[i].name))
				continue;
			if (shdr.sh_size > UINT_MAX) {
				fprintf(stderr,
					"lineinfo: section %s exceeds 4 GiB (size=%llu); skipping\n",
					name,
					(unsigned long long)shdr.sh_size);
				break;
			}
			sections[i].sec_index = elf_ndxscn(scn);
			sections[i].size = shdr.sh_size;
			sections[i].present = true;
			covered = true;
			break;
		}

		/*
		 * Track every other executable section too, so its DWARF
		 * sequences can be biased into their own range and dropped
		 * instead of polluting a covered section's table.
		 */
		if (!covered &&
		    (shdr.sh_flags & SHF_EXECINSTR) && (shdr.sh_flags & SHF_ALLOC) &&
		    shdr.sh_size && shdr.sh_size <= UINT_MAX) {
			struct covered_section *es;

			extra_sections = realloc(extra_sections,
						 (num_extra_sections + 1) *
						 sizeof(*extra_sections));
			if (!extra_sections) {
				fprintf(stderr, "out of memory\n");
				exit(1);
			}
			es = &extra_sections[num_extra_sections++];
			memset(es, 0, sizeof(*es));
			es->name = name;
			es->sec_index = elf_ndxscn(scn);
			es->size = shdr.sh_size;
			es->present = true;
		}
	}

	/* Pack present sections into non-overlapping bias ranges. */
	for (unsigned int i = 0; i < num_sections; i++) {
		if (!sections[i].present)
			continue;
		sections[i].bias = cursor;
		cursor += sections[i].size;
		cursor = (cursor + 15) & ~15ULL;	/* pad for separation */
	}
	for (unsigned int i = 0; i < num_extra_sections; i++) {
		extra_sections[i].bias = cursor;
		cursor += extra_sections[i].size;
		cursor = (cursor + 15) & ~15ULL;
	}
}

/* Look up a covered_section by ELF section header index. */
static struct covered_section *section_by_index(struct covered_section *sections,
						unsigned int num_sections,
						unsigned int sec_index)
{
	for (unsigned int i = 0; i < num_sections; i++) {
		if (sections[i].present && sections[i].sec_index == sec_index)
			return &sections[i];
	}
	return NULL;
}

/*
 * Apply .rela.debug_line relocations to a mutable copy of .debug_line data.
 *
 * elfutils libdw (through at least 0.194) does NOT apply relocations for
 * ET_REL files when using dwarf_begin_elf().  The internal libdwfl layer
 * does this via __libdwfl_relocate(), but that API is not public.
 *
 * For DWARF5, the .debug_line file name table uses DW_FORM_line_strp
 * references into .debug_line_str.  Without relocation, all these offsets
 * resolve to 0 (or garbage), causing dwarf_linesrc()/dwarf_filesrc() to
 * return wrong filenames (typically the comp_dir for every file).
 *
 * This function applies the relocations manually so that the patched
 * .debug_line data can be fed to dwarf_begin_elf() and produce correct
 * results.
 *
 * See elfutils bug https://sourceware.org/bugzilla/show_bug.cgi?id=31447
 * A fix (dwelf_elf_apply_relocs) was proposed but not yet merged as of
 * elfutils 0.194: https://sourceware.org/pipermail/elfutils-devel/2024q3/007388.html
 */
/*
 * Determine the relocation type for a 32-bit absolute reference
 * on the given architecture.  Returns 0 if unknown.
 */
static unsigned int r_type_abs32(unsigned int e_machine)
{
	switch (e_machine) {
	case EM_X86_64:		return R_X86_64_32;
	case EM_386:		return R_386_32;
	case EM_AARCH64:	return R_AARCH64_ABS32;
	case EM_ARM:		return R_ARM_ABS32;
	case EM_RISCV:		return R_RISCV_32;
	case EM_S390:		return R_390_32;
	case EM_MIPS:		return R_MIPS_32;
	case EM_PPC64:		return R_PPC64_ADDR32;
	case EM_PPC:		return R_PPC_ADDR32;
	case EM_LOONGARCH:	return R_LARCH_32;
	case EM_PARISC:		return R_PARISC_DIR32;
	default:		return 0;
	}
}

/*
 * Determine the relocation type for a 64-bit absolute reference
 * on the given architecture.  Returns 0 on 32-bit-only architectures
 * (where DW_LNE_set_address fits in 32 bits and r_type_abs32 covers it).
 */
static unsigned int r_type_abs64(unsigned int e_machine)
{
	switch (e_machine) {
	case EM_X86_64:		return R_X86_64_64;
	case EM_AARCH64:	return R_AARCH64_ABS64;
	case EM_RISCV:		return R_RISCV_64;
	case EM_S390:		return R_390_64;
	case EM_MIPS:		return R_MIPS_64;
	case EM_PPC64:		return R_PPC64_ADDR64;
	case EM_LOONGARCH:	return R_LARCH_64;
	case EM_PARISC:		return R_PARISC_DIR64;
	default:		return 0;
	}
}

/*
 * Write a 4- or 8-byte unsigned integer in target byte order.
 * Cross-builds (e.g. x86_64 host -> s390 module) need the patched
 * .debug_line bytes laid out per the .ko's e_ident[EI_DATA], not the host's.
 */
static void elf_write_uint(unsigned char *dst, uint64_t value, size_t size,
			   bool little_endian)
{
	if (little_endian) {
		for (size_t i = 0; i < size; i++)
			dst[i] = (value >> (i * 8)) & 0xff;
	} else {
		for (size_t i = 0; i < size; i++)
			dst[i] = (value >> ((size - 1 - i) * 8)) & 0xff;
	}
}

/* Counterpart to elf_write_uint: read the implicit addend of an SHT_REL
 * relocation, stored in the relocated field itself in target byte order.
 */
static uint64_t elf_read_uint(const unsigned char *src, size_t size,
			      bool little_endian)
{
	uint64_t value = 0;

	if (little_endian) {
		for (size_t i = 0; i < size; i++)
			value |= (uint64_t)src[i] << (i * 8);
	} else {
		for (size_t i = 0; i < size; i++)
			value |= (uint64_t)src[i] << ((size - 1 - i) * 8);
	}
	return value;
}

/*
 * Apply one .debug_line relocation.  Two reloc widths matter:
 *   abs32 - DW_FORM_line_strp file-table refs into .debug_line_str
 *   abs64 - DW_LNE_set_address arguments (sequence start PCs)
 * Without both, libdw sees zeros and reports wrong filenames or collapses
 * every sequence to address 0 (collision after dedup).
 *
 * @has_addend distinguishes RELA records (explicit @addend) from REL
 * records, whose addend is read from the relocated field itself.
 */
static void apply_one_dl_reloc(Elf_Data *dl_data, Elf_Data *sym_data,
			       bool target_le,
			       unsigned int abs32_type, unsigned int abs64_type,
			       GElf_Xword r_info, GElf_Addr r_offset,
			       GElf_Sxword addend, bool has_addend)
{
	GElf_Sym sym;
	unsigned int r_type = GELF_R_TYPE(r_info);
	size_t r_sym = GELF_R_SYM(r_info);
	bool is_abs64;
	size_t width;
	uint64_t value;

	if (abs32_type && r_type == abs32_type)
		is_abs64 = false;
	else if (abs64_type && r_type == abs64_type)
		is_abs64 = true;
	else
		return;

	if (!gelf_getsym(sym_data, r_sym, &sym))
		return;

	width = is_abs64 ? 8 : 4;

	if (r_offset + width > dl_data->d_size)
		return;

	if (!has_addend)
		addend = (GElf_Sxword)elf_read_uint(
				(unsigned char *)dl_data->d_buf + r_offset,
				width, target_le);

	value = (uint64_t)(sym.st_value + addend);

	/*
	 * If the relocation targets one of the tracked text sections, fold
	 * in that section's synthetic bias so the patched DWARF address
	 * lands in a unique numeric range.  String-ref relocs
	 * (DW_FORM_line_strp into .debug_line_str) target a different
	 * section, so the symbol-based check correctly excludes them from
	 * biasing — for both abs64 (64-bit ELF) and abs32 (32-bit ELF,
	 * where DW_LNE_set_address is also 4 bytes wide).
	 */
	if (module_mode) {
		struct covered_section *cs;

		cs = section_by_index(all_sections, ALL_SECTIONS,
				      sym.st_shndx);
		if (!cs)
			cs = section_by_index(extra_sections,
					      num_extra_sections,
					      sym.st_shndx);
		if (cs)
			value += cs->bias;
	}

	if (!is_abs64)
		value &= 0xffffffffULL;

	elf_write_uint((unsigned char *)dl_data->d_buf + r_offset,
		       value, width, target_le);
}

/* Walk one .rela.debug_line / .rel.debug_line table, if present. */
static void apply_dl_reloc_table(Elf_Scn *scn, bool is_rela,
				 Elf_Data *dl_data, Elf_Data *sym_data,
				 bool target_le,
				 unsigned int abs32_type,
				 unsigned int abs64_type)
{
	GElf_Shdr shdr;
	Elf_Data *data;
	size_t nrels, i;

	if (!scn)
		return;

	data = elf_getdata(scn, NULL);
	if (!data || !gelf_getshdr(scn, &shdr) || !shdr.sh_entsize)
		return;

	nrels = shdr.sh_size / shdr.sh_entsize;

	for (i = 0; i < nrels; i++) {
		if (is_rela) {
			GElf_Rela rela;

			if (!gelf_getrela(data, i, &rela))
				continue;
			apply_one_dl_reloc(dl_data, sym_data, target_le,
					   abs32_type, abs64_type,
					   rela.r_info, rela.r_offset,
					   rela.r_addend, true);
		} else {
			GElf_Rel rel;

			if (!gelf_getrel(data, i, &rel))
				continue;
			apply_one_dl_reloc(dl_data, sym_data, target_le,
					   abs32_type, abs64_type,
					   rel.r_info, rel.r_offset,
					   0, false);
		}
	}
}

static void apply_debug_line_relocations(Elf *elf)
{
	Elf_Scn *scn = NULL;
	Elf_Scn *debug_line_scn = NULL;
	Elf_Scn *rela_debug_line_scn = NULL;
	Elf_Scn *rel_debug_line_scn = NULL;
	Elf_Scn *symtab_scn = NULL;
	GElf_Shdr shdr;
	GElf_Ehdr ehdr;
	unsigned int abs32_type, abs64_type;
	bool target_le;
	size_t shstrndx;
	Elf_Data *dl_data, *sym_data;
	GElf_Shdr sym_shdr;

	if (gelf_getehdr(elf, &ehdr) == NULL)
		return;

	abs32_type = r_type_abs32(ehdr.e_machine);
	abs64_type = r_type_abs64(ehdr.e_machine);
	if (!abs32_type && !abs64_type)
		return;
	target_le = (ehdr.e_ident[EI_DATA] == ELFDATA2LSB);

	if (elf_getshdrstrndx(elf, &shstrndx) != 0)
		return;

	/* Find the relevant sections */
	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		const char *name;

		if (!gelf_getshdr(scn, &shdr))
			continue;
		name = elf_strptr(elf, shstrndx, shdr.sh_name);
		if (!name)
			continue;

		if (!strcmp(name, ".debug_line"))
			debug_line_scn = scn;
		else if (!strcmp(name, ".rela.debug_line"))
			rela_debug_line_scn = scn;
		else if (!strcmp(name, ".rel.debug_line"))
			rel_debug_line_scn = scn;
		else if (shdr.sh_type == SHT_SYMTAB)
			symtab_scn = scn;
	}

	if (!debug_line_scn || !symtab_scn)
		return;
	if (!rela_debug_line_scn && !rel_debug_line_scn)
		return;

	dl_data = elf_getdata(debug_line_scn, NULL);
	sym_data = elf_getdata(symtab_scn, NULL);
	if (!dl_data || !sym_data)
		return;

	if (!gelf_getshdr(symtab_scn, &sym_shdr))
		return;

	/*
	 * RELA (64-bit ELF and most 32-bit targets) carries explicit
	 * addends; REL (i386, arm32, ...) stores the addend in the
	 * relocated field itself.
	 */
	apply_dl_reloc_table(rela_debug_line_scn, true, dl_data, sym_data,
			     target_le, abs32_type, abs64_type);
	apply_dl_reloc_table(rel_debug_line_scn, false, dl_data, sym_data,
			     target_le, abs32_type, abs64_type);
}

/*
 * Decide which covered_section a (biased) DWARF address belongs to.
 * apply_debug_line_relocations() has already added the section's bias to
 * each line-program PC, so [bias, bias+size) ranges are pairwise disjoint
 * and a simple linear scan picks the right bucket.  Returns the index
 * within @sections, or @num_sections if @addr falls outside every
 * present range (caller skips the entry).
 */
static unsigned int classify_address(struct covered_section *sections,
				     unsigned int num_sections,
				     unsigned long long addr,
				     unsigned long long *out_offset)
{
	for (unsigned int i = 0; i < num_sections; i++) {
		if (!sections[i].present)
			continue;
		if (addr < sections[i].bias)
			continue;
		if (addr >= sections[i].bias + sections[i].size)
			continue;
		*out_offset = addr - sections[i].bias;
		return i;
	}
	return num_sections;
}

static void process_dwarf(Dwarf *dwarf, unsigned long long text_addr,
			  struct covered_section *sections,
			  unsigned int num_sections)
{
	Dwarf_Off off = 0, next_off;
	size_t hdr_size;

	while (dwarf_nextcu(dwarf, off, &next_off, &hdr_size,
			    NULL, NULL, NULL) == 0) {
		Dwarf_Die cudie;
		Dwarf_Lines *lines;
		size_t nlines;
		Dwarf_Attribute attr;
		const char *comp_dir = NULL;

		if (!dwarf_offdie(dwarf, off + hdr_size, &cudie))
			goto next;

		if (dwarf_attr(&cudie, DW_AT_comp_dir, &attr))
			comp_dir = dwarf_formstring(&attr);

		if (dwarf_getsrclines(&cudie, &lines, &nlines) != 0)
			goto next;

		for (size_t i = 0; i < nlines; i++) {
			Dwarf_Line *line = dwarf_onesrcline(lines, i);
			Dwarf_Addr addr;
			const char *src;
			const char *rel;
			unsigned int file_id, loffset, sec_id;
			unsigned long long sec_off;
			int lineno;

			if (!line)
				continue;

			if (dwarf_lineaddr(line, &addr) != 0)
				continue;
			if (dwarf_lineno(line, &lineno) != 0)
				continue;
			if (lineno == 0)
				continue;

			src = dwarf_linesrc(line, NULL, NULL);
			if (!src)
				continue;

			if (module_mode) {
				/*
				 * In ET_REL .ko files .text/.init.text/.exit.text
				 * all share sh_addr == 0; classify_address picks
				 * the right bucket from the explicit ranges we
				 * captured.
				 */
				sec_id = classify_address(sections, num_sections,
							  addr, &sec_off);
				if (sec_id == num_sections) {
					skipped_uncovered++;
					continue;
				}
				if (sec_off > UINT_MAX) {
					skipped_overflow++;
					continue;
				}
				loffset = (unsigned int)sec_off;
				sections[sec_id].n_entries++;
			} else {
				unsigned long long raw_offset;

				if (addr < text_addr)
					continue;
				/*
				 * Skip addresses past _etext.  Sections after
				 * .rodata shift when the real lineinfo replaces
				 * the empty stub during the multi-pass vmlinux
				 * link, so any address we'd capture there would
				 * be stale by the time the final kernel runs.
				 */
				if (text_end_addr && addr >= text_end_addr)
					continue;
				raw_offset = addr - text_addr;
				if (raw_offset > UINT_MAX) {
					skipped_overflow++;
					continue;
				}
				loffset = (unsigned int)raw_offset;
				sec_id = 0;
			}

			rel = make_relative(src, comp_dir);
			file_id = find_or_add_file(rel);

			add_entry(loffset, sec_id, file_id, (unsigned int)lineno);
		}
next:
		off = next_off;
	}
}

static void deduplicate(struct covered_section *sections,
			unsigned int num_sections)
{
	unsigned int i, j;

	if (num_entries < 2)
		return;

	/*
	 * Sort by section_id, then offset, then file_id, line.  This groups
	 * each section's entries contiguously so the per-section emit can
	 * iterate a simple range, and ensures the binary search invariant
	 * (offsets ascending) holds within each section.
	 */
	qsort(entries, num_entries, sizeof(*entries), compare_entries);

	/*
	 * Remove duplicates.  Reset on a section_id boundary: the same offset
	 * can legitimately appear in two different sections (they all start
	 * at sh_addr 0 in ET_REL), and the "same as previous kept entry"
	 * collapse is only meaningful inside one section's binary-search
	 * domain.
	 */
	j = 0;
	for (i = 1; i < num_entries; i++) {
		if (entries[i].section_id != entries[j].section_id) {
			j++;
			if (j != i)
				entries[j] = entries[i];
			continue;
		}
		if (entries[i].offset == entries[j].offset)
			continue;
		if (entries[i].file_id == entries[j].file_id &&
		    entries[i].line == entries[j].line)
			continue;
		j++;
		if (j != i)
			entries[j] = entries[i];
	}
	num_entries = j + 1;

	/* Recompute per-section n_entries from the deduped array. */
	if (sections) {
		for (unsigned int k = 0; k < num_sections; k++)
			sections[k].n_entries = 0;
		for (i = 0; i < num_entries; i++)
			sections[entries[i].section_id].n_entries++;
	}
}

static void compute_file_offsets(void)
{
	unsigned int offset = 0;

	for (unsigned int i = 0; i < num_files; i++) {
		files[i].str_offset = offset;
		offset += strlen(files[i].name) + 1;
	}
}

static void print_escaped_asciz(const char *s)
{
	printf("\t.asciz \"");
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			putchar('\\');
		putchar(*s);
	}
	printf("\"\n");
}

static void output_assembly(void)
{
	printf("/* SPDX-License-Identifier: GPL-2.0 */\n");
	printf("/*\n");
	printf(" * Automatically generated by scripts/gen_lineinfo\n");
	printf(" * Do not edit.\n");
	printf(" */\n\n");

	printf("\t.section .rodata, \"a\"\n\n");

	/* Number of entries */
	printf("\t.globl lineinfo_num_entries\n");
	printf("\t.balign 4\n");
	printf("lineinfo_num_entries:\n");
	printf("\t.long %u\n\n", num_entries);

	/* Number of files */
	printf("\t.globl lineinfo_num_files\n");
	printf("\t.balign 4\n");
	printf("lineinfo_num_files:\n");
	printf("\t.long %u\n\n", num_files);

	/* Sorted address offsets from _text */
	printf("\t.globl lineinfo_addrs\n");
	printf("\t.balign 4\n");
	printf("lineinfo_addrs:\n");
	for (unsigned int i = 0; i < num_entries; i++)
		printf("\t.long 0x%x\n", entries[i].offset);
	printf("\n");

	/* File IDs, parallel to addrs (u16 -- supports up to 65535 files) */
	printf("\t.globl lineinfo_file_ids\n");
	printf("\t.balign 2\n");
	printf("lineinfo_file_ids:\n");
	for (unsigned int i = 0; i < num_entries; i++)
		printf("\t.short %u\n", entries[i].file_id);
	printf("\n");

	/* Line numbers, parallel to addrs */
	printf("\t.globl lineinfo_lines\n");
	printf("\t.balign 4\n");
	printf("lineinfo_lines:\n");
	for (unsigned int i = 0; i < num_entries; i++)
		printf("\t.long %u\n", entries[i].line);
	printf("\n");

	/* File string offset table */
	printf("\t.globl lineinfo_file_offsets\n");
	printf("\t.balign 4\n");
	printf("lineinfo_file_offsets:\n");
	for (unsigned int i = 0; i < num_files; i++)
		printf("\t.long %u\n", files[i].str_offset);
	printf("\n");

	/* Filenames size */
	{
		unsigned int fsize = 0;

		for (unsigned int i = 0; i < num_files; i++)
			fsize += strlen(files[i].name) + 1;
		printf("\t.globl lineinfo_filenames_size\n");
		printf("\t.balign 4\n");
		printf("lineinfo_filenames_size:\n");
		printf("\t.long %u\n\n", fsize);
	}

	/* Concatenated NUL-terminated filenames */
	printf("\t.globl lineinfo_filenames\n");
	printf("lineinfo_filenames:\n");
	for (unsigned int i = 0; i < num_files; i++)
		print_escaped_asciz(files[i].name);
	printf("\n");
}

/*
 * Emit one per-section table in the simple flat-array layout:
 *
 *   mod_lineinfo_header
 *   addrs[count]    (u32, sorted)
 *   file_ids[count] (u16) + 2-byte pad if count is odd
 *   lines[count]    (u32)
 *   file_offsets[]  (u32)
 *   filenames[]
 *
 * @suffix uniquifies labels so multiple tables can coexist in one blob.
 * Caller has sorted entries[] so this section's entries occupy [first,
 * first + count).
 */
static void emit_section_table(unsigned int first, unsigned int count,
			       const char *suffix)
{
	printf(".Lhdr%s:\n", suffix);
	printf("\t.balign 4\n");
	printf("\t.long %u\t\t/* num_entries */\n", count);
	printf("\t.long %u\t\t/* num_files */\n", num_files);
	printf("\t.long .Lfilenames_end%s - .Lfilenames%s\n\n", suffix, suffix);

	/* addrs[] */
	for (unsigned int i = 0; i < count; i++)
		printf("\t.long 0x%x\n", entries[first + i].offset);

	/* file_ids[] */
	for (unsigned int i = 0; i < count; i++)
		printf("\t.short %u\n", entries[first + i].file_id);
	if (count & 1)
		printf("\t.short 0\t\t/* pad to align lines[] */\n");

	/* lines[] */
	for (unsigned int i = 0; i < count; i++)
		printf("\t.long %u\n", entries[first + i].line);

	/* file_offsets[] */
	printf("\t.balign 4\n");
	for (unsigned int i = 0; i < num_files; i++)
		printf("\t.long %u\n", files[i].str_offset);

	/* filenames[] */
	printf(".Lfilenames%s:\n", suffix);
	for (unsigned int i = 0; i < num_files; i++)
		print_escaped_asciz(files[i].name);
	printf(".Lfilenames_end%s:\n", suffix);
}

/*
 * Emit one mod_lineinfo_section descriptor.  The "anchor" field is a
 * relocation against the named ELF section symbol; the module loader
 * resolves it on load to the runtime base of that section.
 *
 * On 64-bit ELF: 8-byte slot via .quad <name> (R_*_64 reloc).
 * On 32-bit ELF: 4-byte reloc via .long <name>, plus 4 bytes of zero
 * padding.  The two halves are ordered to match target endianness so a
 * naive u64 read on the kernel side recovers the relocated value.
 */
static void emit_section_descriptor(const char *section_name,
				    unsigned long long size,
				    const char *table_label,
				    const char *root_label)
{
	if (target_64bit) {
		printf("\t.quad %s\t/* sections[].anchor (RELOC) */\n",
		       section_name);
	} else if (target_le) {
		printf("\t.long %s\t/* sections[].anchor low (RELOC) */\n",
		       section_name);
		printf("\t.long 0\t\t/* sections[].anchor high pad */\n");
	} else {
		printf("\t.long 0\t\t/* sections[].anchor high pad */\n");
		printf("\t.long %s\t/* sections[].anchor low (RELOC) */\n",
		       section_name);
	}
	printf("\t.long %llu\t/* sections[].size */\n", size);
	printf("\t.long %s - %s\t/* sections[].table_offset */\n",
	       table_label, root_label);
}

/*
 * Emit one .mod_lineinfo / .init.mod_lineinfo blob.  Walks all_sections[]
 * picking only entries that (a) belong to the requested blob and (b)
 * actually produced at least one DWARF line entry — sections present in
 * the .ko but without DWARF (e.g. compiler-generated stub thunks) are
 * silently skipped.  The caller-supplied entries[] is already sorted by
 * section_id, so each section's entries are contiguous; we walk the
 * master array in order to compute per-section starting indices.
 */
static void emit_blob(const char *output_section,
		      const char *blob_tag,
		      enum mod_lineinfo_blob blob)
{
	unsigned int active = 0;
	unsigned int section_starts[ALL_SECTIONS];
	unsigned int cursor = 0;

	for (unsigned int i = 0; i < ALL_SECTIONS; i++) {
		section_starts[i] = cursor;
		cursor += all_sections[i].n_entries;
		if (all_sections[i].blob == blob && all_sections[i].n_entries)
			active++;
	}

	if (!active)
		return;

	printf("\t.section %s, \"a\"\n\n", output_section);

	printf("\t.balign 8\n");
	printf(".Lroot_%s:\n", blob_tag);
	printf("\t.long %u\t\t/* num_sections */\n", active);
	/* Pad to align the u64 anchor in sections[0] to 8 bytes. */
	printf("\t.balign 8\n");

	{
		unsigned int slot = 0;
		for (unsigned int i = 0; i < ALL_SECTIONS; i++) {
			char table_label[64];
			char root_label[64];

			if (all_sections[i].blob != blob)
				continue;
			if (!all_sections[i].n_entries)
				continue;
			snprintf(table_label, sizeof(table_label),
				 ".Lhdr_%s_%u", blob_tag, slot);
			snprintf(root_label, sizeof(root_label),
				 ".Lroot_%s", blob_tag);
			emit_section_descriptor(all_sections[i].name,
						all_sections[i].size,
						table_label, root_label);
			slot++;
		}
	}
	printf("\n");

	{
		unsigned int slot = 0;

		for (unsigned int i = 0; i < ALL_SECTIONS; i++) {
			char suffix[64];

			if (all_sections[i].blob != blob)
				continue;
			if (!all_sections[i].n_entries)
				continue;
			snprintf(suffix, sizeof(suffix), "_%s_%u",
				 blob_tag, slot);
			emit_section_table(section_starts[i],
					   all_sections[i].n_entries,
					   suffix);
			slot++;
		}
	}
	printf("\n");
}

/*
 * Declare each text-like section we plan to reference as an empty
 * SHF_EXECINSTR section in this object.  Without these stanzas the
 * assembler treats `.quad .exit.text` as an undefined external symbol;
 * after ld -r the resulting GLOBAL UND `.exit.text` doesn't bind to the
 * .ko's LOCAL SECTION symbol of the same name, leaving depmod with an
 * unresolved-symbol warning and the loader unable to relocate the anchor.
 *
 * Declaring the section here gives lineinfo.o its own local SECTION
 * symbol; ld -r merges sections by name so the local symbol simply
 * relocates to offset 0 of the merged section (lineinfo.o is linked
 * FIRST so its zero-byte contribution stays at the start).
 */
static void declare_empty_text_sections(void)
{
	for (unsigned int i = 0; i < ALL_SECTIONS; i++) {
		if (!all_sections[i].present)
			continue;
		printf("\t.section %s, \"ax\"\n", all_sections[i].name);
	}
	printf("\n");
}

static void output_module_assembly(void)
{
	printf("/* SPDX-License-Identifier: GPL-2.0 */\n");
	printf("/*\n");
	printf(" * Automatically generated by scripts/gen_lineinfo --module\n");
	printf(" * Do not edit.\n");
	printf(" */\n\n");

	declare_empty_text_sections();
}

int main(int argc, char *argv[])
{
	int fd;
	Elf *elf;
	Dwarf *dwarf;
	unsigned long long text_addr;

	if (argc >= 2 && !strcmp(argv[1], "--module")) {
		module_mode = 1;
		argv++;
		argc--;
	}

	if (argc != 2) {
		fprintf(stderr, "Usage: %s [--module] <ELF file>\n", argv[0]);
		return 1;
	}

	/*
	 * For module mode, open O_RDWR so we can apply debug section
	 * relocations to the in-memory ELF data.  The modifications
	 * are NOT written back to disk (no elf_update() call).
	 */
	fd = open(argv[1], module_mode ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", argv[1],
			strerror(errno));
		return 1;
	}

	elf_version(EV_CURRENT);
	elf = elf_begin(fd, module_mode ? ELF_C_RDWR : ELF_C_READ, NULL);
	if (!elf) {
		fprintf(stderr, "elf_begin failed: %s\n",
			elf_errmsg(elf_errno()));
		close(fd);
		return 1;
	}

	{
		GElf_Ehdr ehdr;

		if (gelf_getehdr(elf, &ehdr) == NULL) {
			fprintf(stderr, "gelf_getehdr failed\n");
			elf_end(elf);
			close(fd);
			return 1;
		}
		target_64bit = (ehdr.e_ident[EI_CLASS] == ELFCLASS64);
		target_le = (ehdr.e_ident[EI_DATA] == ELFDATA2LSB);
	}

	if (module_mode) {
		/*
		 * .ko files are ET_REL after ld -r.  Resolve covered text
		 * sections FIRST so apply_debug_line_relocations() can use
		 * the assigned biases when patching line-program addresses;
		 * libdw does NOT apply relocations for ET_REL files, so we
		 * also handle DW_FORM_line_strp refs into .debug_line_str.
		 */
		resolve_covered_sections(elf, all_sections, ALL_SECTIONS);
		apply_debug_line_relocations(elf);
		text_addr = 0;	/* unused in module mode */
	} else {
		text_addr = find_text_addr(elf);
		text_end_addr = find_text_end_addr(elf);
	}

	dwarf = dwarf_begin_elf(elf, DWARF_C_READ, NULL);
	if (!dwarf) {
		fprintf(stderr, "dwarf_begin_elf failed: %s\n",
			dwarf_errmsg(dwarf_errno()));
		fprintf(stderr, "Is %s built with CONFIG_DEBUG_INFO?\n",
			argv[1]);
		elf_end(elf);
		close(fd);
		return 1;
	}

	if (module_mode) {
		unsigned int persistent_total, init_total;

		output_module_assembly();	/* file header only */

		/*
		 * Single DWARF pass classifies every line entry into its
		 * covering section (or skips it).  Each entry is tagged with
		 * the master-array section_id so per-blob emit can filter.
		 */
		process_dwarf(dwarf, 0, all_sections, ALL_SECTIONS);
		deduplicate(all_sections, ALL_SECTIONS);
		compute_file_offsets();

		emit_blob(".mod_lineinfo", "p", BLOB_PERSISTENT);
		emit_blob(".init.mod_lineinfo", "i", BLOB_INIT);

		persistent_total = 0;
		init_total = 0;
		for (unsigned int i = 0; i < ALL_SECTIONS; i++) {
			if (all_sections[i].blob == BLOB_PERSISTENT)
				persistent_total += all_sections[i].n_entries;
			else if (all_sections[i].blob == BLOB_INIT)
				init_total += all_sections[i].n_entries;
		}
		fprintf(stderr,
			"lineinfo: persistent %u entries, init %u entries, %u files\n",
			persistent_total, init_total, num_files);

		if (skipped_uncovered)
			fprintf(stderr,
				"lineinfo: %llu entries dropped (outside covered text sections)\n",
				skipped_uncovered);

		if (skipped_overflow)
			fprintf(stderr,
				"lineinfo: warning: %u entries skipped (offset > 4 GiB)\n",
				skipped_overflow);
	} else {
		process_dwarf(dwarf, text_addr, NULL, 0);

		if (skipped_overflow)
			fprintf(stderr,
				"lineinfo: warning: %u entries skipped (offset > 4 GiB from _text)\n",
				skipped_overflow);

		deduplicate(NULL, 0);
		compute_file_offsets();

		fprintf(stderr, "lineinfo: %u entries, %u files\n",
			num_entries, num_files);

		output_assembly();
	}

	dwarf_end(dwarf);
	elf_end(elf);
	close(fd);

	/* Cleanup */
	free(entries);
	for (unsigned int i = 0; i < num_files; i++)
		free(files[i].name);
	free(files);
	return 0;
}
