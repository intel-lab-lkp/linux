/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generalized Memory Management.
 *
 * Copyright (C) 2023- Huawei, Inc.
 * Author: Weixi Zhu
 *
 */
#ifndef _GMEM_H
#define _GMEM_H

#ifdef CONFIG_GMEM
/* h-NUMA topology */
void __init hnuma_init(void);
#else
static inline void hnuma_init(void) {}
#endif

#endif /* _GMEM_H */
