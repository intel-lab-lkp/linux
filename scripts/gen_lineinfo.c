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
 * The output uses a block-indexed, delta-encoded, ULEB128-compressed format
 * for ~3-4x size reduction compared to flat arrays.
 *
 * Requires libdw from elfutils.
 */

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

/* .text range for module mode (keep only runtime code) */
static unsigned long long text_section_start;
static unsigned long long text_section_end;

struct line_entry {
	unsigned int offset;	/* offset from _text */
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

/* Compressed output */
static unsigned char *compressed_data;
static unsigned int compressed_size;
static unsigned int compressed_capacity;

static unsigned int *block_addrs;
static unsigned int *block_offsets;
static unsigned int num_blocks;

#define FILE_HASH_BITS 13
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

static void add_entry(unsigned int offset, unsigned int file_id,
		      unsigned int line)
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
 * Strip a filename to a kernel-relative path.
 *
 * For absolute paths, strip the comp_dir prefix (from DWARF) to get
 * a kernel-tree-relative path, or fall back to the basename.
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

			if (!strncmp(path, comp_dir, len) && path[len] == '/')
				return path + len + 1;
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

		for (p = path; (p = strchr(p, '/')) != NULL; p++) {
			size_t prefix = p - path;
			size_t rest = len - prefix - 1;

			if (rest == prefix && !memcmp(path, p + 1, prefix))
				return p + 1;
		}
	}

	return path;
}

static int compare_entries(const void *a, const void *b)
{
	const struct line_entry *ea = a;
	const struct line_entry *eb = b;

	if (ea->offset != eb->offset)
		return ea->offset < eb->offset ? -1 : 1;
	if (ea->file_id != eb->file_id)
		return ea->file_id < eb->file_id ? -1 : 1;
	if (ea->line != eb->line)
		return ea->line < eb->line ? -1 : 1;
	return 0;
}

static unsigned long long find_text_addr(Elf *elf)
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
			const char *name;

			if (!gelf_getsym(data, i, &sym))
				continue;
			name = elf_strptr(elf, shdr.sh_link, sym.st_name);
			if (name && !strcmp(name, "_text"))
				return sym.st_value;
		}
	}

	fprintf(stderr, "Cannot find _text symbol\n");
	exit(1);
}

static void find_text_section_range(Elf *elf)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;
	size_t shstrndx;

	if (elf_getshdrstrndx(elf, &shstrndx) != 0)
		return;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		const char *name;

		if (!gelf_getshdr(scn, &shdr))
			continue;
		name = elf_strptr(elf, shstrndx, shdr.sh_name);
		if (name && !strcmp(name, ".text")) {
			text_section_start = shdr.sh_addr;
			text_section_end = shdr.sh_addr + shdr.sh_size;
			return;
		}
	}
}

static void process_dwarf(Dwarf *dwarf, unsigned long long text_addr)
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
			unsigned int file_id, loffset;
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

			if (addr < text_addr)
				continue;

			/*
			 * In module mode, keep only .text addresses.
			 * In ET_REL .ko files, .init.text/.exit.text may
			 * overlap with .text address ranges, so we must
			 * explicitly check against the .text bounds.
			 */
			if (module_mode && text_section_end > text_section_start &&
			    (addr < text_section_start || addr >= text_section_end))
				continue;

			{
				unsigned long long raw_offset = addr - text_addr;

				if (raw_offset > UINT_MAX) {
					skipped_overflow++;
					continue;
				}
				loffset = (unsigned int)raw_offset;
			}

			rel = make_relative(src, comp_dir);
			file_id = find_or_add_file(rel);

			add_entry(loffset, file_id, (unsigned int)lineno);
		}
next:
		off = next_off;
	}
}

