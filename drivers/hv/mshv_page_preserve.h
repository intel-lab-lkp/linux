/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Microsoft Corporation, Jork Loeser <jloeser@microsoft.com>
 */

#ifndef _MSHV_PAGE_PRESERVE_H
#define _MSHV_PAGE_PRESERVE_H

#include <linux/kho_radix_tree.h>

struct page;

int mshv_preserve_init(void);
int mshv_register_preserve_page(struct page *pg);
int mshv_unregister_preserve_page(struct page *pg);
int mshv_iterate_preserved(const struct kho_radix_walk_cb *cb, void *data);

#endif /* _MSHV_PAGE_PRESERVE_H */
