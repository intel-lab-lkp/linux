// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Multikernel Technologies, Inc. All rights reserved
 */
#ifndef _LINUX_MULTIKERNEL_H
#define _LINUX_MULTIKERNEL_H

struct resource;

extern phys_addr_t multikernel_alloc(size_t size);
extern void multikernel_free(phys_addr_t addr, size_t size);
extern struct resource *multikernel_get_pool_resource(void);
extern bool multikernel_pool_available(void);

/* Per-instance memory pool management */
extern void *multikernel_create_instance_pool(int instance_id, size_t pool_size, int min_alloc_order);
extern void multikernel_destroy_instance_pool(void *pool_handle);
extern phys_addr_t multikernel_instance_alloc(void *pool_handle, size_t size, size_t align);
extern void multikernel_instance_free(void *pool_handle, phys_addr_t addr, size_t size);
extern size_t multikernel_instance_pool_avail(void *pool_handle);

#endif /* _LINUX_MULTIKERNEL_H */