static void deduplicate(void)
{
	unsigned int i, j;

	if (num_entries < 2)
		return;

	/* Sort by offset, then file_id, then line for stability */
	qsort(entries, num_entries, sizeof(*entries), compare_entries);

	/*
	 * Remove duplicate entries:
	 * - Same offset: keep first (deterministic from stable sort keys)
	 * - Same file:line as previous kept entry: redundant for binary
	 *   search -- any address between them resolves to the earlier one
	 */
	j = 0;
	for (i = 1; i < num_entries; i++) {
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
}

static void compressed_ensure(unsigned int need)
{
	if (compressed_size + need <= compressed_capacity)
		return;
	compressed_capacity = compressed_capacity ? compressed_capacity * 2 : 1024 * 1024;
	while (compressed_capacity < compressed_size + need)
		compressed_capacity *= 2;
	compressed_data = realloc(compressed_data, compressed_capacity);
	if (!compressed_data) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
}

static void compress_entries(void)
{
	unsigned int i, block;

	if (num_entries == 0) {
		num_blocks = 0;
		return;
	}

	num_blocks = (num_entries + LINEINFO_BLOCK_ENTRIES - 1) / LINEINFO_BLOCK_ENTRIES;
	block_addrs = calloc(num_blocks, sizeof(*block_addrs));
	block_offsets = calloc(num_blocks, sizeof(*block_offsets));
	if (!block_addrs || !block_offsets) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	for (block = 0; block < num_blocks; block++) {
		unsigned int base = block * LINEINFO_BLOCK_ENTRIES;
		unsigned int count = num_entries - base;
		unsigned int prev_addr, prev_file_id, prev_line;
		unsigned char buf[10]; /* max 5 bytes per ULEB128 */

		if (count > LINEINFO_BLOCK_ENTRIES)
			count = LINEINFO_BLOCK_ENTRIES;

		block_addrs[block] = entries[base].offset;
		block_offsets[block] = compressed_size;

		/* Entry 0: file_id (ULEB128), line (ULEB128) */
		compressed_ensure(20);
		compressed_size += lineinfo_write_uleb128(
			compressed_data + compressed_size,
			entries[base].file_id);
		compressed_size += lineinfo_write_uleb128(
			compressed_data + compressed_size,
			entries[base].line);

		prev_addr = entries[base].offset;
		prev_file_id = entries[base].file_id;
		prev_line = entries[base].line;

		/* Entries 1..N: deltas */
		for (i = 1; i < count; i++) {
			unsigned int idx = base + i;
			unsigned int addr_delta;
			int32_t file_delta, line_delta;
			unsigned int n;

			addr_delta = entries[idx].offset - prev_addr;
			file_delta = (int32_t)entries[idx].file_id - (int32_t)prev_file_id;
			line_delta = (int32_t)entries[idx].line - (int32_t)prev_line;

			compressed_ensure(15);
			n = lineinfo_write_uleb128(buf, addr_delta);
			memcpy(compressed_data + compressed_size, buf, n);
			compressed_size += n;

			n = lineinfo_write_uleb128(buf, zigzag_encode(file_delta));
			memcpy(compressed_data + compressed_size, buf, n);
			compressed_size += n;

			n = lineinfo_write_uleb128(buf, zigzag_encode(line_delta));
			memcpy(compressed_data + compressed_size, buf, n);
			compressed_size += n;

			prev_addr = entries[idx].offset;
			prev_file_id = entries[idx].file_id;
			prev_line = entries[idx].line;
		}
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

	/* Number of blocks */
	printf("\t.globl lineinfo_num_blocks\n");
	printf("\t.balign 4\n");
	printf("lineinfo_num_blocks:\n");
	printf("\t.long %u\n\n", num_blocks);

	/* Block first-addresses for binary search */
	printf("\t.globl lineinfo_block_addrs\n");
	printf("\t.balign 4\n");
	printf("lineinfo_block_addrs:\n");
	for (unsigned int i = 0; i < num_blocks; i++)
		printf("\t.long 0x%x\n", block_addrs[i]);
	printf("\n");

	/* Block byte offsets into compressed stream */
	printf("\t.globl lineinfo_block_offsets\n");
	printf("\t.balign 4\n");
	printf("lineinfo_block_offsets:\n");
	for (unsigned int i = 0; i < num_blocks; i++)
		printf("\t.long %u\n", block_offsets[i]);
	printf("\n");

	/* Compressed data stream */
	printf("\t.globl lineinfo_data\n");
	printf("lineinfo_data:\n");
	for (unsigned int i = 0; i < compressed_size; i++) {
		if ((i % 16) == 0)
			printf("\t.byte ");
		else
			printf(",");
		printf("0x%02x", compressed_data[i]);
		if ((i % 16) == 15 || i == compressed_size - 1)
			printf("\n");
	}
	printf("\n");

	/* File string offset table */
	printf("\t.globl lineinfo_file_offsets\n");
	printf("\t.balign 4\n");
	printf("lineinfo_file_offsets:\n");
	for (unsigned int i = 0; i < num_files; i++)
		printf("\t.long %u\n", files[i].str_offset);
	printf("\n");

	/* Concatenated NUL-terminated filenames */
	printf("\t.globl lineinfo_filenames\n");
	printf("lineinfo_filenames:\n");
	for (unsigned int i = 0; i < num_files; i++)
		print_escaped_asciz(files[i].name);
	printf("\n");
}

static void output_module_assembly(void)
{
	unsigned int filenames_size = 0;

	for (unsigned int i = 0; i < num_files; i++)
		filenames_size += strlen(files[i].name) + 1;

	printf("/* SPDX-License-Identifier: GPL-2.0 */\n");
	printf("/*\n");
	printf(" * Automatically generated by scripts/gen_lineinfo --module\n");
	printf(" * Do not edit.\n");
	printf(" */\n\n");

	printf("\t.section .mod_lineinfo, \"a\"\n\n");

	/* Header: num_entries, num_files, filenames_size, num_blocks, data_size, reserved */
	printf("\t.balign 4\n");
	printf("\t.long %u\n", num_entries);
	printf("\t.long %u\n", num_files);
	printf("\t.long %u\n", filenames_size);
	printf("\t.long %u\n", num_blocks);
	printf("\t.long %u\n", compressed_size);
	printf("\t.long 0\n\n");

	/* block_addrs[] */
	for (unsigned int i = 0; i < num_blocks; i++)
		printf("\t.long 0x%x\n", block_addrs[i]);
	if (num_blocks)
		printf("\n");

	/* block_offsets[] */
	for (unsigned int i = 0; i < num_blocks; i++)
		printf("\t.long %u\n", block_offsets[i]);
	if (num_blocks)
		printf("\n");

	/* compressed data[] */
	for (unsigned int i = 0; i < compressed_size; i++) {
		if ((i % 16) == 0)
			printf("\t.byte ");
		else
			printf(",");
		printf("0x%02x", compressed_data[i]);
		if ((i % 16) == 15 || i == compressed_size - 1)
			printf("\n");
	}
	if (compressed_size)
		printf("\n");

	/* file_offsets[] */
	for (unsigned int i = 0; i < num_files; i++)
		printf("\t.long %u\n", files[i].str_offset);
	if (num_files)
		printf("\n");

	/* filenames[] */
	for (unsigned int i = 0; i < num_files; i++)
		print_escaped_asciz(files[i].name);
	if (num_files)
		printf("\n");
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

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", argv[1],
			strerror(errno));
		return 1;
	}

	elf_version(EV_CURRENT);
	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (!elf) {
		fprintf(stderr, "elf_begin failed: %s\n",
			elf_errmsg(elf_errno()));
		close(fd);
		return 1;
	}

	if (module_mode) {
		/*
		 * .ko files are ET_REL after ld -r.  libdw applies
		 * relocations using section addresses, so DWARF addresses
		 * include the .text sh_addr.  Use .text sh_addr as the
		 * base so offsets are .text-relative.
		 */
		find_text_section_range(elf);
		text_addr = text_section_start;
	} else {
		text_addr = find_text_addr(elf);
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

	process_dwarf(dwarf, text_addr);

	if (skipped_overflow)
		fprintf(stderr,
			"lineinfo: warning: %u entries skipped (offset > 4 GiB from _text)\n",
			skipped_overflow);

	deduplicate();
	compress_entries();
	compute_file_offsets();

	fprintf(stderr, "lineinfo: %u entries, %u files, %u blocks, %u compressed bytes\n",
		num_entries, num_files, num_blocks, compressed_size);

	if (module_mode)
		output_module_assembly();
	else
		output_assembly();

	dwarf_end(dwarf);
	elf_end(elf);
	close(fd);

	/* Cleanup */
	free(entries);
	for (unsigned int i = 0; i < num_files; i++)
		free(files[i].name);
	free(files);
	free(compressed_data);
	free(block_addrs);
	free(block_offsets);

	return 0;
}
