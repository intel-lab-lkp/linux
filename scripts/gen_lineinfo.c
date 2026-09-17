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
 * adding that bias to relocated values inside apply_debug_relocations().
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
	unsigned int seq;	/* line-program row order, breaks offset ties */
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

/* Monotonic row counter; see the seq tie-break in compare_entries(). */
static unsigned int next_seq;

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
	entries[num_entries].seq = next_seq++;
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
	/*
	 * Several rows routinely share one address: the line program emits a
	 * row per event, and every row but the last covers zero bytes.  Order
	 * them as the program emitted them so deduplicate() can keep the row
	 * that actually describes the instruction.  seq is unique, so the
	 * sort is total and the output is reproducible.
	 */
	if (ea->seq != eb->seq)
		return ea->seq < eb->seq ? -1 : 1;
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

/*
 * Every symbol beginning in covered text, whether or not it is a function.
 * Clang's basic-block sections (CONFIG_PROPELLER_CLANG) split a function
 * into parts that carry STT_NOTYPE labels and a line-program sequence each,
 * so resolve_cu_row_groups() cannot ask for STT_FUNC when it needs to know
 * whether a sequence can begin at an address.
 */
static struct sym_start *text_starts;
static unsigned int num_text_starts;
static unsigned int text_starts_capacity;

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

