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
#define ALL_SECTIONS	ARRAY_SIZE(all_sections)

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

static void add_entry(unsigned int offset, unsigned int section_id,
		      unsigned int file_id, unsigned int line)
{
	if (num_entries >= entries_capacity) {
		entries_capacity = entries_capacity ? entries_capacity * 2 : 65536;
		entries = xrealloc(entries, entries_capacity * sizeof(*entries));
	}
	entries[num_entries].offset = offset;
	entries[num_entries].section_id = section_id;
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

	if (path[0] != '/') {
		/*
		 * libdw doubles relative paths on ET_REL input
		 * (e.g. "a/b.c/a/b.c" -> "a/b.c").  Undo that before the
		 * path is joined to comp_dir, or the halves stop matching.
		 */
		size_t len = strlen(path);
		size_t mid = len / 2;

		if (len > 1 && path[mid] == '/' &&
		    !memcmp(path, path + mid + 1, mid))
			path += mid + 1;
	}

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

	if (required)
		error("cannot find %s symbol", name);
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
 * Ordering shared by entries[], sym_starts[] and seq_ends[]: section first,
 * then offset.  In module mode every offset is section-relative, so the two
 * together are what identifies a location.
 */
static int compare_sec_off(unsigned int sa, unsigned int oa,
			   unsigned int sb, unsigned int ob)
{
	if (sa != sb)
		return sa < sb ? -1 : 1;
	if (oa != ob)
		return oa < ob ? -1 : 1;
	return 0;
}

/* Sorted, duplicate-free extents of every function symbol. */
struct sym_start {
	unsigned int section_id;
	unsigned int offset;
	unsigned int size;
};

static struct sym_start *sym_starts;
static unsigned int num_sym_starts;
static unsigned int sym_starts_capacity;

/* Sorted locations one past the end of each DWARF line-program sequence. */
struct seq_end {
	unsigned int section_id;
	unsigned int offset;
};

static struct seq_end *seq_ends;
static unsigned int num_seq_ends;
static unsigned int seq_ends_capacity;

static int compare_seq_ends(const void *a, const void *b)
{
	const struct seq_end *ea = a, *eb = b;

	return compare_sec_off(ea->section_id, ea->offset,
			       eb->section_id, eb->offset);
}

static void record_seq_end(unsigned int section_id, unsigned int offset)
{
	if (num_seq_ends >= seq_ends_capacity) {
		seq_ends_capacity = seq_ends_capacity ?
				    seq_ends_capacity * 2 : 16384;
		seq_ends = xrealloc(seq_ends,
				    seq_ends_capacity * sizeof(*seq_ends));
	}
	seq_ends[num_seq_ends].section_id = section_id;
	seq_ends[num_seq_ends].offset = offset;
	num_seq_ends++;
}

static int compare_sym_starts(const void *a, const void *b)
{
	const struct sym_start *sa = a, *sb = b;
	int ret = compare_sec_off(sa->section_id, sa->offset,
				  sb->section_id, sb->offset);

	if (ret)
		return ret;
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
static void collect_symbol_starts(Elf *elf, unsigned long long text_addr,
				  struct covered_section *sections,
				  unsigned int num_sections)
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
			unsigned int sec_id = 0;
			unsigned long long raw;
			GElf_Sym sym;

			if (!gelf_getsym(data, i, &sym))
				continue;
			if (GELF_ST_TYPE(sym.st_info) != STT_FUNC)
				continue;

			if (module_mode) {
				/*
				 * ET_REL: st_value is already relative to the
				 * symbol's own section, so only sections the
				 * blob covers are of interest.
				 */
				for (sec_id = 0; sec_id < num_sections; sec_id++)
					if (sections[sec_id].present &&
					    sections[sec_id].sec_index ==
					    sym.st_shndx)
						break;
				if (sec_id == num_sections)
					continue;
				if (sym.st_value >= sections[sec_id].size)
					continue;
				raw = sym.st_value;
			} else {
				if (sym.st_value < text_addr)
					continue;
				if (text_end_addr &&
				    sym.st_value >= text_end_addr)
					continue;
				raw = sym.st_value - text_addr;
			}

			if (raw > UINT_MAX)
				continue;

			if (num_sym_starts >= sym_starts_capacity) {
				sym_starts_capacity = sym_starts_capacity ?
					sym_starts_capacity * 2 : 16384;
				sym_starts = xrealloc(sym_starts,
						      sym_starts_capacity *
						      sizeof(*sym_starts));
			}
			sym_starts[num_sym_starts].section_id = sec_id;
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
			if (!compare_sec_off(sym_starts[i].section_id,
					     sym_starts[i].offset,
					     sym_starts[j].section_id,
					     sym_starts[j].offset))
				continue;
			if (++j != i)
				sym_starts[j] = sym_starts[i];
		}
		num_sym_starts = j + 1;
	}
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
				warn("section %s exceeds 4 GiB (size=%llu); skipping",
				     name, (unsigned long long)shdr.sh_size);
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

			extra_sections = xrealloc(extra_sections,
						  (num_extra_sections + 1) *
						  sizeof(*extra_sections));
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

