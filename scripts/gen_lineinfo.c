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
 * Requires libelf and libdw from elfutils.
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
#include <array_size.h>
#include <hash.h>
#include <hashtable.h>
#include <xalloc.h>

#define LINEINFO_PREFIX "gen_lineinfo: "

static bool verbose;

#define verbose_msg(fmt, ...)						\
	do {								\
		if (verbose)						\
			fprintf(stderr, LINEINFO_PREFIX fmt "\n",	\
				##__VA_ARGS__);				\
	} while (0)

#define warn(fmt, ...) \
	fprintf(stderr, LINEINFO_PREFIX "warning: " fmt "\n", ##__VA_ARGS__)

#define error(fmt, ...)							\
	do {								\
		fprintf(stderr, LINEINFO_PREFIX "error: " fmt "\n",	\
			##__VA_ARGS__);					\
		exit(1);						\
	} while (0)

static unsigned int skipped_overflow;

/*
 * vmlinux mode: end of the invariant .text region.  Zero means "no cap"
 * (graceful fallback when _etext is absent on some build).
 */
static unsigned long long text_end_addr;

struct line_entry {
	unsigned int offset;	/* offset from _text */
	unsigned int file_id;
	unsigned int line;
};

/*
 * Individually allocated so files[] can grow without invalidating the
 * hlist_node linkage.
 */
struct file_entry {
	struct hlist_node hnode;
	unsigned int id;
	unsigned int str_offset;
	char name[];
};

static struct line_entry *entries;
static unsigned int num_entries;
static unsigned int entries_capacity;

static struct file_entry **files;
static unsigned int num_files;
static unsigned int files_capacity;

static HASHTABLE_DEFINE(file_hashtable, 1U << 13);

static void add_entry(unsigned int offset, unsigned int file_id,
		      unsigned int line)
{
	if (num_entries >= entries_capacity) {
		entries_capacity = entries_capacity ? entries_capacity * 2 : 65536;
		entries = xrealloc(entries, entries_capacity * sizeof(*entries));
	}
	entries[num_entries].offset = offset;
	entries[num_entries].file_id = file_id;
	entries[num_entries].line = line;
	num_entries++;
}

static unsigned int find_or_add_file(const char *name)
{
	unsigned int key = hash_str(name);
	struct file_entry *f;
	size_t len;

	hash_for_each_possible(file_hashtable, f, hnode, key)
		if (!strcmp(f->name, name))
			return f->id;

	if (num_files >= 65535)
		error("too many source files (%u > 65535)", num_files);

	if (num_files >= files_capacity) {
		files_capacity = files_capacity ? files_capacity * 2 : 4096;
		files = xrealloc(files, files_capacity * sizeof(*files));
	}

	len = strlen(name);
	f = xmalloc(sizeof(*f) + len + 1);
	memset(f, 0, sizeof(*f));
	memcpy(f->name, name, len + 1);
	f->id = num_files;

	files[num_files] = f;
	hash_add(file_hashtable, &f->hnode, key);

	return num_files++;
}

/*
 * Well-known top-level directories in the kernel source tree.  Only used
 * as a last resort, when a path matches none of the build roots below --
 * e.g. an object compiled outside any of them.
 */
static const char * const kernel_dirs[] = {
	"arch/", "block/", "certs/", "crypto/", "drivers/", "fs/",
	"include/", "init/", "io_uring/", "ipc/", "kernel/", "lib/",
	"mm/", "net/", "rust/", "samples/", "scripts/", "security/",
	"sound/", "tools/", "usr/", "virt/",
};

/* Absolute build and source roots, longest first. */
struct path_root {
	char *path;
	size_t len;
};

static struct path_root path_roots[8];
static unsigned int num_path_roots;

/*
 * Lexically canonicalize @path in place: collapse repeated slashes, drop
 * "." components and resolve ".." against the preceding component.  Purely
 * textual -- nothing is stat()ed, because DWARF can name generated files
 * that do not exist yet when gen_lineinfo runs.
 */
static void normalize_path(char *path)
{
	bool absolute = path[0] == '/';
	char *base, *out = path;
	const char *in = path;

	if (absolute)
		*out++ = *in++;
	base = out;

	while (*in) {
		const char *seg = in;
		size_t seglen;

		while (*in && *in != '/')
			in++;
		seglen = in - seg;
		while (*in == '/')
			in++;

		if (!seglen || (seglen == 1 && seg[0] == '.'))
			continue;

		if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
			if (out > base) {
				/* Pop the previously emitted component. */
				while (out > base && out[-1] != '/')
					out--;
				if (out > base)
					out--;	/* and its separator */
				continue;
			}
			/* "/.." is "/"; a leading ".." in a relative path stays. */
			if (absolute)
				continue;
		}

		/*
		 * Separator first: writing it after the component would land
		 * on the byte @in still points at whenever nothing has been
		 * compacted yet, clobbering the terminator and running the
		 * loop off the end of the string.
		 */
		if (out > base)
			*out++ = '/';
		memmove(out, seg, seglen);
		out += seglen;
	}

	if (out == base && !absolute)
		*out++ = '.';
	*out = '\0';
}

static int compare_path_roots(const void *a, const void *b)
{
	const struct path_root *ra = a, *rb = b;

	if (ra->len != rb->len)
		return ra->len > rb->len ? -1 : 1;
	return 0;
}

static void add_path_root(const char *path)
{
	char buf[PATH_MAX];

	if (!path || !*path)
		return;

	if (path[0] == '/') {
		if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
			return;
	} else {
		char cwd[PATH_MAX];

		/* kbuild runs host tools with cwd == $objtree. */
		if (!getcwd(cwd, sizeof(cwd)))
			return;
		if (snprintf(buf, sizeof(buf), "%s/%s", cwd, path) >= (int)sizeof(buf))
			return;
	}

	normalize_path(buf);

	/* "/" would match every absolute path. */
	if (!strcmp(buf, "/"))
		return;

	for (unsigned int i = 0; i < num_path_roots; i++)
		if (!strcmp(path_roots[i].path, buf))
			return;

	if (num_path_roots == ARRAY_SIZE(path_roots))
		return;

	path_roots[num_path_roots].path = xstrdup(buf);
	path_roots[num_path_roots].len = strlen(buf);
	num_path_roots++;
}

/*
 * Collect the roots that DWARF paths get made relative to.  kbuild exports
 * all three, so no Makefile plumbing is needed: $objtree and $srctree cover
 * in-tree and O= builds, and $srcroot covers M= external modules, whose
 * sources live under neither.
 */
static void init_path_roots(void)
{
	static const char * const vars[] = { "objtree", "srctree", "srcroot" };

	for (unsigned int i = 0; i < ARRAY_SIZE(vars); i++) {
		const char *val = getenv(vars[i]);
		char *real;

		if (!val || !*val)
			continue;

		add_path_root(val);

		/*
		 * Register the resolved form as well, so a symlinked tree
		 * matches whichever spelling the compiler recorded.  Only
		 * the roots are resolved this way -- never a DWARF path.
		 */
		real = realpath(val, NULL);
		if (real) {
			add_path_root(real);
			free(real);
		}
	}

	qsort(path_roots, num_path_roots, sizeof(*path_roots),
	      compare_path_roots);
}

/*
 * Strip a DWARF filename down to a kernel-tree-relative path.
 *
 * Per DWARF, a relative DW_AT_name is relative to the CU's DW_AT_comp_dir,
 * so the two are joined and canonicalized first.  The result is then made
 * relative to the longest matching build root.  Everything after that is a
 * fallback for objects built outside the tree.
 */
static const char *make_relative(const char *path, const char *comp_dir)
{
	static char buf[PATH_MAX];
	const char *p;

	if (path[0] == '/') {
		if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
			return path;
	} else if (comp_dir && comp_dir[0] == '/') {
		if (snprintf(buf, sizeof(buf), "%s/%s", comp_dir, path) >=
		    (int)sizeof(buf))
			return path;
	} else {
		/* Nothing absolute to anchor against. */
		return path;
	}

	normalize_path(buf);

	for (unsigned int i = 0; i < num_path_roots; i++) {
		size_t len = path_roots[i].len;

		if (!strncmp(buf, path_roots[i].path, len) && buf[len] == '/')
			return buf + len + 1;
	}

	/*
	 * comp_dir may still be a usable prefix even when it is not one of
	 * the roots -- but only if stripping it leaves a directory
	 * component, otherwise the kernel_dirs scan recovers more.
	 */
	if (comp_dir) {
		size_t len = strlen(comp_dir);

		if (!strncmp(buf, comp_dir, len) && buf[len] == '/' &&
		    strchr(buf + len + 1, '/'))
			return buf + len + 1;
	}

	for (p = strchr(buf, '/'); p; p = strchr(p + 1, '/'))
		for (unsigned int i = 0; i < ARRAY_SIZE(kernel_dirs); i++)
			if (!strncmp(p + 1, kernel_dirs[i],
				     strlen(kernel_dirs[i])))
				return p + 1;

	p = strrchr(buf, '/');
	return p ? p + 1 : buf;
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

/*
 * Look up a vmlinux symbol by exact name and return its st_value, or
 * @fallback if absent.  Aborts when @required and the symbol is missing.
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

	if (required)
		error("cannot find %s symbol", name);
	return fallback;
}

static unsigned long long find_text_addr(Elf *elf)
{
	return find_vmlinux_sym(elf, "_text", 0, true);
}

/*
 * vmlinux is linked in multiple passes: gen_lineinfo runs against
 * .tmp_vmlinux1 (which carries an empty lineinfo stub), then real tables
 * are linked in for the final image.  Sections placed AFTER .rodata
 * (.init.text, .exit.text, ...) shift forward as .rodata grows to hold
 * the real lineinfo blob, so DWARF addresses we'd capture for them in
 * pass 1 would be stale in the final kernel.  Cap captured addresses at
 * _etext, the symbol that marks the end of .text — placed before .rodata
 * in every architecture's vmlinux.lds.S, so its addresses are invariant
 * across the relink.  Returns 0 if _etext is absent (no cap; v3 behavior).
 */
static unsigned long long find_text_end_addr(Elf *elf)
{
	return find_vmlinux_sym(elf, "_etext", 0, false);
}

static int compare_uints(const void *a, const void *b)
{
	unsigned int ua = *(const unsigned int *)a;
	unsigned int ub = *(const unsigned int *)b;

	if (ua != ub)
		return ua < ub ? -1 : 1;
	return 0;
}

/* Sorted, duplicate-free extents of every function symbol. */
struct sym_start {
	unsigned int offset;
	unsigned int size;
};

static struct sym_start *sym_starts;
static unsigned int num_sym_starts;
static unsigned int sym_starts_capacity;

/* Sorted offsets one past the end of each DWARF line-program sequence. */
static unsigned int *seq_ends;
static unsigned int num_seq_ends;
static unsigned int seq_ends_capacity;

static void append_offset(unsigned int **arr, unsigned int *count,
			  unsigned int *capacity, unsigned int value)
{
	if (*count >= *capacity) {
		*capacity = *capacity ? *capacity * 2 : 16384;
		*arr = xrealloc(*arr, *capacity * sizeof(**arr));
	}
	(*arr)[(*count)++] = value;
}

static void sort_unique(unsigned int *arr, unsigned int *count)
{
	unsigned int j = 0;

	if (*count < 2)
		return;

	qsort(arr, *count, sizeof(*arr), compare_uints);
	for (unsigned int i = 1; i < *count; i++) {
		if (arr[i] == arr[j])
			continue;
		if (++j != i)
			arr[j] = arr[i];
	}
	*count = j + 1;
}

/*
 * Record the end of a line-program sequence.  @addr is one past the last
 * covered byte, so the sequence's own coverage is tested using addr - 1.
 */
static void record_seq_end(unsigned long long addr,
			   unsigned long long text_addr)
{
	unsigned long long raw;

	if (addr <= text_addr)
		return;
	if (text_end_addr && addr - 1 >= text_end_addr)
		return;

	raw = addr - text_addr;
	if (raw > UINT_MAX)
		return;

	append_offset(&seq_ends, &num_seq_ends, &seq_ends_capacity,
		      (unsigned int)raw);
}

static int compare_sym_starts(const void *a, const void *b)
{
	const struct sym_start *sa = a, *sb = b;

	if (sa->offset != sb->offset)
		return sa->offset < sb->offset ? -1 : 1;
	/* Larger extent first, so the dedup below keeps it. */
	if (sa->size != sb->size)
		return sa->size > sb->size ? -1 : 1;
	return 0;
}

/*
 * Collect the extent of every function symbol.  deduplicate() uses these to
 * make sure each function keeps an entry at its own first byte; without that
 * the kernel's symbol-boundary check rejects the preceding function's entry
 * and the frame goes unannotated.
 */
static void collect_symbol_starts(Elf *elf, unsigned long long text_addr)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		Elf_Data *data;
		size_t nsyms;

		if (!gelf_getshdr(scn, &shdr))
			continue;
		if (shdr.sh_type != SHT_SYMTAB || !shdr.sh_entsize)
			continue;

		data = elf_getdata(scn, NULL);
		if (!data)
			continue;

		nsyms = shdr.sh_size / shdr.sh_entsize;
		for (size_t i = 0; i < nsyms; i++) {
			GElf_Sym sym;
			unsigned long long raw;

			if (!gelf_getsym(data, i, &sym))
				continue;
			if (GELF_ST_TYPE(sym.st_info) != STT_FUNC)
				continue;
			if (sym.st_value < text_addr)
				continue;
			if (text_end_addr && sym.st_value >= text_end_addr)
				continue;

			raw = sym.st_value - text_addr;
			if (raw > UINT_MAX)
				continue;

			if (num_sym_starts >= sym_starts_capacity) {
				sym_starts_capacity = sym_starts_capacity ?
					sym_starts_capacity * 2 : 16384;
				sym_starts = xrealloc(sym_starts,
						      sym_starts_capacity *
						      sizeof(*sym_starts));
			}
			sym_starts[num_sym_starts].offset = (unsigned int)raw;
			sym_starts[num_sym_starts].size =
				sym.st_size > UINT_MAX ? UINT_MAX :
				(unsigned int)sym.st_size;
			num_sym_starts++;
		}
	}

	if (num_sym_starts > 1) {
		unsigned int j = 0;

		qsort(sym_starts, num_sym_starts, sizeof(*sym_starts),
		      compare_sym_starts);
		for (unsigned int i = 1; i < num_sym_starts; i++) {
			if (sym_starts[i].offset == sym_starts[j].offset)
				continue;
			if (++j != i)
				sym_starts[j] = sym_starts[i];
		}
		num_sym_starts = j + 1;
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
			bool endseq = false;
			int lineno;

			if (!line)
				continue;

			if (dwarf_lineaddr(line, &addr) != 0)
				continue;

			/*
			 * An end_sequence row marks the first address NOT
			 * covered by this sequence; libdw repeats the previous
			 * line number on it, so keeping it as an entry would
			 * extend a function's annotation past its own end.
			 * Record the boundary instead -- deduplicate() needs
			 * it to tell "this row still covers the next symbol"
			 * from "coverage stopped here".
			 */
			if (dwarf_lineendsequence(line, &endseq) == 0 && endseq) {
				record_seq_end(addr, text_addr);
				continue;
			}

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
			 * Skip addresses past _etext.  Sections after .rodata
			 * shift when the real lineinfo replaces the empty stub
			 * during the multi-pass vmlinux link, so any address
			 * we'd capture there would be stale by the time the
			 * final kernel runs.
			 */
			if (text_end_addr && addr >= text_end_addr)
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

/* True if some line-program sequence ends in (@lo, @hi]. */
static bool seq_end_between(unsigned int lo, unsigned int hi)
{
	unsigned int low = 0, high = num_seq_ends;

	/* First index whose value exceeds @lo. */
	while (low < high) {
		unsigned int mid = low + (high - low) / 2;

		if (seq_ends[mid] <= lo)
			low = mid + 1;
		else
			high = mid;
	}

	return low < num_seq_ends && seq_ends[low] <= hi;
}

/*
 * Give every function an entry at its own first byte.
 *
 * Compilers routinely emit no line row at a symbol's start: .cold
 * fragments in particular are covered by a row belonging to the function
 * they were split out of.  That used to resolve fine, but the kernel now
 * refuses any entry below the resolved symbol's start, so those frames
 * would print unannotated.  Copy the covering row down to the symbol
 * start instead.
 *
 * Two things have to hold before that is honest:
 *
 *  - the covering row's sequence must not have ended in between, or it
 *    describes code that stopped before this symbol.  This is what keeps
 *    the __SCT__* static-call trampolines unannotated.
 *
 *  - the line program must place at least one row inside the symbol, so
 *    we know it describes this symbol's code at all.  This is what keeps
 *    the __pfx_* padding stubs unannotated: they are pure alignment
 *    padding, and no compiler ever emits a row inside one.
 *
 * Symbols failing either test keep no annotation, which is the correct
 * answer for hand-written assembly.
 */
static void synthesize_symbol_starts(void)
{
	unsigned int base_entries = num_entries;
	unsigned int cursor = 0;

	if (!base_entries || !num_sym_starts)
		return;

	sort_unique(seq_ends, &num_seq_ends);

	for (unsigned int i = 0; i < num_sym_starts; i++) {
		unsigned int start = sym_starts[i].offset;
		unsigned int end = start + sym_starts[i].size;

		while (cursor + 1 < base_entries &&
		       entries[cursor + 1].offset <= start)
			cursor++;

		if (entries[cursor].offset > start)
			continue;	/* nothing covers this symbol */
		if (entries[cursor].offset == start)
			continue;	/* already has its own entry */
		if (seq_end_between(entries[cursor].offset, start))
			continue;	/* coverage stopped before here */

		/* Overflow, or no row inside [start, end): not our code. */
		if (end <= start)
			continue;
		if (cursor + 1 >= base_entries ||
		    entries[cursor + 1].offset >= end)
			continue;

		add_entry(start, entries[cursor].file_id, entries[cursor].line);
	}
}

static void deduplicate(void)
{
	unsigned int sym_cursor = 0;
	unsigned int i, j;

	if (num_entries < 2)
		return;

	/* Sort by offset, then file_id, then line for stability */
	qsort(entries, num_entries, sizeof(*entries), compare_entries);

	synthesize_symbol_starts();
	qsort(entries, num_entries, sizeof(*entries), compare_entries);

	/*
	 * Remove duplicate entries:
	 * - Same offset: keep first (deterministic from stable sort keys)
	 * - Same file:line as previous kept entry: redundant for binary
	 *   search -- any address between them resolves to the earlier one
	 *
	 * Entries sitting on a symbol start are exempt from the second rule:
	 * they are the only thing standing between that symbol and the
	 * kernel's boundary check.
	 */
	j = 0;
	for (i = 1; i < num_entries; i++) {
		bool at_symbol_start;

		if (entries[i].offset == entries[j].offset)
			continue;

		while (sym_cursor < num_sym_starts &&
		       sym_starts[sym_cursor].offset < entries[i].offset)
			sym_cursor++;
		at_symbol_start = sym_cursor < num_sym_starts &&
				  sym_starts[sym_cursor].offset == entries[i].offset;

		if (!at_symbol_start &&
		    entries[i].file_id == entries[j].file_id &&
		    entries[i].line == entries[j].line)
			continue;

		j++;
		if (j != i)
			entries[j] = entries[i];
	}
	num_entries = j + 1;
}

static void compute_file_offsets(void)
{
	unsigned int offset = 0;

	for (unsigned int i = 0; i < num_files; i++) {
		files[i]->str_offset = offset;
		offset += strlen(files[i]->name) + 1;
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
		printf("\t.long %u\n", files[i]->str_offset);
	printf("\n");

	/* Filenames size */
	{
		unsigned int fsize = 0;

		for (unsigned int i = 0; i < num_files; i++)
			fsize += strlen(files[i]->name) + 1;
		printf("\t.globl lineinfo_filenames_size\n");
		printf("\t.balign 4\n");
		printf("lineinfo_filenames_size:\n");
		printf("\t.long %u\n\n", fsize);
	}

	/* Concatenated NUL-terminated filenames */
	printf("\t.globl lineinfo_filenames\n");
	printf("lineinfo_filenames:\n");
	for (unsigned int i = 0; i < num_files; i++)
		print_escaped_asciz(files[i]->name);
	printf("\n");
}

int main(int argc, char *argv[])
{
	const char *kbuild_verbose = getenv("KBUILD_VERBOSE");
	unsigned long long text_addr;
	Dwarf *dwarf;
	Elf *elf;
	int fd;

	if (kbuild_verbose && strchr(kbuild_verbose, '1'))
		verbose = true;

	while (argc > 2 && (!strcmp(argv[1], "-v") ||
			    !strcmp(argv[1], "--verbose"))) {
		verbose = true;
		memmove(&argv[1], &argv[2], (argc - 2) * sizeof(char *));
		argc--;
	}

	if (argc != 2) {
		fprintf(stderr, "Usage: %s [-v] <vmlinux>\n", argv[0]);
		return 1;
	}

	init_path_roots();

	fd = open(argv[1], O_RDONLY);
	if (fd < 0)
		error("cannot open %s: %s", argv[1], strerror(errno));

	elf_version(EV_CURRENT);
	elf = elf_begin(fd, ELF_C_READ_MMAP, NULL);
	if (!elf)
		error("elf_begin failed: %s", elf_errmsg(elf_errno()));

	text_addr = find_text_addr(elf);
	text_end_addr = find_text_end_addr(elf);
	collect_symbol_starts(elf, text_addr);

	dwarf = dwarf_begin_elf(elf, DWARF_C_READ, NULL);
	if (!dwarf)
		error("dwarf_begin_elf failed: %s\n"
		      LINEINFO_PREFIX "error: is %s built with CONFIG_DEBUG_INFO?",
		      dwarf_errmsg(dwarf_errno()), argv[1]);

	process_dwarf(dwarf, text_addr);

	if (skipped_overflow)
		warn("%u entries skipped (offset > 4 GiB from _text)",
		     skipped_overflow);

	deduplicate();
	compute_file_offsets();

	verbose_msg("%u entries, %u files", num_entries, num_files);

	output_assembly();

	dwarf_end(dwarf);
	elf_end(elf);
	close(fd);

	/* Cleanup */
	free(entries);
	free(sym_starts);
	free(seq_ends);
	for (unsigned int i = 0; i < num_files; i++)
		free(files[i]);
	free(files);

	return 0;
}
