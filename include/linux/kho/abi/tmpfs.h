/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 */

#ifndef _LINUX_KHO_ABI_TMPFS_H
#define _LINUX_KHO_ABI_TMPFS_H

#include <linux/limits.h>
#include <linux/types.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/abi/memfd.h>

/**
 * DOC: tmpfs Live Update ABI
 *
 * tmpfs uses the ABI defined below for preserving a mount and the regular
 * files in it across a kexec reboot using the LUO.
 *
 * Regular files in the root of such a mount are preserved individually into
 * `struct tmpfs_luo_file_ser` and reference their mount by its LUO token. Only
 * the folios holding the file contents are true handover payload; they use the
 * memfd ABI (`struct memfd_luo_folio_ser`) unchanged.
 *
 * The mount metadata is preserved via `tmpfs_luo_mnt_ser`.
 *
 * This interface is a contract. Any modification to the structure layout
 * constitutes a breaking change. Such changes require incrementing the version
 * number in the corresponding compatible string.
 */

/**
 * struct tmpfs_luo_mnt_ser - Serialized state of a preserved tmpfs mount.
 * @max_blocks: The block limit of the filesystem in PAGE_SIZE units. 0 means
 *              unlimited.
 * @mode:       The mode of the root directory.
 * @flags:      Flags for the mount. Unused flag bits must be set to 0.
 *
 * Ownership is not preserved; the restored root directory belongs to whoever
 * retrieves the mount.
 */
struct tmpfs_luo_mnt_ser {
	u64 max_blocks;
	u32 mode;
	u32 flags;
} __packed;

/**
 * struct tmpfs_luo_file_ser - Serialized state of a preserved tmpfs file.
 * @mnt_token: The LUO token of the tmpfs mount this file lives in.
 * @pos:       The file's current position (f_pos).
 * @size:      The total size of the file in bytes (i_size).
 * @mode:      The permission bits of the file (i_mode). The file type is
 *             always S_IFREG.
 * @flags:     Flags for the file. Unused flag bits must be set to 0.
 * @nr_folios: Number of folios in the folios array.
 * @folios:    KHO vmalloc descriptor pointing to the array of
 *             struct memfd_luo_folio_ser.
 * @name:      The NUL-terminated name of the file in the root of the mount,
 *             This is a single path component: it never contains '/', is never
 *             "." or "..", and all bytes after the terminator must be 0.
 */
struct tmpfs_luo_file_ser {
	u64 mnt_token;
	u64 pos;
	u64 size;
	u32 mode;
	u32 flags;
	u64 nr_folios;
	struct kho_vmalloc folios;
	/* NAME_MAX is uAPI. */
	char name[NAME_MAX + 1];
} __packed;

/* The compatibility string for the tmpfs mount file handler */
#define TMPFS_LUO_MNT_FH_COMPATIBLE	"tmpfs-mnt-v1"

/* The compatibility string for the tmpfs file handler */
#define TMPFS_LUO_FILE_FH_COMPATIBLE	"tmpfs-file-v1"

#endif /* _LINUX_KHO_ABI_TMPFS_H */