/* Sort by (section, offset), keeping only the widest symbol at each. */
static void sort_starts(struct sym_start *a, unsigned int *count)
{
	unsigned int n = *count, j = 0;

	if (n < 2)
		return;

	qsort(a, n, sizeof(*a), compare_sym_starts);
	for (unsigned int i = 1; i < n; i++) {
		if (!compare_sec_off(a[i].section_id, a[i].offset,
				     a[j].section_id, a[j].offset))
			continue;
		if (++j != i)
			a[j] = a[i];
	}
	*count = j + 1;
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
			switch (GELF_ST_TYPE(sym.st_info)) {
			case STT_FUNC:
			case STT_NOTYPE:
				break;
			default:
				continue;
			}

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
			if (num_text_starts >= text_starts_capacity) {
				text_starts_capacity = text_starts_capacity ?
					text_starts_capacity * 2 : 16384;
				text_starts = xrealloc(text_starts,
						       text_starts_capacity *
						       sizeof(*text_starts));
			}
			text_starts[num_text_starts].section_id = sec_id;
			text_starts[num_text_starts].offset = (unsigned int)raw;
			text_starts[num_text_starts].size =
				sym.st_size > UINT_MAX ? UINT_MAX :
				(unsigned int)sym.st_size;
			num_text_starts++;

			if (GELF_ST_TYPE(sym.st_info) != STT_FUNC)
				continue;

			sym_starts[num_sym_starts].section_id = sec_id;
			sym_starts[num_sym_starts].offset = (unsigned int)raw;
			sym_starts[num_sym_starts].size =
				sym.st_size > UINT_MAX ? UINT_MAX :
				(unsigned int)sym.st_size;
			num_sym_starts++;
		}
	}

	sort_starts(sym_starts, &num_sym_starts);
	sort_starts(text_starts, &num_text_starts);

	/*
	 * An unsized label reaches the next symbol, and a sequence beginning
	 * there cannot run past it.
	 */
	for (unsigned int i = 1; i < num_text_starts; i++) {
		if (text_starts[i].section_id != text_starts[i - 1].section_id)
			continue;
		if (!text_starts[i - 1].size)
			text_starts[i - 1].size = text_starts[i].offset -
						  text_starts[i - 1].offset;
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
/*
 * Constants for the newer architectures are missing from older host ELF
 * headers -- glibc only gained the LoongArch definitions in 2.36 -- and
 * gen_lineinfo is built whenever CONFIG_KALLSYMS_LINEINFO is set, on every
 * architecture.  Same guarded-definition pattern as scripts/mod/modpost.c.
 */
#ifndef EM_RISCV
#define EM_RISCV		243
#endif
#ifndef R_RISCV_32
#define R_RISCV_32		1
#endif
#ifndef R_RISCV_64
#define R_RISCV_64		2
#endif
#ifndef R_RISCV_ADD8
#define R_RISCV_ADD8		33
#endif
#ifndef R_RISCV_ADD16
#define R_RISCV_ADD16		34
#endif
#ifndef R_RISCV_ADD32
#define R_RISCV_ADD32		35
#endif
#ifndef R_RISCV_ADD64
#define R_RISCV_ADD64		36
#endif
#ifndef R_RISCV_SUB8
#define R_RISCV_SUB8		37
#endif
#ifndef R_RISCV_SUB16
#define R_RISCV_SUB16		38
#endif
#ifndef R_RISCV_SUB32
#define R_RISCV_SUB32		39
#endif
#ifndef R_RISCV_SUB64
#define R_RISCV_SUB64		40
#endif
#ifndef EM_LOONGARCH
#define EM_LOONGARCH		258
#endif
#ifndef R_LARCH_32
#define R_LARCH_32		1
#endif
#ifndef R_LARCH_64
#define R_LARCH_64		2
#endif
#ifndef R_LARCH_ADD8
#define R_LARCH_ADD8		47
#endif
#ifndef R_LARCH_ADD16
#define R_LARCH_ADD16		48
#endif
#ifndef R_LARCH_ADD32
#define R_LARCH_ADD32		50
#endif
#ifndef R_LARCH_ADD64
#define R_LARCH_ADD64		51
#endif
#ifndef R_LARCH_SUB8
#define R_LARCH_SUB8		52
#endif
#ifndef R_LARCH_SUB16
#define R_LARCH_SUB16		53
#endif
#ifndef R_LARCH_SUB32
#define R_LARCH_SUB32		55
#endif
#ifndef R_LARCH_SUB64
#define R_LARCH_SUB64		56
#endif

/*
 * MIPS n64 stores r_info as a symbol index followed by three extra
 * relocation bytes rather than one integer, so the generic GELF_R_*
 * macros decode it as nonsense: a type-2 relocation against symbol 9
 * reads back as type 9 against symbol 33554432.  Mirrors
 * get_rel_type_and_sym() in scripts/mod/modpost.c.
 */
static void decode_r_info(const GElf_Ehdr *ehdr, GElf_Xword r_info,
			  unsigned int *r_type, size_t *r_sym)
{
	const unsigned int endian_probe = 1;
	bool host_le = *(const unsigned char *)&endian_probe;
	bool target_le = ehdr->e_ident[EI_DATA] == ELFDATA2LSB;
	unsigned char raw[sizeof(r_info)];

	if (ehdr->e_machine != EM_MIPS ||
	    ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
		*r_type = GELF_R_TYPE(r_info);
		*r_sym = GELF_R_SYM(r_info);
		return;
	}

	/*
	 * libelf byte-swaps r_info as one 64-bit quantity when the object's
	 * endianness differs from ours; undo that to get the field layout
	 * back before picking it apart.
	 */
	memcpy(raw, &r_info, sizeof(raw));
	if (target_le != host_le) {
		unsigned char tmp;

		for (size_t i = 0; i < sizeof(raw) / 2; i++) {
			tmp = raw[i];
			raw[i] = raw[sizeof(raw) - 1 - i];
			raw[sizeof(raw) - 1 - i] = tmp;
		}
	}

	*r_type = raw[7];
	if (target_le)
		*r_sym = (size_t)raw[0] | (size_t)raw[1] << 8 |
			 (size_t)raw[2] << 16 | (size_t)raw[3] << 24;
	else
		*r_sym = (size_t)raw[3] | (size_t)raw[2] << 8 |
			 (size_t)raw[1] << 16 | (size_t)raw[0] << 24;
}

/*
 * Some ABIs express a label difference, such as a line program's address
 * advance, as a pair of arithmetic relocations rather than one absolute
 * value: RISC-V and LoongArch both do, and clang emits them for
 * .debug_line even with -mno-relax.  Each applies S + A to the value
 * already stored at the target.  Returns the width in bytes, or 0 if
 * @r_type is not an arithmetic relocation; *@is_sub says which direction.
 */
static size_t arith_reloc_width(unsigned int e_machine, unsigned int r_type,
				bool *is_sub)
{
	static const struct {
		unsigned int machine;
		unsigned int add;
		unsigned int sub;
		size_t width;
	} tbl[] = {
		{ EM_RISCV,	R_RISCV_ADD8,	R_RISCV_SUB8,	1 },
		{ EM_RISCV,	R_RISCV_ADD16,	R_RISCV_SUB16,	2 },
		{ EM_RISCV,	R_RISCV_ADD32,	R_RISCV_SUB32,	4 },
		{ EM_RISCV,	R_RISCV_ADD64,	R_RISCV_SUB64,	8 },
		{ EM_LOONGARCH,	R_LARCH_ADD8,	R_LARCH_SUB8,	1 },
		{ EM_LOONGARCH,	R_LARCH_ADD16,	R_LARCH_SUB16,	2 },
		{ EM_LOONGARCH,	R_LARCH_ADD32,	R_LARCH_SUB32,	4 },
		{ EM_LOONGARCH,	R_LARCH_ADD64,	R_LARCH_SUB64,	8 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(tbl); i++) {
		if (tbl[i].machine != e_machine)
			continue;
		if (r_type == tbl[i].add) {
			*is_sub = false;
			return tbl[i].width;
		}
		if (r_type == tbl[i].sub) {
			*is_sub = true;
			return tbl[i].width;
		}
	}
	return 0;
}

/* Relocations we could not apply; the caller refuses to emit a table. */
static unsigned int unhandled_relocs;

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
 * Apply one relocation to a debug section.  Two reloc widths matter:
 *   abs32 - section-offset refs: DW_FORM_line_strp file-table entries into
 *           .debug_line_str, and DW_AT_stmt_list, DW_AT_abbrev_offset and
 *           DW_FORM_strp refs out of .debug_info
 *   abs64 - DW_LNE_set_address arguments (sequence start PCs)
 * Without both, libdw sees zeros: it reports wrong filenames, collapses
 * every sequence to address 0 (collision after dedup), or decodes every
 * compile unit against the first one's abbrev table and line program.
 *
 * @has_addend distinguishes RELA records (explicit @addend) from REL
 * records, whose addend is read from the relocated field itself.
 * @bias folds in the covered section's synthetic bias for text-directed
 * relocations; only .debug_line addresses reach classify_address().
 */
static void apply_one_debug_reloc(Elf_Data *dl_data, Elf_Data *sym_data,
				  bool target_le, const GElf_Ehdr *ehdr,
				  unsigned int abs32_type,
				  unsigned int abs64_type, GElf_Xword r_info,
				  GElf_Addr r_offset, GElf_Sxword addend,
				  bool has_addend, bool bias)
{
	GElf_Sym sym;
	unsigned int r_type;
	size_t r_sym;
	bool is_abs64 = false;
	bool is_sub = false;
	size_t arith_width;
	size_t width;
	uint64_t value;

	decode_r_info(ehdr, r_info, &r_type, &r_sym);
	arith_width = arith_reloc_width(ehdr->e_machine, r_type, &is_sub);

	if (abs32_type && r_type == abs32_type)
		is_abs64 = false;
	else if (abs64_type && r_type == abs64_type)
		is_abs64 = true;
	else if (arith_width && has_addend)
		;	/* handled below */
	else if (r_type)	/* type 0 is R_*_NONE everywhere we support */
		unhandled_relocs++;

	if (!(abs32_type && r_type == abs32_type) &&
	    !(abs64_type && r_type == abs64_type) &&
	    !(arith_width && has_addend))
		return;

	if (!gelf_getsym(sym_data, r_sym, &sym))
		return;

	if (arith_width) {
		uint64_t cur;

		if (r_offset + arith_width > dl_data->d_size)
			return;
		/*
		 * A delta, not an address: the section bias would appear in
		 * the ADD and cancel in the matching SUB, so leave it out.
		 */
		cur = elf_read_uint((unsigned char *)dl_data->d_buf + r_offset,
				    arith_width, target_le);
		value = (uint64_t)(sym.st_value + addend);
		cur = is_sub ? cur - value : cur + value;
		elf_write_uint((unsigned char *)dl_data->d_buf + r_offset,
			       cur, arith_width, target_le);
		return;
	}

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
	if (module_mode && bias) {
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

/* Walk one .rela.<debug section> / .rel.<debug section> table, if present. */
static void apply_debug_reloc_table(Elf_Scn *scn, bool is_rela,
				    Elf_Data *dl_data, Elf_Data *sym_data,
				    bool target_le, const GElf_Ehdr *ehdr,
				    unsigned int abs32_type,
				    unsigned int abs64_type, bool bias)
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
			apply_one_debug_reloc(dl_data, sym_data, target_le,
					      ehdr, abs32_type, abs64_type,
					      rela.r_info, rela.r_offset,
					      rela.r_addend, true, bias);
		} else {
			GElf_Rel rel;

			if (!gelf_getrel(data, i, &rel))
				continue;
			apply_one_debug_reloc(dl_data, sym_data, target_le,
					      ehdr, abs32_type, abs64_type,
					      rel.r_info, rel.r_offset,
					      0, false, bias);
		}
	}
}

/*
 * Debug sections libdw reads while decoding compile units and their line
 * programs.  In an ET_REL .ko every reference out of these sections is a
 * relocation: a CU's DW_AT_abbrev_offset and DW_AT_stmt_list, DW_FORM_strp
 * names, DW_FORM_line_strp file-table entries and DW_LNE_set_address PCs.
 * libdw applies none of them, so without this pass every CU but the first
 * -- whose offsets genuinely are zero -- decodes against the first CU's
 * abbrev table and line program, and a module built from several objects
 * gets line info for only the first one.
 */
static const struct {
	const char *name;
	bool bias;
} debug_reloc_sections[] = {
	{ ".debug_line",		true },
	{ ".debug_info",		false },
	{ ".debug_str_offsets",		false },
	{ ".debug_addr",		false },
	{ ".debug_rnglists",		false },
	{ ".debug_loclists",		false },
};

static Elf_Scn *find_scn_by_name(Elf *elf, size_t shstrndx, const char *want)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		const char *name;

		if (!gelf_getshdr(scn, &shdr))
			continue;
		name = elf_strptr(elf, shstrndx, shdr.sh_name);
		if (name && !strcmp(name, want))
			return scn;
	}
	return NULL;
}

