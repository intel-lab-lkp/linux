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
#else
#include <stdint.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
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
};

/*
 * Top-level header.  Sits at offset 0 of every .mod_lineinfo /
 * .init.mod_lineinfo section.  The compiler inserts 4 bytes of trailing
 * padding so the u64 anchor in sections[0] starts 8-byte aligned.
 */
struct mod_lineinfo_root {
	u32 num_sections;
	struct mod_lineinfo_section sections[];
};

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
