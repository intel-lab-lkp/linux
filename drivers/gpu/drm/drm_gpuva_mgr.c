// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *     Danilo Krummrich <dakr@redhat.com>
 *
 */

#include <drm/drm_gpuva_mgr.h>

#include <linux/interval_tree_generic.h>
#include <linux/mm.h>

/**
 * DOC: Overview
 *
 * The DRM GPU VA Manager, represented by struct drm_gpuva_manager keeps track
 * of a GPU's virtual address (VA) space and manages the corresponding virtual
 * mappings represented by &drm_gpuva objects. It also keeps track of the
 * mapping's backing &drm_gem_object buffers.
 *
 * &drm_gem_object buffers maintain a list of &drm_gpuva objects representing
 * all existent GPU VA mappings using this &drm_gem_object as backing buffer.
 *
 * GPU VAs can be flagged as sparse, such that drivers may use GPU VAs to also
 * keep track of sparse PTEs in order to support Vulkan 'Sparse Resources'.
 *
 * The GPU VA manager internally uses a rb-tree to manage the
 * &drm_gpuva mappings within a GPU's virtual address space.
 *
 * The &drm_gpuva_manager contains a special &drm_gpuva representing the
 * portion of VA space reserved by the kernel. This node is initialized together
 * with the GPU VA manager instance and removed when the GPU VA manager is
 * destroyed.
 *
 * In a typical application drivers would embed struct drm_gpuva_manager and
 * struct drm_gpuva within their own driver specific structures, there won't be
 * any memory allocations of its own nor memory allocations of &drm_gpuva
 * entries.
 *
 * The data structures needed to store &drm_gpuvas within the &drm_gpuva_manager
 * are contained within struct drm_gpuva already. Hence, for inserting
 * &drm_gpuva entries from within dma-fence signalling critical sections it is
 * enough to pre-allocate the &drm_gpuva structures.
 */

/**
 * DOC: Split and Merge
 *
 * Besides its capability to manage and represent a GPU VA space, the
 * &drm_gpuva_manager also provides functions to let the &drm_gpuva_manager
 * calculate a sequence of operations to satisfy a given map or unmap request.
 *
 * Therefore the DRM GPU VA manager provides an algorithm implementing splitting
 * and merging of existent GPU VA mappings with the ones that are requested to
 * be mapped or unmapped. This feature is required by the Vulkan API to
 * implement Vulkan 'Sparse Memory Bindings' - drivers UAPIs often refer to this
 * as VM BIND.
 *
 * Drivers can call drm_gpuva_sm_map() to receive a sequence of callbacks
 * containing map, unmap and remap operations for a given newly requested
 * mapping. The sequence of callbacks represents the set of operations to
 * execute in order to integrate the new mapping cleanly into the current state
 * of the GPU VA space.
 *
 * Depending on how the new GPU VA mapping intersects with the existent mappings
 * of the GPU VA space the &drm_gpuva_fn_ops callbacks contain an arbitrary
 * amount of unmap operations, a maximum of two remap operations and a single
 * map operation. The caller might receive no callback at all if no operation is
 * required, e.g. if the requested mapping already exists in the exact same way.
 *
 * The single map operation represents the original map operation requested by
 * the caller.
 *
 * &drm_gpuva_op_unmap contains a 'keep' field, which indicates whether the
 * &drm_gpuva to unmap is physically contiguous with the original mapping
 * request. Optionally, if 'keep' is set, drivers may keep the actual page table
 * entries for this &drm_gpuva, adding the missing page table entries only and
 * update the &drm_gpuva_manager's view of things accordingly.
 *
 * Drivers may do the same optimization, namely delta page table updates, also
 * for remap operations. This is possible since &drm_gpuva_op_remap consists of
 * one unmap operation and one or two map operations, such that drivers can
 * derive the page table update delta accordingly.
 *
 * Note that there can't be more than two existent mappings to split up, one at
 * the beginning and one at the end of the new mapping, hence there is a
 * maximum of two remap operations.
 *
 * Analogous to drm_gpuva_sm_map() drm_gpuva_sm_unmap() uses &drm_gpuva_fn_ops
 * to call back into the driver in order to unmap a range of GPU VA space. The
 * logic behind this function is way simpler though: For all existent mappings
 * enclosed by the given range unmap operations are created. For mappings which
 * are only partically located within the given range, remap operations are
 * created such that those mappings are split up and re-mapped partically.
 *
 * As an alternative to drm_gpuva_sm_map() and drm_gpuva_sm_unmap(),
 * drm_gpuva_sm_map_ops_create() and drm_gpuva_sm_unmap_ops_create() can be used
 * to directly obtain an instance of struct drm_gpuva_ops containing a list of
 * &drm_gpuva_op, which can be iterated with drm_gpuva_for_each_op(). This list
 * contains the &drm_gpuva_ops analogous to the callbacks one would receive when
 * calling drm_gpuva_sm_map() or drm_gpuva_sm_unmap(). While this way requires
 * more memory (to allocate the &drm_gpuva_ops), it provides drivers a way to
 * iterate the &drm_gpuva_op multiple times, e.g. once in a context where memory
 * allocations are possible (e.g. to allocate GPU page tables) and once in the
 * dma-fence signalling critical path.
 *
 * To update the &drm_gpuva_manager's view of the GPU VA space
 * drm_gpuva_insert() and drm_gpuva_remove() may be used. These functions can
 * safely be used from &drm_gpuva_fn_ops callbacks originating from
 * drm_gpuva_sm_map() or drm_gpuva_sm_unmap(). However, it might be more
 * convenient to use the provided helper functions drm_gpuva_map(),
 * drm_gpuva_remap() and drm_gpuva_unmap() instead.
 *
 * The following diagram depicts the basic relationships of existent GPU VA
 * mappings, a newly requested mapping and the resulting mappings as implemented
 * by drm_gpuva_sm_map() - it doesn't cover any arbitrary combinations of these.
 *
 * 1) Requested mapping is identical. Replace it, but indicate the backing PTEs
 *    could be kept.
 *
 *    ::
 *
 *	     0     a     1
 *	old: |-----------| (bo_offset=n)
 *
 *	     0     a     1
 *	req: |-----------| (bo_offset=n)
 *
 *	     0     a     1
 *	new: |-----------| (bo_offset=n)
 *
 *
 * 2) Requested mapping is identical, except for the BO offset, hence replace
 *    the mapping.
 *
 *    ::
 *
 *	     0     a     1
 *	old: |-----------| (bo_offset=n)
 *
 *	     0     a     1
 *	req: |-----------| (bo_offset=m)
 *
 *	     0     a     1
 *	new: |-----------| (bo_offset=m)
 *
 *
 * 3) Requested mapping is identical, except for the backing BO, hence replace
 *    the mapping.
 *
 *    ::
 *
 *	     0     a     1
 *	old: |-----------| (bo_offset=n)
 *
 *	     0     b     1
 *	req: |-----------| (bo_offset=n)
 *
 *	     0     b     1
 *	new: |-----------| (bo_offset=n)
 *
 *
 * 4) Existent mapping is a left aligned subset of the requested one, hence
 *    replace the existent one.
 *
 *    ::
 *
 *	     0  a  1
 *	old: |-----|       (bo_offset=n)
 *
 *	     0     a     2
 *	req: |-----------| (bo_offset=n)
 *
 *	     0     a     2
 *	new: |-----------| (bo_offset=n)
 *
 *    .. note::
 *       We expect to see the same result for a request with a different BO
 *       and/or non-contiguous BO offset.
 *
 *
 * 5) Requested mapping's range is a left aligned subset of the existent one,
 *    but backed by a different BO. Hence, map the requested mapping and split
 *    the existent one adjusting its BO offset.
 *
 *    ::
 *
 *	     0     a     2
 *	old: |-----------| (bo_offset=n)
 *
 *	     0  b  1
 *	req: |-----|       (bo_offset=n)
 *
 *	     0  b  1  a' 2
 *	new: |-----|-----| (b.bo_offset=n, a.bo_offset=n+1)
 *
 *    .. note::
 *       We expect to see the same result for a request with a different BO
 *       and/or non-contiguous BO offset.
 *
 *
 * 6) Existent mapping is a superset of the requested mapping. Split it up, but
 *    indicate that the backing PTEs could be kept.
 *
 *    ::
 *
 *	     0     a     2
 *	old: |-----------| (bo_offset=n)
 *
 *	     0  a  1
 *	req: |-----|       (bo_offset=n)
 *
 *	     0  a  1  a' 2
 *	new: |-----|-----| (a.bo_offset=n, a'.bo_offset=n+1)
 *
 *
 * 7) Requested mapping's range is a right aligned subset of the existent one,
 *    but backed by a different BO. Hence, map the requested mapping and split
 *    the existent one, without adjusting the BO offset.
 *
 *    ::
 *
 *	     0     a     2
 *	old: |-----------| (bo_offset=n)
 *
 *	           1  b  2
 *	req:       |-----| (bo_offset=m)
 *
 *	     0  a  1  b  2
 *	new: |-----|-----| (a.bo_offset=n,b.bo_offset=m)
 *
 *
 * 8) Existent mapping is a superset of the requested mapping. Split it up, but
 *    indicate that the backing PTEs could be kept.
 *
 *    ::
 *
 *	      0     a     2
 *	old: |-----------| (bo_offset=n)
 *
 *	           1  a  2
 *	req:       |-----| (bo_offset=n+1)
 *
 *	     0  a' 1  a  2
 *	new: |-----|-----| (a'.bo_offset=n, a.bo_offset=n+1)
 *
 *
 * 9) Existent mapping is overlapped at the end by the requested mapping backed
 *    by a different BO. Hence, map the requested mapping and split up the
 *    existent one, without adjusting the BO offset.
 *
 *    ::
 *
 *	     0     a     2
 *	old: |-----------|       (bo_offset=n)
 *
 *	           1     b     3
 *	req:       |-----------| (bo_offset=m)
 *
 *	     0  a  1     b     3
 *	new: |-----|-----------| (a.bo_offset=n,b.bo_offset=m)
 *
 *
 * 10) Existent mapping is overlapped by the requested mapping, both having the
 *     same backing BO with a contiguous offset. Indicate the backing PTEs of
 *     the old mapping could be kept.
 *
 *     ::
 *
 *	      0     a     2
 *	 old: |-----------|       (bo_offset=n)
 *
 *	            1     a     3
 *	 req:       |-----------| (bo_offset=n+1)
 *
 *	      0  a' 1     a     3
 *	 new: |-----|-----------| (a'.bo_offset=n, a.bo_offset=n+1)
 *
 *
 * 11) Requested mapping's range is a centered subset of the existent one
 *     having a different backing BO. Hence, map the requested mapping and split
 *     up the existent one in two mappings, adjusting the BO offset of the right
 *     one accordingly.
 *
 *     ::
 *
 *	      0        a        3
 *	 old: |-----------------| (bo_offset=n)
 *
 *	            1  b  2
 *	 req:       |-----|       (bo_offset=m)
 *
 *	      0  a  1  b  2  a' 3
 *	 new: |-----|-----|-----| (a.bo_offset=n,b.bo_offset=m,a'.bo_offset=n+2)
 *
 *
 * 12) Requested mapping is a contiguous subset of the existent one. Split it
 *     up, but indicate that the backing PTEs could be kept.
 *
 *     ::
 *
 *	      0        a        3
 *	 old: |-----------------| (bo_offset=n)
 *
 *	            1  a  2
 *	 req:       |-----|       (bo_offset=n+1)
 *
 *	      0  a' 1  a  2 a'' 3
 *	 old: |-----|-----|-----| (a'.bo_offset=n, a.bo_offset=n+1, a''.bo_offset=n+2)
 *
 *
 * 13) Existent mapping is a right aligned subset of the requested one, hence
 *     replace the existent one.
 *
 *     ::
 *
 *	            1  a  2
 *	 old:       |-----| (bo_offset=n+1)
 *
 *	      0     a     2
 *	 req: |-----------| (bo_offset=n)
 *
 *	      0     a     2
 *	 new: |-----------| (bo_offset=n)
 *
 *     .. note::
 *        We expect to see the same result for a request with a different bo
 *        and/or non-contiguous bo_offset.
 *
 *
 * 14) Existent mapping is a centered subset of the requested one, hence
 *     replace the existent one.
 *
 *     ::
 *
 *	            1  a  2
 *	 old:       |-----| (bo_offset=n+1)
 *
 *	      0        a       3
 *	 req: |----------------| (bo_offset=n)
 *
 *	      0        a       3
 *	 new: |----------------| (bo_offset=n)
 *
 *     .. note::
 *        We expect to see the same result for a request with a different bo
 *        and/or non-contiguous bo_offset.
 *
 *
 * 15) Existent mappings is overlapped at the beginning by the requested mapping
 *     backed by a different BO. Hence, map the requested mapping and split up
 *     the existent one, adjusting its BO offset accordingly.
 *
 *     ::
 *
 *	            1     a     3
 *	 old:       |-----------| (bo_offset=n)
 *
 *	      0     b     2
 *	 req: |-----------|       (bo_offset=m)
 *
 *	      0     b     2  a' 3
 *	 new: |-----------|-----| (b.bo_offset=m,a.bo_offset=n+2)
 */