static Elf_Scn *find_symtab_scn(Elf *elf)
{
	Elf_Scn *scn = NULL;
	GElf_Shdr shdr;

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		if (gelf_getshdr(scn, &shdr) && shdr.sh_type == SHT_SYMTAB)
			return scn;
	}
	return NULL;
}

static void apply_debug_relocations(Elf *elf)
{
	Elf_Scn *symtab_scn;
	GElf_Ehdr ehdr;
	GElf_Shdr shdr;
	unsigned int abs32_type, abs64_type;
	bool target_le;
	size_t shstrndx;
	Elf_Data *sym_data;

	if (gelf_getehdr(elf, &ehdr) == NULL)
		return;

	abs32_type = r_type_abs32(ehdr.e_machine);
	abs64_type = r_type_abs64(ehdr.e_machine);
	if (!abs32_type && !abs64_type)
		error("no known absolute relocation type for ELF machine %u; refusing to emit line info from unrelocated DWARF",
		      ehdr.e_machine);
	target_le = (ehdr.e_ident[EI_DATA] == ELFDATA2LSB);

	if (elf_getshdrstrndx(elf, &shstrndx) != 0)
		return;

	symtab_scn = find_symtab_scn(elf);
	if (!symtab_scn)
		return;
	sym_data = elf_getdata(symtab_scn, NULL);
	if (!sym_data)
		return;

	for (size_t i = 0; i < ARRAY_SIZE(debug_reloc_sections); i++) {
		const char *name = debug_reloc_sections[i].name;
		bool bias = debug_reloc_sections[i].bias;
		char relname[64];
		Elf_Scn *dbg_scn, *rela_scn, *rel_scn;
		Elf_Data *dbg_data;

		dbg_scn = find_scn_by_name(elf, shstrndx, name);
		if (!dbg_scn)
			continue;

		/*
		 * CONFIG_DEBUG_INFO_COMPRESSED_* hands us SHF_COMPRESSED
		 * sections.  elf_getdata() would return the compressed bytes
		 * while every relocation offset addresses the uncompressed
		 * contents, so patching without expanding first corrupts the
		 * stream and libdw then reads nothing.  Expand in place;
		 * libdw sees the same expanded data afterwards.
		 */
		if (gelf_getshdr(dbg_scn, &shdr) &&
		    (shdr.sh_flags & SHF_COMPRESSED) &&
		    elf_compress(dbg_scn, 0, 0) < 0)
			error("cannot decompress %s: %s", name,
			      elf_errmsg(elf_errno()));

		dbg_data = elf_getdata(dbg_scn, NULL);
		if (!dbg_data)
			continue;

		snprintf(relname, sizeof(relname), ".rela%s", name);
		rela_scn = find_scn_by_name(elf, shstrndx, relname);
		snprintf(relname, sizeof(relname), ".rel%s", name);
		rel_scn = find_scn_by_name(elf, shstrndx, relname);

		/*
		 * RELA (64-bit ELF and most 32-bit targets) carries explicit
		 * addends; REL (i386, arm32, ...) stores the addend in the
		 * relocated field itself.
		 */
		apply_debug_reloc_table(rela_scn, true, dbg_data, sym_data,
					target_le, &ehdr, abs32_type,
					abs64_type, bias);
		apply_debug_reloc_table(rel_scn, false, dbg_data, sym_data,
					target_le, &ehdr, abs32_type,
					abs64_type, bias);

		/*
		 * Anything left unapplied would shift every mapping after it.
		 * Refuse the object rather than emit line numbers that point
		 * at the wrong source lines.
		 */
		if (unhandled_relocs)
			error("%s: %u unsupported relocation(s); refusing to emit line info",
			      name, unhandled_relocs);
	}
}

