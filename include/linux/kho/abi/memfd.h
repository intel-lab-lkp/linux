/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

#ifndef _LINUX_KHO_ABI_MEMFD_H
#define _LINUX_KHO_ABI_MEMFD_H

#include <linux/bits.h>
#include <linux/kho/abi/luo.h>
#include <linux/types.h>
#include <linux/kho/abi/kexec_handover.h>

/**
 * DOC: memfd Live Update ABI
 *
 * memfd uses the ABI defined below for preserving its state across a kexec
 * reboot using the LUO.
 *
 * The state is serialized into a packed structure `struct memfd_luo_ser`
 * which is handed over to the next kernel via the KHO mechanism.
 *
 * This interface is a contract. Any changes should be additive using feature
 * flags to ensure backwards compatibility.
 */

#define MEMFD_LUO_FEATURE_SEALS		BIT_ULL(0)
#define MEMFD_LUO_FEATURE_FOLIOS	BIT_ULL(1)

#define MEMFD_LUO_FEATURES_SUPP		(MEMFD_LUO_FEATURE_SEALS | \
					 MEMFD_LUO_FEATURE_FOLIOS)
#define MEMFD_LUO_FEATURES_REQ		(MEMFD_LUO_FEATURE_SEALS | \
					 MEMFD_LUO_FEATURE_FOLIOS)
#define MEMFD_LUO_FEATURES_ACTIVE	(MEMFD_LUO_FEATURE_SEALS | \
					 MEMFD_LUO_FEATURE_FOLIOS)

/**
 * MEMFD_LUO_FOLIO_DIRTY - The folio is dirty.
 *
 * This flag indicates the folio contains data from user. A non-dirty folio is
 * one that was allocated (say using fallocate(2)) but not written to.
 */
#define MEMFD_LUO_FOLIO_DIRTY		BIT(0)

/**
 * MEMFD_LUO_FOLIO_UPTODATE - The folio is up-to-date.
 *
 * An up-to-date folio has been zeroed out. shmem zeroes out folios on first
 * use. This flag tracks which folios need zeroing.
 */
#define MEMFD_LUO_FOLIO_UPTODATE	BIT(1)

/**
 * struct memfd_luo_folio_ser - Serialized state of a single folio.
 * @pfn:       The page frame number of the folio.
 * @flags:     Flags to describe the state of the folio.
 * @index:     The page offset (pgoff_t) of the folio within the original file.
 */
struct memfd_luo_folio_ser {
	u64 pfn:52;
	u64 flags:12;
	u64 index;
} __packed;

/*
 * The set of base seals supported by MEMFD_LUO_FEATURE_SEALS.
 * If support for new seals is needed, define a dedicated feature bit
 * (e.g. MEMFD_LUO_FEATURE_SEAL_<NAME>) to allow granular compatibility.
 */
#define MEMFD_LUO_BASE_SEALS	(F_SEAL_SEAL | \
				 F_SEAL_SHRINK | \
				 F_SEAL_GROW | \
				 F_SEAL_WRITE | \
				 F_SEAL_FUTURE_WRITE | \
				 F_SEAL_EXEC)
#define MEMFD_LUO_ALL_SEALS	MEMFD_LUO_BASE_SEALS

/**
 * struct memfd_luo_ser - Main serialization structure for a memfd.
 * @features:  Bit mask of supported, required, and active features.
 * @pos:       The file's current position (f_pos).
 * @size:      The total size of the file in bytes (i_size).
 * @seals:     The seals present on the memfd. The seals are uABI so it is safe
 *             to directly use them in the ABI.
 * @flags:     Flags for the file. Unused flag bits must be set to 0.
 * @nr_folios: Number of folios in the folios array.
 * @folios:    KHO vmalloc descriptor pointing to the array of
 *             struct memfd_luo_folio_ser.
 */
struct memfd_luo_ser {
	struct luo_feature_hdr features;
	u64 pos;
	u64 size;
	u32 seals;
	u32 flags;
	u64 nr_folios;
	struct kho_vmalloc folios;
} __packed;

/* The name for memfd file handler */
#define MEMFD_LUO_FH_NAME	"memfd"

#endif /* _LINUX_KHO_ABI_MEMFD_H */