/**
 * DOC: Locking
 *
 * Generally, the GPU VA manager does not take care of locking itself, it is
 * the drivers responsibility to take care about locking. Drivers might want to
 * protect the following operations: inserting, removing and iterating
 * &drm_gpuva objects as well as generating all kinds of operations, such as
 * split / merge or prefetch.
 *
 * The GPU VA manager also does not take care of the locking of the backing
 * &drm_gem_object buffers GPU VA lists by itself; drivers are responsible to
 * enforce mutual exclusion using either the GEMs dma_resv lock or alternatively
 * a driver specific external lock. For the latter see also
 * drm_gem_gpuva_set_lock().
 *
 * However, the GPU VA manager contains lockdep checks to ensure callers of its
 * API hold the corresponding lock whenever the &drm_gem_objects GPU VA list is
 * accessed by functions such as drm_gpuva_link() or drm_gpuva_unlink().
 */

/**
 * DOC: Examples
 *
 * This section gives two examples on how to let the DRM GPUVA Manager generate
 * &drm_gpuva_op in order to satisfy a given map or unmap request and how to
 * make use of them.
 *
 * The below code is strictly limited to illustrate the generic usage pattern.
 * To maintain simplicitly, it doesn't make use of any abstractions for common
 * code, different (asyncronous) stages with fence signalling critical paths,
 * any other helpers or error handling in terms of freeing memory and dropping
 * previously taken locks.
 *
 * 1) Obtain a list of &drm_gpuva_op to create a new mapping::
 *
 *	// Allocates a new &drm_gpuva.
 *	struct drm_gpuva * driver_gpuva_alloc(void);
 *
 *	// Typically drivers would embedd the &drm_gpuva_manager and &drm_gpuva
 *	// structure in individual driver structures and lock the dma-resv with
 *	// drm_exec or similar helpers.
 *	int driver_mapping_create(struct drm_gpuva_manager *mgr,
 *				  u64 addr, u64 range,
 *				  struct drm_gem_object *obj, u64 offset)
 *	{
 *		struct drm_gpuva_ops *ops;
 *		struct drm_gpuva_op *op
 *
 *		driver_lock_va_space();
 *		ops = drm_gpuva_sm_map_ops_create(mgr, addr, range,
 *						  obj, offset);
 *		if (IS_ERR(ops))
 *			return PTR_ERR(ops);
 *
 *		drm_gpuva_for_each_op(op, ops) {
 *			struct drm_gpuva *va;
 *
 *			switch (op->op) {
 *			case DRM_GPUVA_OP_MAP:
 *				va = driver_gpuva_alloc();
 *				if (!va)
 *					; // unwind previous VA space updates,
 *					  // free memory and unlock
 *
 *				driver_vm_map();
 *				drm_gpuva_map(mgr, va, &op->map);
 *				drm_gpuva_link(va);
 *
 *				break;
 *			case DRM_GPUVA_OP_REMAP: {
 *				struct drm_gpuva *prev = NULL, *next = NULL;
 *
 *				va = op->remap.unmap->va;
 *
 *				if (op->remap.prev) {
 *					prev = driver_gpuva_alloc();
 *					if (!prev)
 *						; // unwind previous VA space
 *						  // updates, free memory and
 *						  // unlock
 *				}
 *
 *				if (op->remap.next) {
 *					next = driver_gpuva_alloc();
 *					if (!next)
 *						; // unwind previous VA space
 *						  // updates, free memory and
 *						  // unlock
 *				}
 *
 *				driver_vm_remap();
 *				drm_gpuva_remap(prev, next, &op->remap);
 *
 *				drm_gpuva_unlink(va);
 *				if (prev)
 *					drm_gpuva_link(prev);
 *				if (next)
 *					drm_gpuva_link(next);
 *
 *				break;
 *			}
 *			case DRM_GPUVA_OP_UNMAP:
 *				va = op->unmap->va;
 *
 *				driver_vm_unmap();
 *				drm_gpuva_unlink(va);
 *				drm_gpuva_unmap(&op->unmap);
 *
 *				break;
 *			default:
 *				break;
 *			}
 *		}
 *		driver_unlock_va_space();
 *
 *		return 0;
 *	}
 *
 * 2) Receive a callback for each &drm_gpuva_op to create a new mapping::
 *
 *	struct driver_context {
 *		struct drm_gpuva_manager *mgr;
 *		struct drm_gpuva *new_va;
 *		struct drm_gpuva *prev_va;
 *		struct drm_gpuva *next_va;
 *	};
 *
 *	// ops to pass to drm_gpuva_manager_init()
 *	static const struct drm_gpuva_fn_ops driver_gpuva_ops = {
 *		.sm_step_map = driver_gpuva_map,
 *		.sm_step_remap = driver_gpuva_remap,
 *		.sm_step_unmap = driver_gpuva_unmap,
 *	};
 *
 *	// Typically drivers would embedd the &drm_gpuva_manager and &drm_gpuva
 *	// structure in individual driver structures and lock the dma-resv with
 *	// drm_exec or similar helpers.
 *	int driver_mapping_create(struct drm_gpuva_manager *mgr,
 *				  u64 addr, u64 range,
 *				  struct drm_gem_object *obj, u64 offset)
 *	{
 *		struct driver_context ctx;
 *		struct drm_gpuva_ops *ops;
 *		struct drm_gpuva_op *op;
 *		int ret = 0;
 *
 *		ctx.mgr = mgr;
 *
 *		ctx.new_va = kzalloc(sizeof(*ctx.new_va), GFP_KERNEL);
 *		ctx.prev_va = kzalloc(sizeof(*ctx.prev_va), GFP_KERNEL);
 *		ctx.next_va = kzalloc(sizeof(*ctx.next_va), GFP_KERNEL);
 *		if (!ctx.new_va || !ctx.prev_va || !ctx.next_va) {
 *			ret = -ENOMEM;
 *			goto out;
 *		}
 *
 *		driver_lock_va_space();
 *		ret = drm_gpuva_sm_map(mgr, &ctx, addr, range, obj, offset);
 *		driver_unlock_va_space();
 *
 *	out:
 *		kfree(ctx.new_va);
 *		kfree(ctx.prev_va);
 *		kfree(ctx.next_va);
 *		return ret;
 *	}
 *
 *	int driver_gpuva_map(struct drm_gpuva_op *op, void *__ctx)
 *	{
 *		struct driver_context *ctx = __ctx;
 *
 *		drm_gpuva_map(ctx->mgr, ctx->new_va, &op->map);
 *
 *		drm_gpuva_link(ctx->new_va);
 *
 *		// prevent the new GPUVA from being freed in
 *		// driver_mapping_create()
 *		ctx->new_va = NULL;
 *
 *		return 0;
 *	}
 *
 *	int driver_gpuva_remap(struct drm_gpuva_op *op, void *__ctx)
 *	{
 *		struct driver_context *ctx = __ctx;
 *
 *		drm_gpuva_remap(ctx->prev_va, ctx->next_va, &op->remap);
 *
 *		drm_gpuva_unlink(op->remap.unmap->va);
 *		kfree(op->remap.unmap->va);
 *
 *		if (op->remap.prev) {
 *			drm_gpuva_link(ctx->prev_va);
 *			ctx->prev_va = NULL;
 *		}
 *
 *		if (op->remap.next) {
 *			drm_gpuva_link(ctx->next_va);
 *			ctx->next_va = NULL;
 *		}
 *
 *		return 0;
 *	}
 *
 *	int driver_gpuva_unmap(struct drm_gpuva_op *op, void *__ctx)
 *	{
 *		drm_gpuva_unlink(op->unmap.va);
 *		drm_gpuva_unmap(&op->unmap);
 *		kfree(op->unmap.va);
 *
 *		return 0;
 *	}
 */