/*
 * Decide which covered_section a (biased) DWARF address belongs to.
 * apply_debug_relocations() has already added the section's bias to
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

/*
 * One flag per row read from the compile unit being processed: whether the
 * row closed a line-program sequence.  libdw returns rows sorted by address,
 * so the flag is the only thing that tells the two kinds of line-0 row apart.
 */
static bool *cu_row_seq_end;
static size_t cu_row_seq_end_cap;

static void cu_rows_reserve(size_t rows)
{
	if (rows <= cu_row_seq_end_cap)
		return;
	cu_row_seq_end = xrealloc(cu_row_seq_end,
				  rows * sizeof(*cu_row_seq_end));
	cu_row_seq_end_cap = rows;
}

/* Size of the symbol starting exactly at (@section, @offset), 0 if none. */
static unsigned int symbol_extent_at(unsigned int section, unsigned int offset)
{
	unsigned int low = 0, high = num_text_starts;

	while (low < high) {
		unsigned int mid = low + (high - low) / 2;
		int cmp = compare_sec_off(text_starts[mid].section_id,
					  text_starts[mid].offset,
					  section, offset);

		if (cmp < 0)
			low = mid + 1;
		else if (cmp > 0)
			high = mid;
		else
			return text_starts[mid].size;
	}

	return 0;
}