/*
 * Classify the end of a line-program sequence.  @addr is one past the last
 * covered byte, so the section it belongs to is resolved from addr - 1.
 */
static void classify_seq_end(unsigned long long addr,
			     unsigned long long text_addr,
			     struct covered_section *sections,
			     unsigned int num_sections)
{
	unsigned long long raw;

	if (!addr)
		return;

	if (module_mode) {
		unsigned long long sec_off;
		unsigned int sec_id;

		sec_id = classify_address(sections, num_sections, addr - 1,
					  &sec_off);
		if (sec_id == num_sections || sec_off + 1 > UINT_MAX)
			return;
		record_seq_end(sec_id, (unsigned int)sec_off + 1);
		return;
	}

	if (addr <= text_addr)
		return;
	if (text_end_addr && addr - 1 >= text_end_addr)
		return;

	raw = addr - text_addr;
	if (raw > UINT_MAX)
		return;

	record_seq_end(0, (unsigned int)raw);
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
				classify_seq_end(addr, text_addr, sections,
						 num_sections);
				continue;
			}

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

/* True if some sequence in @section ends in (@lo, @hi]. */
static bool seq_end_between(unsigned int section, unsigned int lo,
			    unsigned int hi)
{
	unsigned int low = 0, high = num_seq_ends;

	/* First index past (@section, @lo). */
	while (low < high) {
		unsigned int mid = low + (high - low) / 2;

		if (compare_sec_off(seq_ends[mid].section_id,
				    seq_ends[mid].offset, section, lo) <= 0)
			low = mid + 1;
		else
			high = mid;
	}

	return low < num_seq_ends && seq_ends[low].section_id == section &&
	       seq_ends[low].offset <= hi;
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

	qsort(seq_ends, num_seq_ends, sizeof(*seq_ends), compare_seq_ends);

	for (unsigned int i = 0; i < num_sym_starts; i++) {
		unsigned int sec = sym_starts[i].section_id;
		unsigned int start = sym_starts[i].offset;
		unsigned int end = start + sym_starts[i].size;

		/*
		 * Both arrays are sorted by (section_id, offset), so one
		 * forward cursor tracks the last entry at or below the symbol.
		 */
		while (cursor + 1 < base_entries &&
		       compare_sec_off(entries[cursor + 1].section_id,
				       entries[cursor + 1].offset,
				       sec, start) <= 0)
			cursor++;

		if (entries[cursor].section_id != sec)
			continue;	/* no entries in this section yet */
		if (entries[cursor].offset > start)
			continue;	/* nothing covers this symbol */
		if (entries[cursor].offset == start)
			continue;	/* already has its own entry */
		if (seq_end_between(sec, entries[cursor].offset, start))
			continue;	/* coverage stopped before here */

		/* Overflow, or no row inside [start, end): not our code. */
		if (end <= start)
			continue;
		if (cursor + 1 >= base_entries ||
		    entries[cursor + 1].section_id != sec ||
		    entries[cursor + 1].offset >= end)
			continue;

		add_entry(start, sec, entries[cursor].file_id,
			  entries[cursor].line);
	}
}