#define to_drm_gpuva(__node)	container_of((__node), struct drm_gpuva, rb.node)

#define GPUVA_START(node) ((node)->va.addr)
#define GPUVA_LAST(node) ((node)->va.addr + (node)->va.range - 1)

/* We do not actually use drm_gpuva_it_next(), tell the compiler to not complain
 * about this.
 */
INTERVAL_TREE_DEFINE(struct drm_gpuva, rb.node, u64, rb.__subtree_last,
		     GPUVA_START, GPUVA_LAST, static __maybe_unused,
		     drm_gpuva_it)

static int __drm_gpuva_insert(struct drm_gpuva_manager *mgr,
			      struct drm_gpuva *va);
static void __drm_gpuva_remove(struct drm_gpuva *va);

static bool
drm_gpuva_check_overflow(u64 addr, u64 range)
{
	u64 end;

	return WARN(check_add_overflow(addr, range, &end),
		    "GPUVA address limited to %zu bytes.\n", sizeof(end));
}

static bool
drm_gpuva_in_mm_range(struct drm_gpuva_manager *mgr, u64 addr, u64 range)
{
	u64 end = addr + range;
	u64 mm_start = mgr->mm_start;
	u64 mm_end = mm_start + mgr->mm_range;

	return addr >= mm_start && end <= mm_end;
}

static bool
drm_gpuva_in_kernel_node(struct drm_gpuva_manager *mgr, u64 addr, u64 range)
{
	u64 end = addr + range;
	u64 kstart = mgr->kernel_alloc_node.va.addr;
	u64 krange = mgr->kernel_alloc_node.va.range;
	u64 kend = kstart + krange;

	return krange && addr < kend && kstart < end;
}

static bool
drm_gpuva_range_valid(struct drm_gpuva_manager *mgr,
		      u64 addr, u64 range)
{
	return !drm_gpuva_check_overflow(addr, range) &&
	       drm_gpuva_in_mm_range(mgr, addr, range) &&
	       !drm_gpuva_in_kernel_node(mgr, addr, range);
}

/**
 * drm_gpuva_manager_init() - initialize a &drm_gpuva_manager
 * @mgr: pointer to the &drm_gpuva_manager to initialize
 * @drm: the drivers &drm_device
 * @name: the name of the GPU VA space
 * @start_offset: the start offset of the GPU VA space
 * @range: the size of the GPU VA space
 * @reserve_offset: the start of the kernel reserved GPU VA area
 * @reserve_range: the size of the kernel reserved GPU VA area
 * @ops: &drm_gpuva_fn_ops called on &drm_gpuva_sm_map / &drm_gpuva_sm_unmap
 *
 * The &drm_gpuva_manager must be initialized with this function before use.
 *
 * Note that @mgr must be cleared to 0 before calling this function. The given
 * &name is expected to be managed by the surrounding driver structures.
 */
void
drm_gpuva_manager_init(struct drm_gpuva_manager *mgr,
		       struct drm_device *drm,
		       const char *name,
		       u64 start_offset, u64 range,
		       u64 reserve_offset, u64 reserve_range,
		       const struct drm_gpuva_fn_ops *ops)
{
	mgr->rb.tree = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&mgr->rb.list);

	mt_init(&mgr->mt_ext);

	INIT_LIST_HEAD(&mgr->evict.list);
	spin_lock_init(&mgr->evict.lock);

	drm_gpuva_check_overflow(start_offset, range);
	mgr->mm_start = start_offset;
	mgr->mm_range = range;

	mgr->name = name ? name : "unknown";
	mgr->ops = ops;

	memset(&mgr->kernel_alloc_node, 0, sizeof(struct drm_gpuva));

	if (reserve_range) {
		mgr->kernel_alloc_node.va.addr = reserve_offset;
		mgr->kernel_alloc_node.va.range = reserve_range;

		if (likely(!drm_gpuva_check_overflow(reserve_offset,
						     reserve_range)))
			__drm_gpuva_insert(mgr, &mgr->kernel_alloc_node);
	}

	drm_gem_private_object_init(drm, &mgr->d_obj, 0);
	mgr->resv = mgr->d_obj.resv;
}
EXPORT_SYMBOL_GPL(drm_gpuva_manager_init);

/**
 * drm_gpuva_manager_destroy() - cleanup a &drm_gpuva_manager
 * @mgr: pointer to the &drm_gpuva_manager to clean up
 *
 * Note that it is a bug to call this function on a manager that still
 * holds GPU VA mappings.
 */
void
drm_gpuva_manager_destroy(struct drm_gpuva_manager *mgr)
{
	mgr->name = NULL;

	if (mgr->kernel_alloc_node.va.range)
		__drm_gpuva_remove(&mgr->kernel_alloc_node);

	WARN(!RB_EMPTY_ROOT(&mgr->rb.tree.rb_root),
	     "GPUVA tree is not empty, potentially leaking memory.\n");

	mtree_destroy(&mgr->mt_ext);
	WARN(!list_empty(&mgr->evict.list), "Evict list should be empty.\n");

	drm_gem_private_object_fini(&mgr->d_obj);
}
EXPORT_SYMBOL_GPL(drm_gpuva_manager_destroy);