/*
 * True if the rows a unit has where one of its sequences ended open a new
 * sequence there, rather than closing the one that ended.  libdw cannot say:
 * both look like an end_sequence followed by a normal row at one address.
 *
 * A sequence can only begin at a function, and a unit that begins one goes
 * on to describe that function -- with a row inside it, or, for a function
 * the unit covers in a single row, with the sequence end that closes it.
 * Neither holds for the padding between two functions, which carries a
 * symbol of its own that no unit describes.
 */
static bool starts_new_sequence(unsigned int group, unsigned int next,
				unsigned int start)
{
	unsigned long long end;
	unsigned int size;

	size = symbol_extent_at(entries[group].section_id,
				entries[group].offset);
	if (!size)
		return false;

	end = (unsigned long long)entries[group].offset + size;

	for (unsigned int t = next; t < num_entries; t++) {
		if (entries[t].section_id != entries[group].section_id)
			break;
		if (entries[t].offset > end)
			break;
		if (entries[t].offset < end || cu_row_seq_end[t - start])
			return true;
	}

	return false;
}

/*
 * Collapse the rows one compile unit emitted at a single address, of which
 * all but one cover zero bytes.
 *
 * Rows that are not sequence ends keep their line-program order here, so the
 * last of them is the one that describes the address, line 0 included: such a
 * row declares that no source location applies, which is what addr2line
 * reports for it too.
 *
 * The row closing a sequence is harder.  libdw sorts rows by address and
 * puts an end_sequence ahead of any normal row sharing that address, which
 * hides the difference between a sequence whose last row lands on its own
 * end, covering nothing, and a sequence that begins where another ended.
 * The symbol table settles it, see starts_new_sequence().
 */
