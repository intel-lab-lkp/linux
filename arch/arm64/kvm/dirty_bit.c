// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Ltd.
 * Author: Leonardo Bras <leo.bras@arm.com>
 */

#include <asm/kvm_dirty_bit.h>

/* HDBSS entry field definitions */
#define HDBSS_ENTRY_VALID BIT(0)
#define HDBSS_ENTRY_TTWL_SHIFT (1)
#define HDBSS_ENTRY_TTWL_MASK (GENMASK(3, 1))
#define HDBSS_ENTRY_TTWL(x) \
	(((x) << HDBSS_ENTRY_TTWL_SHIFT) & HDBSS_ENTRY_TTWL_MASK)
#define HDBSS_ENTRY_TTWL_RESV HDBSS_ENTRY_TTWL(-4)
#define HDBSS_ENTRY_IPA GENMASK_ULL(55, 12)

inline u64 hdbss_get_ttwl(u64 chunk_size)
{
	u64 hw_lvl = ARM64_HW_PGTABLE_LEVELS(ilog2(chunk_size));

	return HDBSS_ENTRY_TTWL(3 - hw_lvl);
}
