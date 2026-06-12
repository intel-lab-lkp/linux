/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Virtual swap space
 *
 * Copyright (C) 2026 Nhat Pham
 */
#ifndef _MM_VSWAP_H
#define _MM_VSWAP_H

#include <linux/swap.h>

#ifdef CONFIG_VSWAP

extern struct swap_info_struct *vswap_si;

static inline bool swap_is_vswap(struct swap_info_struct *si)
{
	return si->flags & SWP_VSWAP;
}

#else

static inline bool swap_is_vswap(struct swap_info_struct *si)
{
	return false;
}

#endif /* CONFIG_VSWAP */

#ifdef CONFIG_SWAP
#include "swap.h"
static inline bool is_vswap_entry(swp_entry_t entry)
{
	return swap_is_vswap(__swap_entry_to_info(entry));
}
#else
static inline bool is_vswap_entry(swp_entry_t entry)
{
	return false;
}
#endif /* CONFIG_SWAP */

#endif /* _MM_VSWAP_H */