static void resolve_cu_row_groups(unsigned int start)
{
	unsigned int i = start, j = start;

	while (i < num_entries) {
		unsigned int k = i, normal = i, seq_end = i, keep;
		bool have_normal = false, have_seq_end = false;

		while (k < num_entries &&
		       entries[k].section_id == entries[i].section_id &&
		       entries[k].offset == entries[i].offset) {
			if (cu_row_seq_end[k - start]) {
				seq_end = k;
				have_seq_end = true;
			} else {
				normal = k;
				have_normal = true;
			}
			k++;
		}

		if (!have_normal)
			keep = seq_end;
		else if (!have_seq_end)
			keep = normal;
		else if (starts_new_sequence(i, k, start))
			keep = normal;
		else
			keep = seq_end;

		cu_row_seq_end[j - start] = cu_row_seq_end[keep - start];
		entries[j++] = entries[keep];
		i = k;
	}

	num_entries = j;
}

/*
 * Address spans described by an assembler compile unit.  A single .loc there
 * covers a whole macro expansion, which can emit a function per invocation,
 * so a row from before a symbol is the row that describes it.  In C the same
 * shape means padding or a split-out fragment carrying a neighbour's line,
 * which is why synthesize_symbol_starts() asks for this.
 */
struct asm_span {
	unsigned int section_id;
	unsigned int lo;
	unsigned int hi;
};

static struct asm_span *asm_spans;
static unsigned int num_asm_spans;
static unsigned int asm_spans_capacity;

static int compare_asm_spans(const void *a, const void *b)
{
	const struct asm_span *sa = a, *sb = b;

	return compare_sec_off(sa->section_id, sa->lo, sb->section_id, sb->lo);
}

/* True if an assembler unit describes (@section, @offset). */
static bool in_asm_span(unsigned int section, unsigned int offset)
{
	unsigned int low = 0, high = num_asm_spans;

	while (low < high) {
		unsigned int mid = low + (high - low) / 2;

		if (compare_sec_off(asm_spans[mid].section_id,
				    asm_spans[mid].lo, section, offset) <= 0)
			low = mid + 1;
		else
			high = mid;
	}

	return low && asm_spans[low - 1].section_id == section &&
	       offset <= asm_spans[low - 1].hi;
}

/*
 * One span per sequence this assembler unit contributed.  A unit's code lands
 * in scattered places once linked, so a span covering the whole unit would
 * claim whatever sits between.  @first_seq_end is where this unit's sequence
 * ends begin, needed because a sequence reaching the end of the covered range
 * has its closing row dropped and leaves no marker to stop at.
 */