/**
 * drm_gpuva_manager_prepare_objects() - prepare all assoiciated BOs
 * @mgr: the &drm_gpuva_manager
 * @num_fences: the amount of &dma_fences to reserve
 *
 * Calls drm_exec_prepare_obj() for all &drm_gem_objects the given
 * &drm_gpuva_manager contains mappings of.
 *
 * Drivers can obtain the corresponding &drm_exec instance through
 * DRM_GPUVA_EXEC(). It is the drivers responsibility to call drm_exec_init()
 * and drm_exec_fini() accordingly.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int
drm_gpuva_manager_prepare_objects(struct drm_gpuva_manager *mgr,
				  unsigned int num_fences)
{
	struct drm_exec *exec = DRM_GPUVA_EXEC(mgr);
	MA_STATE(mas, &mgr->mt_ext, 0, 0);
	union {
		void *ptr;
		uintptr_t cnt;
	} ref;
	int ret;

	ret = drm_exec_prepare_obj(exec, &mgr->d_obj, num_fences);
	if (ret)
		goto out;

	rcu_read_lock();
	mas_for_each(&mas, ref.ptr, ULONG_MAX) {
		struct drm_gem_object *obj;

		mas_pause(&mas);
		rcu_read_unlock();

		obj = (struct drm_gem_object *)(uintptr_t)mas.index;
		ret = drm_exec_prepare_obj(exec, obj, num_fences);
		if (ret)
			goto out;

		rcu_read_lock();
	}
	rcu_read_unlock();

out:
	return ret;
}
EXPORT_SYMBOL_GPL(drm_gpuva_manager_prepare_objects);

/**
 * drm_gpuva_manager_lock_extra() - lock all dma-resv of all assoiciated BOs
 * @mgr: the &drm_gpuva_manager
 * @fn: callback received by the driver to lock additional dma-resv
 * @priv: private driver data passed to @fn
 * @num_fences: the amount of &dma_fences to reserve
 * @interruptible: sleep interruptible if waiting
 *
 * Acquires all dma-resv locks of all &drm_gem_objects the given
 * &drm_gpuva_manager contains mappings of.
 *
 * Addionally, when calling this function the driver receives the given @fn
 * callback to lock additional dma-resv in the context of the
 * &drm_gpuva_managers &drm_exec instance. Typically, drivers would call
 * drm_exec_prepare_obj() from within this callback.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int
drm_gpuva_manager_lock_extra(struct drm_gpuva_manager *mgr,
			     int (*fn)(struct drm_gpuva_manager *mgr,
				       void *priv, unsigned int num_fences),
			     void *priv,
			     unsigned int num_fences,
			     bool interruptible)
{
	struct drm_exec *exec = DRM_GPUVA_EXEC(mgr);
	uint32_t flags;
	int ret;

	flags = interruptible ? DRM_EXEC_INTERRUPTIBLE_WAIT : 0 |
		DRM_EXEC_IGNORE_DUPLICATES;

	drm_exec_init(exec, flags);

	drm_exec_until_all_locked(exec) {
		ret = drm_gpuva_manager_prepare_objects(mgr, num_fences);
		drm_exec_retry_on_contention(exec);
		if (ret)
			goto err;

		if (fn) {
			ret = fn(mgr, priv, num_fences);
			drm_exec_retry_on_contention(exec);
			if (ret)
				goto err;
		}
	}

	return 0;

err:
	drm_exec_fini(exec);
	return ret;
}
EXPORT_SYMBOL_GPL(drm_gpuva_manager_lock_extra);

static int
fn_lock_array(struct drm_gpuva_manager *mgr, void *priv,
				unsigned int num_fences)
{
	struct {
		struct drm_gem_object **objs;
		unsigned int num_objs;
	} *args = priv;

	return drm_exec_prepare_array(DRM_GPUVA_EXEC(mgr), args->objs,
				      args->num_objs, num_fences);
}

/**
 * drm_gpuva_manager_lock_array() - lock all dma-resv of all assoiciated BOs
 * @mgr: the &drm_gpuva_manager
 * @objs: additional &drm_gem_objects to lock
 * @num_objs: the number of additional &drm_gem_objects to lock
 * @num_fences: the amount of &dma_fences to reserve
 * @interruptible: sleep interruptible if waiting
 *
 * Acquires all dma-resv locks of all &drm_gem_objects the given
 * &drm_gpuva_manager contains mappings of, plus the ones given through @objs.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int
drm_gpuva_manager_lock_array(struct drm_gpuva_manager *mgr,
			     struct drm_gem_object **objs,
			     unsigned int num_objs,
			     unsigned int num_fences,
			     bool interruptible)
{
	struct {
		struct drm_gem_object **objs;
		unsigned int num_objs;
	} args;

	args.objs = objs;
	args.num_objs = num_objs;

	return drm_gpuva_manager_lock_extra(mgr, fn_lock_array, &args,
					    num_fences, interruptible);
}
EXPORT_SYMBOL_GPL(drm_gpuva_manager_lock_array);

/**
 * drm_gpuva_manager_validate() - validate all BOs marked as evicted
 * @mgr: the &drm_gpuva_manager to validate evicted BOs
 *
 * Calls the &drm_gpuva_fn_ops.bo_validate callback for all evicted buffer
 * objects being mapped in the given &drm_gpuva_manager.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int
drm_gpuva_manager_validate(struct drm_gpuva_manager *mgr)
{
	const struct drm_gpuva_fn_ops *ops = mgr->ops;
	struct drm_gpuva_gem *vm_bo;
	int ret;

	if (unlikely(!ops || !ops->bo_validate))
		return -ENOTSUPP;

	/* At this point we should hold all dma-resv locks of all GEM objects
	 * associated with this GPU-VM, hence it is safe to walk the list.
	 */
	list_for_each_entry(vm_bo, &mgr->evict.list, list.entry.evict) {
		dma_resv_assert_held(vm_bo->obj->resv);

		ret = ops->bo_validate(vm_bo->obj);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(drm_gpuva_manager_validate);

/**
 * drm_gpuva_manager_resv_add_fence - add fence to private and all extobj
 * dma-resv
 * @mgr: the &drm_gpuva_manager to add a fence to
 * @fence: fence to add
 * @private_usage: private dma-resv usage
 * @extobj_usage: extobj dma-resv usage
 */
void
drm_gpuva_manager_resv_add_fence(struct drm_gpuva_manager *mgr,
				 struct dma_fence *fence,
				 enum dma_resv_usage private_usage,
				 enum dma_resv_usage extobj_usage)
{
	struct drm_exec *exec = DRM_GPUVA_EXEC(mgr);
	struct drm_gem_object *obj;
	unsigned long index;

	drm_exec_for_each_locked_object(exec, index, obj) {
			dma_resv_assert_held(obj->resv);
			dma_resv_add_fence(obj->resv, fence,
					   drm_gpuva_is_extobj(mgr, obj) ?
					   private_usage : extobj_usage);
	}
}
EXPORT_SYMBOL_GPL(drm_gpuva_manager_resv_add_fence);

static struct drm_gpuva_gem *
__drm_gpuva_gem_find(struct drm_gpuva_manager *mgr,
		     struct drm_gem_object *obj)
{
	struct drm_gpuva_gem *vm_bo;

	drm_gem_gpuva_assert_lock_held(obj);

	drm_gem_for_each_gpuva_gem(vm_bo, obj)
		if (vm_bo->mgr == mgr)
			return vm_bo;

	return NULL;
}

/**
 * drm_gpuva_gem_create() - create a new instance of struct drm_gpuva_gem
 * @mgr: The &drm_gpuva_manager the @obj is mapped in.
 * @obj: The &drm_gem_object being mapped in the @mgr.
 *
 * If provided by the driver, this function uses the &drm_gpuva_fn_ops
 * vm_bo_alloc() callback to allocate.
 *
 * Returns: a pointer to the &drm_gpuva_gem on success, NULL on failure
 */
struct drm_gpuva_gem *
drm_gpuva_gem_create(struct drm_gpuva_manager *mgr,
		     struct drm_gem_object *obj)
{
	const struct drm_gpuva_fn_ops *ops = mgr->ops;
	struct drm_gpuva_gem *vm_bo;

	if (ops && ops->vm_bo_alloc)
		vm_bo = ops->vm_bo_alloc();
	else
		vm_bo = kzalloc(sizeof(*vm_bo), GFP_KERNEL);

	if (unlikely(!vm_bo))
		return NULL;

	vm_bo->mgr = mgr;
	vm_bo->obj = obj;

	kref_init(&vm_bo->kref);
	INIT_LIST_HEAD(&vm_bo->list.gpuva);
	INIT_LIST_HEAD(&vm_bo->list.entry.gem);
	INIT_LIST_HEAD(&vm_bo->list.entry.evict);

	drm_gem_object_get(obj);

	return vm_bo;
}
EXPORT_SYMBOL_GPL(drm_gpuva_gem_create);

void
drm_gpuva_gem_destroy(struct kref *kref)
{
	struct drm_gpuva_gem *vm_bo = container_of(kref, struct drm_gpuva_gem,
						   kref);
	const struct drm_gpuva_fn_ops *ops = vm_bo->mgr->ops;

	drm_gem_object_put(vm_bo->obj);

	if (ops && ops->vm_bo_free)
		ops->vm_bo_free(vm_bo);
	else
		kfree(vm_bo);
}
EXPORT_SYMBOL_GPL(drm_gpuva_gem_destroy);

/**
 * drm_gpuva_gem_find() - find the &drm_gpuva_gem for the given
 * &drm_gpuva_manager and &drm_gem_object
 * @mgr: The &drm_gpuva_manager the @obj is mapped in.
 * @obj: The &drm_gem_object being mapped in the @mgr.
 *
 * Find the &drm_gpuva_gem representing the combination of the given
 * &drm_gpuva_manager and &drm_gem_object. If found, increases the reference
 * count of the &drm_gpuva_gem accordingly.
 *
 * Returns: a pointer to the &drm_gpuva_gem on success, NULL on failure
 */
struct drm_gpuva_gem *
drm_gpuva_gem_find(struct drm_gpuva_manager *mgr,
		   struct drm_gem_object *obj)
{
	struct drm_gpuva_gem *vm_bo = __drm_gpuva_gem_find(mgr, obj);

	return vm_bo ? drm_gpuva_gem_get(vm_bo) : NULL;
}
EXPORT_SYMBOL_GPL(drm_gpuva_gem_find);

/**
 * drm_gpuva_gem_obtain() - obtains and instance of the &drm_gpuva_gem for the
 * given &drm_gpuva_manager and &drm_gem_object
 * @mgr: The &drm_gpuva_manager the @obj is mapped in.
 * @obj: The &drm_gem_object being mapped in the @mgr.
 *
 * Find the &drm_gpuva_gem representing the combination of the given
 * &drm_gpuva_manager and &drm_gem_object. If found, increases the reference
 * count of the &drm_gpuva_gem accordingly. If not found, allsocates a new
 * &drm_gpuva_gem.
 *
 * Returns: a pointer to the &drm_gpuva_gem on success, an ERR_PTR on failure
 */
struct drm_gpuva_gem *
drm_gpuva_gem_obtain(struct drm_gpuva_manager *mgr,
		     struct drm_gem_object *obj)
{
	struct drm_gpuva_gem *vm_bo;

	vm_bo = drm_gpuva_gem_find(mgr, obj);
	if (vm_bo)
		return vm_bo;

	vm_bo = drm_gpuva_gem_create(mgr, obj);
	if (!vm_bo)
		return ERR_PTR(-ENOMEM);

	return vm_bo;
}
EXPORT_SYMBOL_GPL(drm_gpuva_gem_obtain);

/**
 * drm_gpuva_gem_obtain_prealloc() - obtains and instance of the &drm_gpuva_gem
 * for the given &drm_gpuva_manager and &drm_gem_object
 * @mgr: The &drm_gpuva_manager the @obj is mapped in.
 * @obj: The &drm_gem_object being mapped in the @mgr.
 *
 * Find the &drm_gpuva_gem representing the combination of the given
 * &drm_gpuva_manager and &drm_gem_object. If found, increases the reference
 * count of the found &drm_gpuva_gem accordingly, while the @__vm_bo reference
 * count is decreased. If not found @__vm_bo is returned.
 *
 * Returns: a pointer to the found &drm_gpuva_gem or @__vm_bo if no existing
 * &drm_gpuva_gem was found
 */
struct drm_gpuva_gem *
drm_gpuva_gem_obtain_prealloc(struct drm_gpuva_manager *mgr,
			      struct drm_gem_object *obj,
			      struct drm_gpuva_gem *__vm_bo)
{
	struct drm_gpuva_gem *vm_bo;

	vm_bo = drm_gpuva_gem_find(mgr, obj);
	if (vm_bo) {
		drm_gpuva_gem_put(__vm_bo);
		return vm_bo;
	}

	return __vm_bo;
}
EXPORT_SYMBOL_GPL(drm_gpuva_gem_obtain_prealloc);

static int
__drm_gpuva_extobj_insert(struct drm_gpuva_manager *mgr,
			  struct drm_gem_object *obj,
			  gfp_t gfp)
{
	MA_STATE(mas, &mgr->mt_ext, 0, 0);
	union {
		struct drm_gem_object *obj;
		uintptr_t index;
	} gem;
	union {
		void *ptr;
		uintptr_t cnt;
	} ref;
	int ret = 0;

	gem.obj = obj;
	mas_set(&mas, gem.index);

	mas_lock(&mas);
	ref.ptr = mas_walk(&mas);
	if (ref.ptr) {
		++ref.cnt;
		mas_store(&mas, ref.ptr);
	} else {
		if (unlikely(!gfp)) {
			ret = -EINVAL;
			goto out;
		}

		mas_set(&mas, gem.index);
		ref.cnt = 1;
		ret = mas_store_gfp(&mas, ref.ptr, gfp);
		if (likely(!ret))
			drm_gem_object_get(obj);
	}
out:
	mas_unlock(&mas);
	return ret;
}

static void
__drm_gpuva_extobj_remove(struct drm_gpuva_manager *mgr,
			  struct drm_gem_object *obj)
{
	MA_STATE(mas, &mgr->mt_ext, 0, 0);
	union {
		struct drm_gem_object *obj;
		uintptr_t index;
	} gem;
	union {
		void *ptr;
		uintptr_t cnt;
	} ref;

	gem.obj = obj;
	mas_set(&mas, gem.index);

	mas_lock(&mas);
	if (unlikely(!(ref.ptr = mas_walk(&mas))))
		goto out;

	if (!--ref.cnt) {
		mas_erase(&mas);
		drm_gem_object_put(obj);
	} else {
		mas_store(&mas, ref.ptr);
	}
out:
	mas_unlock(&mas);
}

/**
 * drm_gpuva_extobj_insert - insert an external &drm_gem_object
 * @mgr: the &drm_gpuva_manager to insert into
 * @obj: the &drm_gem_object to insert as extobj
 *
 * Insert a &drm_gem_object into the &drm_gpuva_managers external object tree.
 * If the &drm_gem_object already exists in the tree, the reference counter
 * of this external object is increased by one.
 *
 * Drivers should insert the external &drm_gem_object before the dma-fence
 * signalling critical section, e.g. when submitting the job, and before
 * locking all &drm_gem_objects of a GPU-VM, e.g. with drm_gpuva_manager_lock()
 * or its dervates.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int
drm_gpuva_extobj_insert(struct drm_gpuva_manager *mgr,
			struct drm_gem_object *obj)
{
	return drm_gpuva_is_extobj(mgr, obj) ?
		__drm_gpuva_extobj_insert(mgr, obj, GFP_KERNEL) : 0;

}
EXPORT_SYMBOL_GPL(drm_gpuva_extobj_insert);

/**
 * drm_gpuva_extobj_get - increase the referecne count of an external
 * &drm_gem_object
 * @mgr: the &drm_gpuva_manager storing the extobj
 * @obj: the &drm_gem_object to representing the extobj
 *
 * Increases the reference count of the extobj represented by @obj.
 *
 * Drivers should call this for every &drm_gpuva backed by a &drm_gem_object
 * being inserted.
 *
 * For &drm_gpuva_op_remap operations drivers should make sure to only take an
 * additional reference if the re-map operation splits an existing &drm_gpuva
 * into two separate ones.
 *
 * See also drm_gpuva_map_get() and drm_gpuva_remap_get().
 *
 * Returns: 0 on success, negative error code on failure.
 */
void
drm_gpuva_extobj_get(struct drm_gpuva_manager *mgr,
		     struct drm_gem_object *obj)
{
	if (drm_gpuva_is_extobj(mgr, obj))
		WARN(__drm_gpuva_extobj_insert(mgr, obj, 0),
		     "Can't increase ref-count of non-existent extobj.");
}
EXPORT_SYMBOL_GPL(drm_gpuva_extobj_get);

/**
 * drm_gpuva_extobj_put - decrease the referecne count of an external
 * &drm_gem_object
 * @mgr: the &drm_gpuva_manager storing the extobj
 * @obj: the &drm_gem_object to representing the extobj
 *
 * Decreases the reference count of the extobj represented by @obj.
 *
 * Drivers should call this for every &drm_gpuva backed by a &drm_gem_object
 * being removed from the GPU VA space.
 *
 * See also drm_gpuva_unmap_put().
 *
 * Returns: 0 on success, negative error code on failure.
 */
void
drm_gpuva_extobj_put(struct drm_gpuva_manager *mgr,
		     struct drm_gem_object *obj)
{
	if (drm_gpuva_is_extobj(mgr, obj))
		__drm_gpuva_extobj_remove(mgr, obj);
}
EXPORT_SYMBOL_GPL(drm_gpuva_extobj_put);

/**
 * drm_gpuva_gem_evict() - add / remove a &drm_gem_object to / from a
 * &drm_gpuva_managers evicted list
 * @obj: the &drm_gem_object to add or remove
 * @evict: indicates whether the object is evicted
 *
 * Adds a &drm_gem_object to or removes it from all &drm_gpuva_managers evicted
 * list containing a mapping of this &drm_gem_object.
 */
void
drm_gpuva_gem_evict(struct drm_gem_object *obj, bool evict)
{
	struct drm_gpuva_gem *vm_bo;

	/* Required for iterating the GEMs GPUVA GEM list. If no driver specific
	 * lock has been set, the list is protected with the GEMs dma-resv lock.
	 */
	drm_gem_gpuva_assert_lock_held(obj);

	/* Required to protect the GPUVA managers evict list against concurrent
	 * access through drm_gpuva_manager_validate(). Concurrent insertions to
	 * the evict list through different GEM object evictions are protected
	 * by the GPUVA managers evict lock.
	 */
	dma_resv_assert_held(obj->resv);

	drm_gem_for_each_gpuva_gem(vm_bo, obj) {
		struct drm_gpuva_manager *mgr = vm_bo->mgr;

		spin_lock(&mgr->evict.lock);
		if (evict)
			list_add_tail(&vm_bo->list.entry.evict,
				      &mgr->evict.list);
		else
			list_del_init(&vm_bo->list.entry.evict);
		spin_unlock(&mgr->evict.lock);
	}
}
EXPORT_SYMBOL_GPL(drm_gpuva_gem_evict);

static int
__drm_gpuva_insert(struct drm_gpuva_manager *mgr,
		   struct drm_gpuva *va)
{
	struct rb_node *node;
	struct list_head *head;

	if (drm_gpuva_it_iter_first(&mgr->rb.tree,
				    GPUVA_START(va),
				    GPUVA_LAST(va)))
		return -EEXIST;

	va->mgr = mgr;

	drm_gpuva_it_insert(va, &mgr->rb.tree);

	node = rb_prev(&va->rb.node);
	if (node)
		head = &(to_drm_gpuva(node))->rb.entry;
	else
		head = &mgr->rb.list;

	list_add(&va->rb.entry, head);

	return 0;
}

/**
 * drm_gpuva_insert() - insert a &drm_gpuva
 * @mgr: the &drm_gpuva_manager to insert the &drm_gpuva in
 * @va: the &drm_gpuva to insert
 *
 * Insert a &drm_gpuva with a given address and range into a
 * &drm_gpuva_manager.
 *
 * It is safe to use this function using the safe versions of iterating the GPU
 * VA space, such as drm_gpuva_for_each_va_safe() and
 * drm_gpuva_for_each_va_range_safe().
 *
 * Returns: 0 on success, negative error code on failure.
 */
int
drm_gpuva_insert(struct drm_gpuva_manager *mgr,
		 struct drm_gpuva *va)
{
	u64 addr = va->va.addr;
	u64 range = va->va.range;

	if (unlikely(!drm_gpuva_range_valid(mgr, addr, range)))
		return -EINVAL;

	return __drm_gpuva_insert(mgr, va);
}
EXPORT_SYMBOL_GPL(drm_gpuva_insert);

static void
__drm_gpuva_remove(struct drm_gpuva *va)
{
	drm_gpuva_it_remove(va, &va->mgr->rb.tree);
	list_del_init(&va->rb.entry);
}

/**
 * drm_gpuva_remove() - remove a &drm_gpuva
 * @va: the &drm_gpuva to remove
 *
 * This removes the given &va from the underlaying tree.
 *
 * It is safe to use this function using the safe versions of iterating the GPU
 * VA space, such as drm_gpuva_for_each_va_safe() and
 * drm_gpuva_for_each_va_range_safe().
 */
void
drm_gpuva_remove(struct drm_gpuva *va)
{
	struct drm_gpuva_manager *mgr = va->mgr;

	if (unlikely(va == &mgr->kernel_alloc_node)) {
		WARN(1, "Can't destroy kernel reserved node.\n");
		return;
	}

	__drm_gpuva_remove(va);
}
EXPORT_SYMBOL_GPL(drm_gpuva_remove);

/**
 * drm_gpuva_link() - link a &drm_gpuva
 * @va: the &drm_gpuva to link
 * @vm_bo: the &drm_gpuva_gem to add the &drm_gpuva to
 *
 * This adds the given &va to the GPU VA list of the &drm_gpuva_gem and the
 * &drm_gpuva_gem to the &drm_gem_object it is associated with.
 *
 * For every &drm_gpuva entry added to the &drm_gpuva_gem an additional
 * reference of the latter is taken.
 *
 * This function expects the caller to protect the GEM's GPUVA list against
 * concurrent access using either the GEMs dma_resv lock or a driver specific
 * lock set through drm_gem_gpuva_set_lock().
 */
void
drm_gpuva_link(struct drm_gpuva *va, struct drm_gpuva_gem *vm_bo)
{
	struct drm_gem_object *obj = va->gem.obj;

	if (unlikely(!obj))
		return;

	drm_gem_gpuva_assert_lock_held(obj);

	drm_gpuva_gem_get(vm_bo);
	list_add_tail(&va->gem.entry, &vm_bo->list.gpuva);
	if (list_empty(&vm_bo->list.entry.gem))
		list_add_tail(&vm_bo->list.entry.gem, &obj->gpuva.list);
}
EXPORT_SYMBOL_GPL(drm_gpuva_link);

/**
 * drm_gpuva_unlink() - unlink a &drm_gpuva
 * @va: the &drm_gpuva to unlink
 *
 * This removes the given &va from the GPU VA list of the &drm_gem_object it is
 * associated with.
 *
 * This removes the given &va from the GPU VA list of the &drm_gpuva_gem and
 * the &drm_gpuva_gem from the &drm_gem_object it is associated with in case
 * this call unlinks the last &drm_gpuva from the &drm_gpuva_gem.
 *
 * For every &drm_gpuva entry removed from the &drm_gpuva_gem a reference of
 * the latter is dropped.
 *
 * This function expects the caller to protect the GEM's GPUVA list against
 * concurrent access using either the GEMs dma_resv lock or a driver specific
 * lock set through drm_gem_gpuva_set_lock().
 */
void
drm_gpuva_unlink(struct drm_gpuva *va)
{
	struct drm_gem_object *obj = va->gem.obj;
	struct drm_gpuva_gem *vm_bo;

	if (unlikely(!obj))
		return;

	drm_gem_gpuva_assert_lock_held(obj);

	vm_bo = __drm_gpuva_gem_find(va->mgr, obj);
	if (WARN(!vm_bo, "GPUVA doesn't seem to be linked.\n"))
		return;

	list_del_init(&va->gem.entry);

	if (list_empty(&vm_bo->list.gpuva)) {
		list_del_init(&vm_bo->list.entry.gem);
		list_del_init(&vm_bo->list.entry.evict);
	}
	drm_gpuva_gem_put(vm_bo);
}
EXPORT_SYMBOL_GPL(drm_gpuva_unlink);

/**
 * drm_gpuva_find_first() - find the first &drm_gpuva in the given range
 * @mgr: the &drm_gpuva_manager to search in
 * @addr: the &drm_gpuvas address
 * @range: the &drm_gpuvas range
 *
 * Returns: the first &drm_gpuva within the given range
 */
struct drm_gpuva *
drm_gpuva_find_first(struct drm_gpuva_manager *mgr,
		     u64 addr, u64 range)
{
	u64 last = addr + range - 1;

	return drm_gpuva_it_iter_first(&mgr->rb.tree, addr, last);
}
EXPORT_SYMBOL_GPL(drm_gpuva_find_first);

/**
 * drm_gpuva_find() - find a &drm_gpuva
 * @mgr: the &drm_gpuva_manager to search in
 * @addr: the &drm_gpuvas address
 * @range: the &drm_gpuvas range
 *
 * Returns: the &drm_gpuva at a given &addr and with a given &range
 */
struct drm_gpuva *
drm_gpuva_find(struct drm_gpuva_manager *mgr,
	       u64 addr, u64 range)
{
	struct drm_gpuva *va;

	va = drm_gpuva_find_first(mgr, addr, range);
	if (!va)
		goto out;

	if (va->va.addr != addr ||
	    va->va.range != range)
		goto out;

	return va;

out:
	return NULL;
}
EXPORT_SYMBOL_GPL(drm_gpuva_find);

/**
 * drm_gpuva_find_prev() - find the &drm_gpuva before the given address
 * @mgr: the &drm_gpuva_manager to search in
 * @start: the given GPU VA's start address
 *
 * Find the adjacent &drm_gpuva before the GPU VA with given &start address.
 *
 * Note that if there is any free space between the GPU VA mappings no mapping
 * is returned.
 *
 * Returns: a pointer to the found &drm_gpuva or NULL if none was found
 */
struct drm_gpuva *
drm_gpuva_find_prev(struct drm_gpuva_manager *mgr, u64 start)
{
	if (!drm_gpuva_range_valid(mgr, start - 1, 1))
		return NULL;

	return drm_gpuva_it_iter_first(&mgr->rb.tree, start - 1, start);
}
EXPORT_SYMBOL_GPL(drm_gpuva_find_prev);

/**
 * drm_gpuva_find_next() - find the &drm_gpuva after the given address
 * @mgr: the &drm_gpuva_manager to search in
 * @end: the given GPU VA's end address
 *
 * Find the adjacent &drm_gpuva after the GPU VA with given &end address.
 *
 * Note that if there is any free space between the GPU VA mappings no mapping
 * is returned.
 *
 * Returns: a pointer to the found &drm_gpuva or NULL if none was found
 */
struct drm_gpuva *
drm_gpuva_find_next(struct drm_gpuva_manager *mgr, u64 end)
{
	if (!drm_gpuva_range_valid(mgr, end, 1))
		return NULL;

	return drm_gpuva_it_iter_first(&mgr->rb.tree, end, end + 1);
}
EXPORT_SYMBOL_GPL(drm_gpuva_find_next);

/**
 * drm_gpuva_interval_empty() - indicate whether a given interval of the VA space
 * is empty
 * @mgr: the &drm_gpuva_manager to check the range for
 * @addr: the start address of the range
 * @range: the range of the interval
 *
 * Returns: true if the interval is empty, false otherwise
 */
bool
drm_gpuva_interval_empty(struct drm_gpuva_manager *mgr, u64 addr, u64 range)
{
	return !drm_gpuva_find_first(mgr, addr, range);
}
EXPORT_SYMBOL_GPL(drm_gpuva_interval_empty);

/**
 * drm_gpuva_map() - helper to insert a &drm_gpuva according to a
 * &drm_gpuva_op_map
 * @mgr: the &drm_gpuva_manager
 * @va: the &drm_gpuva to insert
 * @op: the &drm_gpuva_op_map to initialize @va with
 *
 * Initializes the @va from the @op and inserts it into the given @mgr.
 */
void
drm_gpuva_map(struct drm_gpuva_manager *mgr,
	      struct drm_gpuva *va,
	      struct drm_gpuva_op_map *op)
{
	drm_gpuva_init_from_op(va, op);
	drm_gpuva_insert(mgr, va);
}
EXPORT_SYMBOL_GPL(drm_gpuva_map);

/**
 * drm_gpuva_map_get() - helper to insert a &drm_gpuva according to a
 * &drm_gpuva_op_map
 * @mgr: the &drm_gpuva_manager
 * @va: the &drm_gpuva to insert
 * @op: the &drm_gpuva_op_map to initialize @va with
 *
 * Initializes the @va from the @op and inserts it into the given @mgr and
 * increases the reference count of the corresponding extobj.
 */
void
drm_gpuva_map_get(struct drm_gpuva_manager *mgr,
		  struct drm_gpuva *va,
		  struct drm_gpuva_op_map *op)
{
	drm_gpuva_map(mgr, va, op);
	drm_gpuva_extobj_get(mgr, va->gem.obj);
}
EXPORT_SYMBOL_GPL(drm_gpuva_map_get);

/**
 * drm_gpuva_remap() - helper to remap a &drm_gpuva according to a
 * &drm_gpuva_op_remap
 * @prev: the &drm_gpuva to remap when keeping the start of a mapping
 * @next: the &drm_gpuva to remap when keeping the end of a mapping
 * @op: the &drm_gpuva_op_remap to initialize @prev and @next with
 *
 * Removes the currently mapped &drm_gpuva and remaps it using @prev and/or
 * @next.
 */
void
drm_gpuva_remap(struct drm_gpuva *prev,
		struct drm_gpuva *next,
		struct drm_gpuva_op_remap *op)
{
	struct drm_gpuva *va = op->unmap->va;
	struct drm_gpuva_manager *mgr = va->mgr;

	drm_gpuva_remove(va);

	if (op->prev) {
		drm_gpuva_init_from_op(prev, op->prev);
		drm_gpuva_insert(mgr, prev);
	}

	if (op->next) {
		drm_gpuva_init_from_op(next, op->next);
		drm_gpuva_insert(mgr, next);
	}
}
EXPORT_SYMBOL_GPL(drm_gpuva_remap);

/**
 * drm_gpuva_remap_get() - helper to remap a &drm_gpuva according to a
 * &drm_gpuva_op_remap
 * @prev: the &drm_gpuva to remap when keeping the start of a mapping
 * @next: the &drm_gpuva to remap when keeping the end of a mapping
 * @op: the &drm_gpuva_op_remap to initialize @prev and @next with
 *
 * Removes the currently mapped &drm_gpuva and remaps it using @prev and/or
 * @next. Additionally, if the re-map splits the existing &drm_gpuva into two
 * separate mappings, increases the reference count of the corresponding extobj.
 */
void
drm_gpuva_remap_get(struct drm_gpuva *prev,
		    struct drm_gpuva *next,
		    struct drm_gpuva_op_remap *op)
{
	struct drm_gpuva *va = op->unmap->va;
	struct drm_gpuva_manager *mgr = va->mgr;

	drm_gpuva_remap(prev, next, op);
	if (op->prev && op->next)
		drm_gpuva_extobj_get(mgr, va->gem.obj);
}
EXPORT_SYMBOL_GPL(drm_gpuva_remap_get);

/**
 * drm_gpuva_unmap() - helper to remove a &drm_gpuva according to a
 * &drm_gpuva_op_unmap
 * @op: the &drm_gpuva_op_unmap specifying the &drm_gpuva to remove
 *
 * Removes the &drm_gpuva associated with the &drm_gpuva_op_unmap.
 */
void
drm_gpuva_unmap(struct drm_gpuva_op_unmap *op)
{
	drm_gpuva_remove(op->va);
}
EXPORT_SYMBOL_GPL(drm_gpuva_unmap);

/**
 * drm_gpuva_unmap_put() - helper to remove a &drm_gpuva according to a
 * &drm_gpuva_op_unmap
 * @op: the &drm_gpuva_op_unmap specifying the &drm_gpuva to remove
 *
 * Removes the &drm_gpuva associated with the &drm_gpuva_op_unmap and decreases
 * the reference count of the corresponding extobj.
 */
void
drm_gpuva_unmap_put(struct drm_gpuva_op_unmap *op)
{
	struct drm_gpuva *va = op->va;

	drm_gpuva_unmap(op);
	drm_gpuva_extobj_put(va->mgr, va->gem.obj);
}
EXPORT_SYMBOL_GPL(drm_gpuva_unmap_put);

static int
op_map_cb(const struct drm_gpuva_fn_ops *fn, void *priv,
	  u64 addr, u64 range,
	  struct drm_gem_object *obj, u64 offset)
{
	struct drm_gpuva_op op = {};

	op.op = DRM_GPUVA_OP_MAP;
	op.map.va.addr = addr;
	op.map.va.range = range;
	op.map.gem.obj = obj;
	op.map.gem.offset = offset;

	return fn->sm_step_map(&op, priv);
}

static int
op_remap_cb(const struct drm_gpuva_fn_ops *fn, void *priv,
	    struct drm_gpuva_op_map *prev,
	    struct drm_gpuva_op_map *next,
	    struct drm_gpuva_op_unmap *unmap)
{
	struct drm_gpuva_op op = {};
	struct drm_gpuva_op_remap *r;

	op.op = DRM_GPUVA_OP_REMAP;
	r = &op.remap;
	r->prev = prev;
	r->next = next;
	r->unmap = unmap;

	return fn->sm_step_remap(&op, priv);
}

static int
op_unmap_cb(const struct drm_gpuva_fn_ops *fn, void *priv,
	    struct drm_gpuva *va, bool merge)
{
	struct drm_gpuva_op op = {};

	op.op = DRM_GPUVA_OP_UNMAP;
	op.unmap.va = va;
	op.unmap.keep = merge;

	return fn->sm_step_unmap(&op, priv);
}

static int
__drm_gpuva_sm_map(struct drm_gpuva_manager *mgr,
		   const struct drm_gpuva_fn_ops *ops, void *priv,
		   u64 req_addr, u64 req_range,
		   struct drm_gem_object *req_obj, u64 req_offset)
{
	struct drm_gpuva *va, *next, *prev = NULL;
	u64 req_end = req_addr + req_range;
	int ret;

	if (unlikely(!drm_gpuva_range_valid(mgr, req_addr, req_range)))
		return -EINVAL;

	drm_gpuva_for_each_va_range_safe(va, next, mgr, req_addr, req_end) {
		struct drm_gem_object *obj = va->gem.obj;
		u64 offset = va->gem.offset;
		u64 addr = va->va.addr;
		u64 range = va->va.range;
		u64 end = addr + range;
		bool merge = !!va->gem.obj;

		if (addr == req_addr) {
			merge &= obj == req_obj &&
				 offset == req_offset;

			if (end == req_end) {
				ret = op_unmap_cb(ops, priv, va, merge);
				if (ret)
					return ret;
				break;
			}

			if (end < req_end) {
				ret = op_unmap_cb(ops, priv, va, merge);
				if (ret)
					return ret;
				goto next;
			}

			if (end > req_end) {
				struct drm_gpuva_op_map n = {
					.va.addr = req_end,
					.va.range = range - req_range,
					.gem.obj = obj,
					.gem.offset = offset + req_range,
				};
				struct drm_gpuva_op_unmap u = {
					.va = va,
					.keep = merge,
				};

				ret = op_remap_cb(ops, priv, NULL, &n, &u);
				if (ret)
					return ret;
				break;
			}
		} else if (addr < req_addr) {
			u64 ls_range = req_addr - addr;
			struct drm_gpuva_op_map p = {
				.va.addr = addr,
				.va.range = ls_range,
				.gem.obj = obj,
				.gem.offset = offset,
			};
			struct drm_gpuva_op_unmap u = { .va = va };

			merge &= obj == req_obj &&
				 offset + ls_range == req_offset;
			u.keep = merge;

			if (end == req_end) {
				ret = op_remap_cb(ops, priv, &p, NULL, &u);
				if (ret)
					return ret;
				break;
			}

			if (end < req_end) {
				ret = op_remap_cb(ops, priv, &p, NULL, &u);
				if (ret)
					return ret;
				goto next;
			}

			if (end > req_end) {
				struct drm_gpuva_op_map n = {
					.va.addr = req_end,
					.va.range = end - req_end,
					.gem.obj = obj,
					.gem.offset = offset + ls_range +
						      req_range,
				};

				ret = op_remap_cb(ops, priv, &p, &n, &u);
				if (ret)
					return ret;
				break;
			}
		} else if (addr > req_addr) {
			merge &= obj == req_obj &&
				 offset == req_offset +
					   (addr - req_addr);

			if (end == req_end) {
				ret = op_unmap_cb(ops, priv, va, merge);
				if (ret)
					return ret;
				break;
			}

			if (end < req_end) {
				ret = op_unmap_cb(ops, priv, va, merge);
				if (ret)
					return ret;
				goto next;
			}

			if (end > req_end) {
				struct drm_gpuva_op_map n = {
					.va.addr = req_end,
					.va.range = end - req_end,
					.gem.obj = obj,
					.gem.offset = offset + req_end - addr,
				};
				struct drm_gpuva_op_unmap u = {
					.va = va,
					.keep = merge,
				};

				ret = op_remap_cb(ops, priv, NULL, &n, &u);
				if (ret)
					return ret;
				break;
			}
		}
next:
		prev = va;
	}

	return op_map_cb(ops, priv,
			 req_addr, req_range,
			 req_obj, req_offset);
}

static int
__drm_gpuva_sm_unmap(struct drm_gpuva_manager *mgr,
		     const struct drm_gpuva_fn_ops *ops, void *priv,
		     u64 req_addr, u64 req_range)
{
	struct drm_gpuva *va, *next;
	u64 req_end = req_addr + req_range;
	int ret;

	if (unlikely(!drm_gpuva_range_valid(mgr, req_addr, req_range)))
		return -EINVAL;

	drm_gpuva_for_each_va_range_safe(va, next, mgr, req_addr, req_end) {
		struct drm_gpuva_op_map prev = {}, next = {};
		bool prev_split = false, next_split = false;
		struct drm_gem_object *obj = va->gem.obj;
		u64 offset = va->gem.offset;
		u64 addr = va->va.addr;
		u64 range = va->va.range;
		u64 end = addr + range;

		if (addr < req_addr) {
			prev.va.addr = addr;
			prev.va.range = req_addr - addr;
			prev.gem.obj = obj;
			prev.gem.offset = offset;

			prev_split = true;
		}

		if (end > req_end) {
			next.va.addr = req_end;
			next.va.range = end - req_end;
			next.gem.obj = obj;
			next.gem.offset = offset + (req_end - addr);

			next_split = true;
		}

		if (prev_split || next_split) {
			struct drm_gpuva_op_unmap unmap = { .va = va };

			ret = op_remap_cb(ops, priv,
					  prev_split ? &prev : NULL,
					  next_split ? &next : NULL,
					  &unmap);
			if (ret)
				return ret;
		} else {
			ret = op_unmap_cb(ops, priv, va, false);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * drm_gpuva_sm_map() - creates the &drm_gpuva_op split/merge steps
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @req_addr: the start address of the new mapping
 * @req_range: the range of the new mapping
 * @req_obj: the &drm_gem_object to map
 * @req_offset: the offset within the &drm_gem_object
 * @priv: pointer to a driver private data structure
 *
 * This function iterates the given range of the GPU VA space. It utilizes the
 * &drm_gpuva_fn_ops to call back into the driver providing the split and merge
 * steps.
 *
 * Drivers may use these callbacks to update the GPU VA space right away within
 * the callback. In case the driver decides to copy and store the operations for
 * later processing neither this function nor &drm_gpuva_sm_unmap is allowed to
 * be called before the &drm_gpuva_manager's view of the GPU VA space was
 * updated with the previous set of operations. To update the
 * &drm_gpuva_manager's view of the GPU VA space drm_gpuva_insert(),
 * drm_gpuva_destroy_locked() and/or drm_gpuva_destroy_unlocked() should be
 * used.
 *
 * A sequence of callbacks can contain map, unmap and remap operations, but
 * the sequence of callbacks might also be empty if no operation is required,
 * e.g. if the requested mapping already exists in the exact same way.
 *
 * There can be an arbitrary amount of unmap operations, a maximum of two remap
 * operations and a single map operation. The latter one represents the original
 * map operation requested by the caller.
 *
 * Returns: 0 on success or a negative error code
 */
int
drm_gpuva_sm_map(struct drm_gpuva_manager *mgr, void *priv,
		 u64 req_addr, u64 req_range,
		 struct drm_gem_object *req_obj, u64 req_offset)
{
	const struct drm_gpuva_fn_ops *ops = mgr->ops;

	if (unlikely(!(ops && ops->sm_step_map &&
		       ops->sm_step_remap &&
		       ops->sm_step_unmap)))
		return -EINVAL;

	return __drm_gpuva_sm_map(mgr, ops, priv,
				  req_addr, req_range,
				  req_obj, req_offset);
}
EXPORT_SYMBOL_GPL(drm_gpuva_sm_map);

/**
 * drm_gpuva_sm_unmap() - creates the &drm_gpuva_ops to split on unmap
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @priv: pointer to a driver private data structure
 * @req_addr: the start address of the range to unmap
 * @req_range: the range of the mappings to unmap
 *
 * This function iterates the given range of the GPU VA space. It utilizes the
 * &drm_gpuva_fn_ops to call back into the driver providing the operations to
 * unmap and, if required, split existent mappings.
 *
 * Drivers may use these callbacks to update the GPU VA space right away within
 * the callback. In case the driver decides to copy and store the operations for
 * later processing neither this function nor &drm_gpuva_sm_map is allowed to be
 * called before the &drm_gpuva_manager's view of the GPU VA space was updated
 * with the previous set of operations. To update the &drm_gpuva_manager's view
 * of the GPU VA space drm_gpuva_insert(), drm_gpuva_destroy_locked() and/or
 * drm_gpuva_destroy_unlocked() should be used.
 *
 * A sequence of callbacks can contain unmap and remap operations, depending on
 * whether there are actual overlapping mappings to split.
 *
 * There can be an arbitrary amount of unmap operations and a maximum of two
 * remap operations.
 *
 * Returns: 0 on success or a negative error code
 */
int
drm_gpuva_sm_unmap(struct drm_gpuva_manager *mgr, void *priv,
		   u64 req_addr, u64 req_range)
{
	const struct drm_gpuva_fn_ops *ops = mgr->ops;

	if (unlikely(!(ops && ops->sm_step_remap &&
		       ops->sm_step_unmap)))
		return -EINVAL;

	return __drm_gpuva_sm_unmap(mgr, ops, priv,
				    req_addr, req_range);
}
EXPORT_SYMBOL_GPL(drm_gpuva_sm_unmap);

static struct drm_gpuva_op *
gpuva_op_alloc(struct drm_gpuva_manager *mgr)
{
	const struct drm_gpuva_fn_ops *fn = mgr->ops;
	struct drm_gpuva_op *op;

	if (fn && fn->op_alloc)
		op = fn->op_alloc();
	else
		op = kzalloc(sizeof(*op), GFP_KERNEL);

	if (unlikely(!op))
		return NULL;

	return op;
}

static void
gpuva_op_free(struct drm_gpuva_manager *mgr,
	      struct drm_gpuva_op *op)
{
	const struct drm_gpuva_fn_ops *fn = mgr->ops;

	if (fn && fn->op_free)
		fn->op_free(op);
	else
		kfree(op);
}

static int
drm_gpuva_sm_step(struct drm_gpuva_op *__op,
		  void *priv)
{
	struct {
		struct drm_gpuva_manager *mgr;
		struct drm_gpuva_ops *ops;
	} *args = priv;
	struct drm_gpuva_manager *mgr = args->mgr;
	struct drm_gpuva_ops *ops = args->ops;
	struct drm_gpuva_op *op;

	op = gpuva_op_alloc(mgr);
	if (unlikely(!op))
		goto err;

	memcpy(op, __op, sizeof(*op));

	if (op->op == DRM_GPUVA_OP_REMAP) {
		struct drm_gpuva_op_remap *__r = &__op->remap;
		struct drm_gpuva_op_remap *r = &op->remap;

		r->unmap = kmemdup(__r->unmap, sizeof(*r->unmap),
				   GFP_KERNEL);
		if (unlikely(!r->unmap))
			goto err_free_op;

		if (__r->prev) {
			r->prev = kmemdup(__r->prev, sizeof(*r->prev),
					  GFP_KERNEL);
			if (unlikely(!r->prev))
				goto err_free_unmap;
		}

		if (__r->next) {
			r->next = kmemdup(__r->next, sizeof(*r->next),
					  GFP_KERNEL);
			if (unlikely(!r->next))
				goto err_free_prev;
		}
	}

	list_add_tail(&op->entry, &ops->list);

	return 0;

err_free_unmap:
	kfree(op->remap.unmap);
err_free_prev:
	kfree(op->remap.prev);
err_free_op:
	gpuva_op_free(mgr, op);
err:
	return -ENOMEM;
}

static const struct drm_gpuva_fn_ops gpuva_list_ops = {
	.sm_step_map = drm_gpuva_sm_step,
	.sm_step_remap = drm_gpuva_sm_step,
	.sm_step_unmap = drm_gpuva_sm_step,
};

/**
 * drm_gpuva_sm_map_ops_create() - creates the &drm_gpuva_ops to split and merge
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @req_addr: the start address of the new mapping
 * @req_range: the range of the new mapping
 * @req_obj: the &drm_gem_object to map
 * @req_offset: the offset within the &drm_gem_object
 *
 * This function creates a list of operations to perform splitting and merging
 * of existent mapping(s) with the newly requested one.
 *
 * The list can be iterated with &drm_gpuva_for_each_op and must be processed
 * in the given order. It can contain map, unmap and remap operations, but it
 * also can be empty if no operation is required, e.g. if the requested mapping
 * already exists is the exact same way.
 *
 * There can be an arbitrary amount of unmap operations, a maximum of two remap
 * operations and a single map operation. The latter one represents the original
 * map operation requested by the caller.
 *
 * Note that before calling this function again with another mapping request it
 * is necessary to update the &drm_gpuva_manager's view of the GPU VA space. The
 * previously obtained operations must be either processed or abandoned. To
 * update the &drm_gpuva_manager's view of the GPU VA space drm_gpuva_insert(),
 * drm_gpuva_destroy_locked() and/or drm_gpuva_destroy_unlocked() should be
 * used.
 *
 * After the caller finished processing the returned &drm_gpuva_ops, they must
 * be freed with &drm_gpuva_ops_free.
 *
 * Returns: a pointer to the &drm_gpuva_ops on success, an ERR_PTR on failure
 */
struct drm_gpuva_ops *
drm_gpuva_sm_map_ops_create(struct drm_gpuva_manager *mgr,
			    u64 req_addr, u64 req_range,
			    struct drm_gem_object *req_obj, u64 req_offset)
{
	struct drm_gpuva_ops *ops;
	struct {
		struct drm_gpuva_manager *mgr;
		struct drm_gpuva_ops *ops;
	} args;
	int ret;

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (unlikely(!ops))
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->list);

	args.mgr = mgr;
	args.ops = ops;

	ret = __drm_gpuva_sm_map(mgr, &gpuva_list_ops, &args,
				 req_addr, req_range,
				 req_obj, req_offset);
	if (ret)
		goto err_free_ops;

	return ops;

err_free_ops:
	drm_gpuva_ops_free(mgr, ops);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gpuva_sm_map_ops_create);

/**
 * drm_gpuva_sm_unmap_ops_create() - creates the &drm_gpuva_ops to split on
 * unmap
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @req_addr: the start address of the range to unmap
 * @req_range: the range of the mappings to unmap
 *
 * This function creates a list of operations to perform unmapping and, if
 * required, splitting of the mappings overlapping the unmap range.
 *
 * The list can be iterated with &drm_gpuva_for_each_op and must be processed
 * in the given order. It can contain unmap and remap operations, depending on
 * whether there are actual overlapping mappings to split.
 *
 * There can be an arbitrary amount of unmap operations and a maximum of two
 * remap operations.
 *
 * Note that before calling this function again with another range to unmap it
 * is necessary to update the &drm_gpuva_manager's view of the GPU VA space. The
 * previously obtained operations must be processed or abandoned. To update the
 * &drm_gpuva_manager's view of the GPU VA space drm_gpuva_insert(),
 * drm_gpuva_destroy_locked() and/or drm_gpuva_destroy_unlocked() should be
 * used.
 *
 * After the caller finished processing the returned &drm_gpuva_ops, they must
 * be freed with &drm_gpuva_ops_free.
 *
 * Returns: a pointer to the &drm_gpuva_ops on success, an ERR_PTR on failure
 */
struct drm_gpuva_ops *
drm_gpuva_sm_unmap_ops_create(struct drm_gpuva_manager *mgr,
			      u64 req_addr, u64 req_range)
{
	struct drm_gpuva_ops *ops;
	struct {
		struct drm_gpuva_manager *mgr;
		struct drm_gpuva_ops *ops;
	} args;
	int ret;

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (unlikely(!ops))
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->list);

	args.mgr = mgr;
	args.ops = ops;

	ret = __drm_gpuva_sm_unmap(mgr, &gpuva_list_ops, &args,
				   req_addr, req_range);
	if (ret)
		goto err_free_ops;

	return ops;

err_free_ops:
	drm_gpuva_ops_free(mgr, ops);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gpuva_sm_unmap_ops_create);

/**
 * drm_gpuva_prefetch_ops_create() - creates the &drm_gpuva_ops to prefetch
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @addr: the start address of the range to prefetch
 * @range: the range of the mappings to prefetch
 *
 * This function creates a list of operations to perform prefetching.
 *
 * The list can be iterated with &drm_gpuva_for_each_op and must be processed
 * in the given order. It can contain prefetch operations.
 *
 * There can be an arbitrary amount of prefetch operations.
 *
 * After the caller finished processing the returned &drm_gpuva_ops, they must
 * be freed with &drm_gpuva_ops_free.
 *
 * Returns: a pointer to the &drm_gpuva_ops on success, an ERR_PTR on failure
 */
struct drm_gpuva_ops *
drm_gpuva_prefetch_ops_create(struct drm_gpuva_manager *mgr,
			      u64 addr, u64 range)
{
	struct drm_gpuva_ops *ops;
	struct drm_gpuva_op *op;
	struct drm_gpuva *va;
	u64 end = addr + range;
	int ret;

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->list);

	drm_gpuva_for_each_va_range(va, mgr, addr, end) {
		op = gpuva_op_alloc(mgr);
		if (!op) {
			ret = -ENOMEM;
			goto err_free_ops;
		}

		op->op = DRM_GPUVA_OP_PREFETCH;
		op->prefetch.va = va;
		list_add_tail(&op->entry, &ops->list);
	}

	return ops;

err_free_ops:
	drm_gpuva_ops_free(mgr, ops);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gpuva_prefetch_ops_create);

/**
 * drm_gpuva_gem_unmap_ops_create() - creates the &drm_gpuva_ops to unmap a GEM
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @obj: the &drm_gem_object to unmap
 *
 * This function creates a list of operations to perform unmapping for every
 * GPUVA attached to a GEM.
 *
 * The list can be iterated with &drm_gpuva_for_each_op and consists out of an
 * arbitrary amount of unmap operations.
 *
 * After the caller finished processing the returned &drm_gpuva_ops, they must
 * be freed with &drm_gpuva_ops_free.
 *
 * It is the callers responsibility to protect the GEMs GPUVA list against
 * concurrent access using the GEMs dma_resv lock.
 *
 * Returns: a pointer to the &drm_gpuva_ops on success, an ERR_PTR on failure
 */
struct drm_gpuva_ops *
drm_gpuva_gem_unmap_ops_create(struct drm_gpuva_manager *mgr,
			       struct drm_gem_object *obj)
{
	struct drm_gpuva_ops *ops;
	struct drm_gpuva_op *op;
	struct drm_gpuva_gem *vm_bo;
	struct drm_gpuva *va;
	int ret;

	drm_gem_gpuva_assert_lock_held(obj);

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->list);

	drm_gem_for_each_gpuva(va, vm_bo, mgr, obj) {
		op = gpuva_op_alloc(mgr);
		if (!op) {
			ret = -ENOMEM;
			goto err_free_ops;
		}

		op->op = DRM_GPUVA_OP_UNMAP;
		op->unmap.va = va;
		list_add_tail(&op->entry, &ops->list);
	}

	return ops;

err_free_ops:
	drm_gpuva_ops_free(mgr, ops);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gpuva_gem_unmap_ops_create);

/**
 * drm_gpuva_ops_free() - free the given &drm_gpuva_ops
 * @mgr: the &drm_gpuva_manager the ops were created for
 * @ops: the &drm_gpuva_ops to free
 *
 * Frees the given &drm_gpuva_ops structure including all the ops associated
 * with it.
 */
void
drm_gpuva_ops_free(struct drm_gpuva_manager *mgr,
		   struct drm_gpuva_ops *ops)
{
	struct drm_gpuva_op *op, *next;

	drm_gpuva_for_each_op_safe(op, next, ops) {
		list_del(&op->entry);

		if (op->op == DRM_GPUVA_OP_REMAP) {
			kfree(op->remap.prev);
			kfree(op->remap.next);
			kfree(op->remap.unmap);
		}

		gpuva_op_free(mgr, op);
	}

	kfree(ops);
}
EXPORT_SYMBOL_GPL(drm_gpuva_ops_free);