static void deduplicate(struct covered_section *sections,
			unsigned int num_sections)
{
	unsigned int sym_cursor = 0;
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

	synthesize_symbol_starts();
	qsort(entries, num_entries, sizeof(*entries), compare_entries);

	/*
	 * Remove duplicates.  Reset on a section_id boundary: the same offset
	 * can legitimately appear in two different sections (they all start
	 * at sh_addr 0 in ET_REL), and the "same as previous kept entry"
	 * collapse is only meaningful inside one section's binary-search
	 * domain.
	 *
	 * Within a section:
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

		if (entries[i].section_id != entries[j].section_id) {
			j++;
			if (j != i)
				entries[j] = entries[i];
			continue;
		}
		if (entries[i].offset == entries[j].offset)
			continue;

		while (sym_cursor < num_sym_starts &&
		       compare_sec_off(sym_starts[sym_cursor].section_id,
				       sym_starts[sym_cursor].offset,
				       entries[i].section_id,
				       entries[i].offset) < 0)
			sym_cursor++;
		at_symbol_start = sym_cursor < num_sym_starts &&
				  !compare_sec_off(sym_starts[sym_cursor].section_id,
						   sym_starts[sym_cursor].offset,
						   entries[i].section_id,
						   entries[i].offset);

		if (!at_symbol_start &&
		    entries[i].file_id == entries[j].file_id &&
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
	/*
	 * Align before defining the label, not after: the descriptor stores
	 * table_offset as .Lhdr - .Lroot, and every field offset inside the
	 * header is measured from .Lhdr too.  Emitting the label first binds
	 * it to the pre-padding address, so the kernel would read the header
	 * out of the padding bytes.
	 */
	printf("\t.balign 4\n");
	printf(".Lhdr%s:\n", suffix);
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
		printf("\t.long %u\n", files[i]->str_offset);

	/* filenames[] */
	printf(".Lfilenames%s:\n", suffix);
	for (unsigned int i = 0; i < num_files; i++)
		print_escaped_asciz(files[i]->name);
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

	if (argc >= 2 && !strcmp(argv[1], "--module")) {
		module_mode = 1;
		argv++;
		argc--;
	}

	if (argc != 2) {
		fprintf(stderr, "Usage: %s [-v] [--module] <ELF file>\n",
			argv[0]);
		return 1;
	}

	init_path_roots();

	fd = open(argv[1], O_RDONLY);
	if (fd < 0)
		error("cannot open %s: %s", argv[1], strerror(errno));

	elf_version(EV_CURRENT);
	/*
	 * Module mode patches line-program addresses in the in-memory ELF
	 * data and must never write them back.  A private copy-on-write
	 * mapping gives exactly that, so the .ko can stay open read-only.
	 */
	elf = elf_begin(fd, module_mode ? ELF_C_READ_MMAP_PRIVATE :
					  ELF_C_READ_MMAP, NULL);
	if (!elf)
		error("elf_begin failed: %s", elf_errmsg(elf_errno()));

	{
		GElf_Ehdr ehdr;

		if (gelf_getehdr(elf, &ehdr) == NULL)
			error("gelf_getehdr failed");
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

	collect_symbol_starts(elf, text_addr, all_sections, ALL_SECTIONS);

	dwarf = dwarf_begin_elf(elf, DWARF_C_READ, NULL);
	if (!dwarf)
		error("dwarf_begin_elf failed: %s\n"
		      LINEINFO_PREFIX "error: is %s built with CONFIG_DEBUG_INFO?",
		      dwarf_errmsg(dwarf_errno()), argv[1]);

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
		verbose_msg("persistent %u entries, init %u entries, %u files",
			    persistent_total, init_total, num_files);

		/*
		 * Expected for every module: .static_call.text and friends
		 * are deliberately not covered.
		 */
		if (skipped_uncovered)
			verbose_msg("%llu entries dropped (outside covered text sections)",
				    skipped_uncovered);

		if (skipped_overflow)
			warn("%u entries skipped (offset > 4 GiB)",
			     skipped_overflow);
	} else {
		process_dwarf(dwarf, text_addr, NULL, 0);

		if (skipped_overflow)
			warn("%u entries skipped (offset > 4 GiB from _text)",
			     skipped_overflow);

		deduplicate(NULL, 0);
		compute_file_offsets();

		verbose_msg("%u entries, %u files", num_entries, num_files);

		output_assembly();
	}

	dwarf_end(dwarf);
	elf_end(elf);
	close(fd);

	/* Cleanup */
	free(entries);
	free(sym_starts);
	free(seq_ends);
	free(extra_sections);
	for (unsigned int i = 0; i < num_files; i++)
		free(files[i]);
	free(files);
	for (unsigned int i = 0; i < num_path_roots; i++)
		free(path_roots[i].path);
	return 0;
}