static void record_cu_asm_spans(unsigned int start, unsigned int first_seq_end)
{
	unsigned int i = start;

	while (i < num_entries) {
		unsigned int sec = entries[i].section_id, j = i, hi;

		while (!cu_row_seq_end[j - start] && j + 1 < num_entries &&
		       entries[j + 1].section_id == sec)
			j++;

		hi = entries[j].offset;
		if (!cu_row_seq_end[j - start]) {
			for (unsigned int t = first_seq_end; t < num_seq_ends;
			     t++) {
				if (seq_ends[t].section_id != sec ||
				    seq_ends[t].offset <= hi)
					continue;
				if (hi == entries[j].offset ||
				    seq_ends[t].offset < hi)
					hi = seq_ends[t].offset;
			}
		}

		if (num_asm_spans >= asm_spans_capacity) {
			asm_spans_capacity = asm_spans_capacity ?
				asm_spans_capacity * 2 : 256;
			asm_spans = xrealloc(asm_spans, asm_spans_capacity *
						       sizeof(*asm_spans));
		}
		asm_spans[num_asm_spans].section_id = sec;
		asm_spans[num_asm_spans].lo = entries[i].offset;
		asm_spans[num_asm_spans].hi = hi;
		num_asm_spans++;
		i = j + 1;
	}
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
		unsigned int cu_first_entry, cu_first_seq_end;

		if (!dwarf_offdie(dwarf, off + hdr_size, &cudie))
			goto next;

		if (dwarf_attr(&cudie, DW_AT_comp_dir, &attr))
			comp_dir = dwarf_formstring(&attr);

		if (dwarf_getsrclines(&cudie, &lines, &nlines) != 0)
			goto next;

		cu_first_entry = num_entries;
		cu_first_seq_end = num_seq_ends;
		cu_rows_reserve(nlines);

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
			 * Record the boundary, which deduplicate() needs to
			 * tell "this row still covers the next symbol" from
			 * "coverage stopped here", and emit a line-0 entry so
			 * that a lookup in the gap before the next sequence
			 * reports nothing instead of inheriting the line
			 * above it.
			 */
			if (dwarf_lineendsequence(line, &endseq) == 0 && endseq) {
				classify_seq_end(addr, text_addr, sections,
						 num_sections);
				lineno = 0;
			} else if (dwarf_lineno(line, &lineno) != 0) {
				continue;
			}

			/*
			 * Line 0 means "no source location applies from here"
			 * (DWARF5 6.2.2).  Keep such a row rather than drop
			 * it: without it the preceding entry covers the
			 * interval, and an address the compiler explicitly
			 * declined to attribute gets a confident, wrong line.
			 * It needs no file of its own, since the lookup side
			 * reads the zero line as "no annotation" and stops
			 * before it looks one up.
			 */
			src = dwarf_linesrc(line, NULL, NULL);
			if (!src && lineno)
				continue;

			if (module_mode) {
				/*
				 * In ET_REL .ko files .text/.init.text/.exit.text
				 * all share sh_addr == 0; classify_address picks
				 * the right bucket from the explicit ranges we
				 * captured.  An end_sequence row addresses the
				 * byte after the sequence, which for a sequence
				 * reaching the end of its section is the first
				 * byte of whichever section got the next bias.
				 * Resolve those from the last byte they cover.
				 */
				if (endseq && !addr)
					continue;
				sec_id = classify_address(sections, num_sections,
							  endseq ? addr - 1 : addr,
							  &sec_off);
				if (sec_id == num_sections) {
					skipped_uncovered++;
					continue;
				}
				if (endseq)
					sec_off++;
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

			if (src) {
				rel = make_relative(src, comp_dir);
				file_id = find_or_add_file(rel);
			} else {
				file_id = 0;
			}

			add_entry(loffset, sec_id, file_id, (unsigned int)lineno);
			cu_row_seq_end[num_entries - 1 - cu_first_entry] = endseq;
		}

		resolve_cu_row_groups(cu_first_entry);

		/*
		 * DW_LANG_Mips_Assembler is what gas stamps on a .S unit, and
		 * what tells synthesize_symbol_starts() that a row reaching
		 * over a symbol describes it rather than covering padding.
		 */
		if (dwarf_srclang(&cudie) == DW_LANG_Mips_Assembler)
			record_cu_asm_spans(cu_first_entry, cu_first_seq_end);
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
	qsort(asm_spans, num_asm_spans, sizeof(*asm_spans), compare_asm_spans);

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

		if (end <= start)
			continue;	/* size overflowed */

		/*
		 * A row reaching over a symbol with none inside it is padding
		 * or a split-out fragment in C, carrying the line of whatever
		 * it came from.  In assembly it is one macro expanding to a
		 * function per invocation, which the row does describe.
		 */
		if (!in_asm_span(sec, start) &&
		    (cursor + 1 >= base_entries ||
		     entries[cursor + 1].section_id != sec ||
		     entries[cursor + 1].offset >= end))
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
	 * - Same offset: keep last.  Earlier rows at that address cover no
	 *   bytes, so the final one is what describes the instruction, and
	 *   it is what addr2line reports.  For an inlined call that is the
	 *   inlined body rather than the call site.
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
		if (entries[i].offset == entries[j].offset) {
			/*
			 * Rows from different compile units can share an
			 * address, and coverage is their union: a real
			 * location beats another unit's no-coverage marker.
			 * Within one unit resolve_cu_row_groups() has already
			 * picked the row that describes the address.
			 */
			if (entries[i].line || !entries[j].line)
				entries[j] = entries[i];
			continue;
		}

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
		 * sections FIRST so apply_debug_relocations() can use the
		 * assigned biases when patching line-program addresses;
		 * libdw does NOT apply relocations for ET_REL files, so we
		 * patch every debug section it reads.
		 */
		resolve_covered_sections(elf, all_sections, ALL_SECTIONS);
		apply_debug_relocations(elf);
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
