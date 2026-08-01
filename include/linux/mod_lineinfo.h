/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mod_lineinfo.h - Binary format for per-module source line information
 *
 * This header defines the layout of the .mod_lineinfo and
 * .init.mod_lineinfo sections embedded in loadable kernel modules.  It
 * is dual-use: included from both the kernel and the userspace
 * gen_lineinfo tool.
 *
 * Top-level layout (all values in target-native endianness):
 *
 *   struct mod_lineinfo_root
 *   struct mod_lineinfo_section sections[hdr.num_sections]
 *   ... per-section sub-tables, each pointed at by sections[i].table_offset
 *
 * Each mod_lineinfo_section descriptor identifies one ELF text section
 * covered by the lineinfo blob.  Its .anchor field is an ELF relocation
 * resolved at module-load time to the runtime base of the named section,
 * eliminating the need to derive the base from mod->mem[].base segments.
 * If the relocation fails to resolve (e.g. unknown reloc type), .anchor
 * stays zero and lookups silently degrade to "no annotation".
 *
 * Each per-section sub-table is laid out as a stand-alone
 * mod_lineinfo_header followed by parallel arrays:
 *
 *   struct mod_lineinfo_header     (16 bytes)
 *   u32 addrs[num_entries]         -- offsets from this section's base, sorted
 *   u16 file_ids[num_entries]      -- parallel to addrs
 *   <2-byte pad if num_entries is odd>
 *   u32 lines[num_entries]         -- parallel to addrs
 *   u32 file_offsets[num_files]    -- byte offset into filenames[]
 *   char filenames[filenames_size] -- concatenated NUL-terminated strings
 */
#ifndef _LINUX_MOD_LINEINFO_H
#define _LINUX_MOD_LINEINFO_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/build_bug.h>
#include <linux/stddef.h>
#else
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
#ifndef __aligned
#define __aligned(x)	__attribute__((__aligned__(x)))
#endif
#ifndef static_assert
#define static_assert(e, ...)	_Static_assert(e, #e)
#endif
#endif

/*
 * Per-section descriptor.  One entry per ELF text section covered by the
 * blob (.text, .exit.text, .init.text, ...).
 */
struct mod_lineinfo_section {
	u64 anchor;		/* RELOC: runtime base of covered section, or 0 */
	u32 size;		/* covered section size in bytes */
	u32 table_offset;	/* byte offset from blob start to this section's
				 * mod_lineinfo_header */
} __aligned(8);

/*
 * Top-level header.  Sits at offset 0 of every .mod_lineinfo /
 * .init.mod_lineinfo section.  The generated assembly pads to an 8-byte
 * boundary after num_sections, so sections[0] must start at offset 8.
 * The __aligned(8) on struct mod_lineinfo_section guarantees that even on
 * 32-bit targets where the natural alignment of u64 is smaller (4 on i386,
 * 2 on m68k) and the compiler would otherwise place sections[] at offset 4.
 */
struct mod_lineinfo_root {
	u32 num_sections;
	struct mod_lineinfo_section sections[];
};

static_assert(offsetof(struct mod_lineinfo_root, sections) == 8,
	      "blob layout: sections[] must sit at offset 8 to match the generated assembly");
static_assert(sizeof(struct mod_lineinfo_section) == 16,
	      "blob layout: section descriptors are 16 bytes in the generated assembly");

struct mod_lineinfo_header {
	u32 num_entries;
	u32 num_files;
	u32 filenames_size;	/* total bytes of concatenated filenames */
};

/* Offset helpers: compute byte offset from the per-section header to each array. */

static inline u32 mod_lineinfo_addrs_off(void)
{
	return sizeof(struct mod_lineinfo_header);
}

static inline u32 mod_lineinfo_file_ids_off(u32 num_entries)
{
	return mod_lineinfo_addrs_off() + num_entries * sizeof(u32);
}

static inline u32 mod_lineinfo_lines_off(u32 num_entries)
{
	/* u16 file_ids[] may need 2-byte padding to align lines[] to 4 bytes */
	u32 off = mod_lineinfo_file_ids_off(num_entries) +
		  num_entries * sizeof(u16);
	return (off + 3) & ~3u;
}

static inline u32 mod_lineinfo_file_offsets_off(u32 num_entries)
{
	return mod_lineinfo_lines_off(num_entries) + num_entries * sizeof(u32);
}

static inline u32 mod_lineinfo_filenames_off(u32 num_entries, u32 num_files)
{
	return mod_lineinfo_file_offsets_off(num_entries) +
	       num_files * sizeof(u32);
}

#endif /* _LINUX_MOD_LINEINFO_H */
