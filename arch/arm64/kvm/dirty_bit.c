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
