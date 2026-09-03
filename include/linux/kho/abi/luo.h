/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

/**
 * DOC: Live Update Orchestrator ABI
 *
 * Live Update Orchestrator uses the stable Application Binary Interface
 * defined below to pass state from a pre-update kernel to a post-update
 * kernel. The ABI is built upon the Kexec HandOver framework and registers
 * the central `struct luo_ser` via the KHO raw subtree API.
 *
 * This interface is a contract. To ensure the stability of moving between
 * kernel versions, any changes to the structures should only be additive
 * and should utilize the feature flags to denote the supported, active, and
 * required status of the feature.
 *
 * Features that are entirely optional can be added with the supported and
 * active flags both asserted, and the required flag cleared. Features that
 * require that the next kernel supports the feature should only be added as
 * supported to allow compatible transition kernels. At a later time, the
 * active and required flag should be asserted.
 *
 *
 * KHO Structure Overview:
 *   The entire LUO state is encapsulated within a single KHO entry named "LUO".
 *   This entry contains the `struct luo_ser` structure.
 *
 * Serialization Structures:
 *   - struct luo_ser:
 *     The central ABI structure that contains the overall state of the LUO.
 *     It includes the feature flags, the liveupdate-number, and pointers
 *     to sessions and FLBs.
 *
 *   - struct luo_session_ser:
 *     Metadata for a single session, including its name and a physical pointer
 *     to the first `struct kho_block_header_ser` for all files in that session.
 *     Multiple blocks are linked via the `next` field in the header.
 *
 *   - struct luo_file_ser:
 *     Metadata for a single preserved file. Contains the `compatible` string to
 *     find the correct handler in the new kernel, a user-provided `token` for
 *     identification, and an opaque `data` handle for the handler to use.
 *
 *   - struct luo_flb_header_ser:
 *     Header for the FLB array. Contains the total page count of the
 *     preserved memory block and the number of `struct luo_flb_ser` entries
 *     that follow.
 *
 *   - struct luo_flb_ser:
 *     Metadata for a single preserved global object. Contains its `name`
 *     (compatible string), an opaque `data` handle, and the `count`
 *     number of files depending on it.
 */

#ifndef _LINUX_KHO_ABI_LUO_H
#define _LINUX_KHO_ABI_LUO_H

#include <linux/align.h>
#include <linux/bits.h>
#include <linux/kho/abi/block.h>
#include <uapi/linux/liveupdate.h>

/**
 * struct luo_feature_hdr - Feature negotiation and versioning header.
 * @supp:     Bitmask of features supported by the producing subsystem.
 * @req:      Bitmask of features required for safe deserialization by the
 *            receiving subsystem.
 * @active:   Bitmask of features actually active/used in the serialized payload.
 * @reserved: Reserved for future use.
 *
 * This header can be embedded at the beginning of any serialized structure to
 * provide forward and backward compatibility via feature negotiation across
 * live update.
 */
struct luo_feature_hdr {
	u64 supp;
	u64 req;
	u64 active;
	u64 reserved[13];
} __packed;

#define LUO_FEATURE_ACTIVE(s, f)		\
	do {					\
		(s)->features.supp |= (u64)(f);	\
		(s)->features.active |= (u64)(f);	\
	} while (0)
#define LUO_FEATURE_SUPPORTED(s, f)	((s)->features.supp |= (u64)(f))
#define LUO_FEATURE_REQUIRED(s, f)	((s)->features.req |= (u64)(f))
#define LUO_FEATURE_IS_ACTIVE(s, f)	(!!((s)->features.active & (u64)(f)))
#define LUO_FEATURE_IS_SUPPORTED(s, f)	(!!((s)->features.supp & (u64)(f)))
#define LUO_FEATURE_IS_REQUIRED(s, f)	(!!((s)->features.req & (u64)(f)))

/*
 * The LUO state is registered under this KHO entry name.
 */
#define LUO_KHO_ENTRY_NAME	"LUO"

/*
 * Bits associated with the luo_feature_hdr values.
 */
#define LUO_FEATURE_NUMBER    BIT_ULL(0)
#define LUO_FEATURE_SESSIONS  BIT_ULL(1)
#define LUO_FEATURE_FLBS      BIT_ULL(2)

#define LUO_CORE_FEATURES_SUPP		(LUO_FEATURE_NUMBER | \
					 LUO_FEATURE_SESSIONS | \
					 LUO_FEATURE_FLBS)
#define LUO_CORE_FEATURES_REQ		(LUO_FEATURE_NUMBER | \
					 LUO_FEATURE_SESSIONS | \
					 LUO_FEATURE_FLBS)
#define LUO_CORE_FEATURES_ACTIVE	(LUO_FEATURE_NUMBER | \
					 LUO_FEATURE_SESSIONS | \
					 LUO_FEATURE_FLBS)

/**
 * struct luo_ser - Centralized LUO ABI header.
 * @features:       Bit mask of supported and active features.
 * @liveupdate_num: A counter tracking the number of successful live updates.
 * @sessions_pa:    Physical address of the first session block header.
 * @flbs_pa:        Physical address of the FLB header.
 *
 * This structure is the root of all preserved LUO state.
 */
