/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2013 - 2018 Intel Corporation. */

#ifndef _I40E_ALLOC_H_
#define _I40E_ALLOC_H_

#include <linux/types.h>

struct i40e_hw;

/* Memory allocation types */
enum i40e_memory_type {
	i40e_mem_arq_buf = 0,		/* ARQ indirect command buffer */
	i40e_mem_asq_buf = 1,
	i40e_mem_atq_buf = 2,		/* ATQ indirect command buffer */
	i40e_mem_arq_ring = 3,		/* ARQ descriptor ring */
	i40e_mem_atq_ring = 4,		/* ATQ descriptor ring */
	i40e_mem_pd = 5,		/* Page Descriptor */
	i40e_mem_bp = 6,		/* Backing Page - 4KB */
	i40e_mem_bp_jumbo = 7,		/* Backing Page - > 4KB */
	i40e_mem_reserved
};

/* memory allocation tracking */
struct i40e_dma_mem {
	void *va;
	dma_addr_t pa;
	u32 size;
};

struct i40e_virt_mem {
	void *va;
	u32 size;
};

#define i40e_allocate_dma_mem(h, m, unused, s, a) \
			i40e_allocate_dma_mem_d(h, m, s, a)
#define i40e_free_dma_mem(h, m) i40e_free_dma_mem_d(h, m)

#define i40e_allocate_virt_mem(h, m, s) i40e_allocate_virt_mem_d(h, m, s)
#define i40e_free_virt_mem(h, m) i40e_free_virt_mem_d(h, m)

/* prototype for functions used for dynamic memory allocation */
int i40e_allocate_dma_mem(struct i40e_hw *hw,
			  struct i40e_dma_mem *mem,
			  enum i40e_memory_type type,
			  u64 size, u32 alignment);
int i40e_free_dma_mem(struct i40e_hw *hw,
		      struct i40e_dma_mem *mem);
int i40e_allocate_virt_mem(struct i40e_hw *hw,
			   struct i40e_virt_mem *mem,
			   u32 size);
int i40e_free_virt_mem(struct i40e_hw *hw,
		       struct i40e_virt_mem *mem);

#endif /* _I40E_ALLOC_H_ */
