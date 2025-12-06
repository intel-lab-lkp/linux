/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <pratyush@kernel.org>
 */

#ifndef _LINUX_KHO_ABI_HUGETLB_H
#define _LINUX_KHO_ABI_HUGETLB_H

#include <linux/hugetlb.h>
#include <linux/kexec_handover.h>

/**
 * DOC: hugetlb-backed memfd live update ABI
 *
 * This header defines the ABI for preserving the state of the hugetlb subsystem
 * and a hugetlb-backed memfd across a kexec reboot using LUO.
 *
 * This interface is a contract. Any modification to the structure layout
 * constitutes a breaking change. Such changes require incrementing the version
 * number in the HUGETLB_FLB_COMPATIBLE or HUGE_MEMFD_COMPATIBLE strings for
 * hugetlb FLB or hugetlb-backed memfd, respectively.
 */

/*
 * Keep the serialized max hstates separate from the kernel's HUGE_MAX_HSTATE to
 * keep the value stable.
 *
 * Currently x86 and arm64 are supported. x86 has HUGE_MAX_HSTATE as 2 and arm64
 * has 4. Pick 4 as the number to start with.
 */
#define HUGETLB_SER_MAX_HSTATES		4

static_assert(HUGETLB_SER_MAX_HSTATES >= HUGE_MAX_HSTATE);

/**
 * struct hugetlb_hstate_ser: Serialized state of a hstate.
 * @nr_pages:     Number of preserved pages in the hstate.
 * @order:        Order of the hstate this struct describes.
 *
 * The only state needed for hstates is the number of pages that are preserved
 * from this hstate. The preserved pages are added to the hstate when the file
 * is retrieved. This information gets used in early boot to calculate the
 * remaining pages that must be allocated by the normal path.
 */
struct hugetlb_hstate_ser {
	/* Number of _preserved_ pages in the hstate. */
	u64 nr_pages;
	u8 order;
} __packed;

/**
 * struct hugetlb_ser - The main serialization structure for HugeTLB FLB.
 * @hstates:      Array of serialized hstates.
 * @nr_hstates:   Number of serialized hstates in the array.
 */
struct hugetlb_ser {
	struct hugetlb_hstate_ser hstates[HUGETLB_SER_MAX_HSTATES];
	u8 nr_hstates;
} __packed;

static_assert(sizeof(struct hugetlb_ser) <= PAGE_SIZE);

#define HUGETLB_FLB_COMPATIBLE "hugetlb-v1"

/**
 * struct hugemfd_folio_ser - Serialized state of a single folio.
 * @pfn:          The page frame number of the folio.
 * @reserved:     Reserved bits. Might be used for flags later.
 * @index:        The page offset of the folio in the original file.
 */
struct hugemfd_folio_ser {
	u64 pfn:52;
	u64 reserved:12;
	u64 index;
} __packed;

/**
 * struct hugemfd_ser - Main serialization structure of a HugeTLB-backed memfd.
 * @pos:          The file's current position (f_pos).
 * @size:         The total size of the file in bytes (i_size).
 * @nr_folios:    Number of folios in the folios array.
 * @folios:       KHO vmalloc descriptor pointing to the array of
 *                struct hugemfd_folio_ser.
 * @order:        Order of the hugepages that back this file.
 */
struct hugemfd_ser {
	u64 size;
	u64 pos;
	u64 nr_folios;
	struct kho_vmalloc folios;
	u8 order;
} __packed;

#define HUGE_MEMFD_COMPATIBLE "huge-memfd-v1"

#endif /* _LINUX_KHO_ABI_HUGETLB_H */