struct luo_ser {
	struct luo_feature_hdr features;
	u64 liveupdate_num;
	u64 sessions_pa;
	u64 flbs_pa;
} __packed;

#define LIVEUPDATE_HNDL_NAME_LENGTH	48

/**
 * struct luo_file_ser - Represents the serialized preserves files.
 * @name:        File handler name.
 * @data:        Private data
 * @token:       User provided token for this file
 *
 * If this structure is modified, `LUO_ABI_COMPATIBLE` must be updated.
 */
struct luo_file_ser {
	char name[LIVEUPDATE_HNDL_NAME_LENGTH];
	u64 data;
	u64 token;
} __packed;

/**
 * struct luo_file_set_ser - Represents the serialized metadata for file set
 * @files:   The physical address of the first `struct kho_block_header_ser`.
 *           This structure is the header for a block of memory containing
 *           an array of `struct luo_file_ser` entries. Multiple blocks are
 *           linked via the `next` field in the header.
 * @count:   The total number of files that were part of this session during
 *           serialization. Used for iteration and validation during
 *           restoration.
 */
struct luo_file_set_ser {
	u64 files;
	u64 count;
} __packed;

/**
 * struct luo_session_ser - Represents the serialized metadata for a LUO session.
 * @name:         The unique name of the session, provided by the userspace at
 *                the time of session creation.
 * @file_set_ser: Serialized files belonging to this session,
 *
 * This structure is used to package session-specific metadata for transfer
 * between kernels via Kexec Handover. An array of these structures (one per
 * session) is created and passed to the new kernel, allowing it to reconstruct
 * the session context.
 *
 * If this structure is modified, `LUO_ABI_COMPATIBLE` must be updated.
 */
struct luo_session_ser {
	char name[LIVEUPDATE_SESSION_NAME_LENGTH];
	struct luo_file_set_ser file_set_ser;
} __packed;

/* The max size is set so it can be reliably used during in serialization */
#define LIVEUPDATE_FLB_COMPAT_LENGTH	48

/**
 * struct luo_flb_header_ser - Header for the serialized FLB data block.
 * @pgcnt: The total number of pages occupied by the entire preserved memory
 *         region, including this header and the subsequent array of
 *         &struct luo_flb_ser entries.
 * @count: The number of &struct luo_flb_ser entries that follow this header
 *         in the memory block.
 *
 * This structure is located at the physical address specified by the
 * flbs_pa in luo_ser.
 *
 * If this structure is modified, `LUO_ABI_COMPATIBLE` must be updated.
 */
struct luo_flb_header_ser {
	u64 pgcnt;
	u64 count;
} __packed;

/**
 * struct luo_flb_ser - Represents the serialized state of a single FLB object.
 * @name:    The unique compatibility string of the FLB object, used to find the
 *           corresponding &struct liveupdate_flb handler in the new kernel.
 * @data:    The opaque u64 handle returned by the FLB's .preserve() operation
 *           in the old kernel. This handle encapsulates the entire state needed
 *           for restoration.
 * @count:   The reference count at the time of serialization; i.e., the number
 *           of preserved files that depended on this FLB. This is used by the
 *           new kernel to correctly manage the FLB's lifecycle.
 *
 * An array of these structures is created in a preserved memory region and
 * passed to the new kernel. Each entry allows the LUO core to restore one
 * global, shared object.
 *
 * If this structure is modified, `LUO_ABI_COMPATIBLE` must be updated.
 */
struct luo_flb_ser {
	char name[LIVEUPDATE_FLB_COMPAT_LENGTH];
	u64 data;
	u64 count;
} __packed;

/* Kernel Live Update Test ABI */
#ifdef CONFIG_LIVEUPDATE_TEST
#define LIVEUPDATE_TEST_FLB_COMPATIBLE(i)	"liveupdate-test-flb-v" #i
#endif

#define LIVEUPDATE_VER_HDR_MAGIC	0x4c565550 /* 'LVUP' */
#define LIVEUPDATE_VER_HDR_VER		1

/**
 * struct liveupdate_ver_hdr - Header of vmlinux section with version lists
 * @magic:     Magic number ('LVUP').
 * @version:   Version of the header format.
 *
 * This struct is the header for the vmlinux section ".liveupdate_features". The
 * section contains the list of feature/version entries that the kernel supports.
 */
struct liveupdate_ver_hdr {
	u32 magic;
	u32 version;
} __packed;

/**
 * struct liveupdate_feature_entry - Live update feature/version entry
 * @name:       Name of the subsystem or feature ("luo" for core).
 * @feat_bytes: Number of bytes covered by supp, req, and active bitmaps (e.g. 8).
 * @reserved:   Reserved / padding for alignment.
 * @supp:       Bitmask of supported features.
 * @req:        Bitmask of required features.
 * @active:     Bitmask of active features.
 */
struct liveupdate_feature_entry {
	char name[LIVEUPDATE_HNDL_NAME_LENGTH];
	u32 feat_bytes;
	u32 reserved;
	u64 supp;
	u64 req;
	u64 active;
} __packed;

#endif /* _LINUX_KHO_ABI_LUO_H */
