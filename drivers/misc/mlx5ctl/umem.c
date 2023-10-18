// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/mlx5/device.h>
#include <linux/mlx5/driver.h>
#include <uapi/misc/mlx5ctl.h>

#include "umem.h"

#define umem_dbg(__mdev, fmt, ...) \
	dev_dbg((__mdev)->device, "mlx5ctl_umem: " fmt, ##__VA_ARGS__)

#define MLX5CTL_UMEM_MAX_MB 64

static size_t umem_num_pages(u64 addr, size_t len)
{
	return (size_t)((ALIGN(addr + len, PAGE_SIZE) -
			 ALIGN_DOWN(addr, PAGE_SIZE))) /
			 PAGE_SIZE;
}

struct mlx5ctl_umem {
	struct sg_table sgt;
	unsigned long addr;
	size_t size;
	size_t offset;
	size_t npages;
	struct task_struct *source_task;
	struct mm_struct *source_mm;
	struct user_struct *source_user;
	u32 umem_id;
	struct page **page_list;
};

struct mlx5ctl_umem_db {
	struct xarray xarray;
	struct mlx5_core_dev *mdev;
	u32 uctx_uid;
};

static int inc_user_locked_vm(struct mlx5ctl_umem *umem, unsigned long npages)
{
	unsigned long lock_limit;
	unsigned long cur_pages;
	unsigned long new_pages;

	lock_limit = task_rlimit(umem->source_task, RLIMIT_MEMLOCK) >>
		     PAGE_SHIFT;
	do {
		cur_pages = atomic_long_read(&umem->source_user->locked_vm);
		new_pages = cur_pages + npages;
		if (new_pages > lock_limit)
			return -ENOMEM;
	} while (atomic_long_cmpxchg(&umem->source_user->locked_vm, cur_pages,
				     new_pages) != cur_pages);
	return 0;
}

static void dec_user_locked_vm(struct mlx5ctl_umem *umem, unsigned long npages)
{
	if (WARN_ON(atomic_long_read(&umem->source_user->locked_vm) < npages))
		return;
	atomic_long_sub(npages, &umem->source_user->locked_vm);
}

static struct mlx5ctl_umem *mlx5ctl_umem_pin(struct mlx5ctl_umem_db *umem_db,
					     unsigned long addr, size_t size)
{
	size_t npages = umem_num_pages(addr, size);
	struct mlx5_core_dev *mdev = umem_db->mdev;
	unsigned long endaddr = addr + size;
	struct mlx5ctl_umem *umem;
	struct page **page_list;
	int err = -EINVAL;
	int pinned = 0;

	umem_dbg(mdev, "%s: addr %p size %zu npages %zu\n",
		 __func__, (void *)addr, size, npages);

	/* Avoid integer overflow */
	if (endaddr < addr || PAGE_ALIGN(endaddr) < endaddr)
		return ERR_PTR(-EINVAL);

	if (npages == 0 || pages_to_mb(npages) > MLX5CTL_UMEM_MAX_MB)
		return ERR_PTR(-EINVAL);

	page_list = kvmalloc_array(npages, sizeof(struct page *), GFP_KERNEL_ACCOUNT);
	if (!page_list)
		return ERR_PTR(-ENOMEM);

	umem = kzalloc(sizeof(*umem), GFP_KERNEL_ACCOUNT);
	if (!umem) {
		kvfree(page_list);
		return ERR_PTR(-ENOMEM);
	}

	umem->addr = addr;
	umem->size = size;
	umem->offset = addr & ~PAGE_MASK;
	umem->npages = npages;

	umem->page_list = page_list;
	umem->source_mm = current->mm;
	umem->source_task = current->group_leader;
	get_task_struct(current->group_leader);
	umem->source_user = get_uid(current_user());

	/* mm and RLIMIT_MEMLOCK user task accounting similar to what is
	 * being done in iopt_alloc_pages() and do_update_pinned()
	 * for IOPT_PAGES_ACCOUNT_USER @drivers/iommu/iommufd/pages.c
	 */
	mmgrab(umem->source_mm);

	pinned = pin_user_pages_fast(addr, npages, FOLL_WRITE, page_list);
	if (pinned != npages) {
		umem_dbg(mdev, "pin_user_pages_fast failed %d\n", pinned);
		err = pinned < 0 ? pinned : -ENOMEM;
		goto pin_failed;
	}

	err = inc_user_locked_vm(umem, npages);
	if (err)
		goto pin_failed;

	atomic64_add(npages, &umem->source_mm->pinned_vm);

	err = sg_alloc_table_from_pages(&umem->sgt, page_list, npages, 0,
					npages << PAGE_SHIFT, GFP_KERNEL_ACCOUNT);
	if (err) {
		umem_dbg(mdev, "sg_alloc_table failed: %d\n", err);
		goto sgt_failed;
	}

	umem_dbg(mdev, "\tsgt: size %zu npages %zu sgt.nents (%d)\n",
		 size, npages, umem->sgt.nents);

	err = dma_map_sgtable(mdev->device, &umem->sgt, DMA_BIDIRECTIONAL, 0);
	if (err) {
		umem_dbg(mdev, "dma_map_sgtable failed: %d\n", err);
		goto dma_failed;
	}

	umem_dbg(mdev, "\tsgt: dma_nents %d\n", umem->sgt.nents);
	return umem;

dma_failed:
sgt_failed:
	sg_free_table(&umem->sgt);
	atomic64_sub(npages, &umem->source_mm->pinned_vm);
	dec_user_locked_vm(umem, npages);
pin_failed:
	if (pinned > 0)
		unpin_user_pages(page_list, pinned);
	mmdrop(umem->source_mm);
	free_uid(umem->source_user);
	put_task_struct(umem->source_task);

	kfree(umem);
	kvfree(page_list);
	return ERR_PTR(err);
}

static void mlx5ctl_umem_unpin(struct mlx5ctl_umem_db *umem_db,
			       struct mlx5ctl_umem *umem)
{
	struct mlx5_core_dev *mdev = umem_db->mdev;

	umem_dbg(mdev, "%s: addr %p size %zu npages %zu dma_nents %d\n",
		 __func__, (void *)umem->addr, umem->size, umem->npages,
		 umem->sgt.nents);

	dma_unmap_sgtable(mdev->device, &umem->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(&umem->sgt);

	atomic64_sub(umem->npages, &umem->source_mm->pinned_vm);
	dec_user_locked_vm(umem, umem->npages);
	unpin_user_pages(umem->page_list, umem->npages);
	mmdrop(umem->source_mm);
	free_uid(umem->source_user);
	put_task_struct(umem->source_task);

	kvfree(umem->page_list);
	kfree(umem);
}

static int mlx5ctl_umem_create(struct mlx5_core_dev *mdev,
			       struct mlx5ctl_umem *umem, u32 uid)
{
	u32 out[MLX5_ST_SZ_DW(create_umem_out)] = {};
	int err, inlen, i, n = 0;
	struct scatterlist *sg;
	void *in, *umemptr;
	__be64 *mtt;

	inlen = MLX5_ST_SZ_BYTES(create_umem_in) +
		umem->npages * MLX5_ST_SZ_BYTES(mtt);

	in = kzalloc(inlen, GFP_KERNEL_ACCOUNT);
	if (!in)
		return -ENOMEM;

	MLX5_SET(create_umem_in, in, opcode, MLX5_CMD_OP_CREATE_UMEM);
	MLX5_SET(create_umem_in, in, uid, uid);

	umemptr = MLX5_ADDR_OF(create_umem_in, in, umem);

	MLX5_SET(umem, umemptr, log_page_size,
		 PAGE_SHIFT - MLX5_ADAPTER_PAGE_SHIFT);
	MLX5_SET64(umem, umemptr, num_of_mtt, umem->npages);
	MLX5_SET(umem, umemptr, page_offset, umem->offset);

	umem_dbg(mdev,
		 "UMEM CREATE: log_page_size %d num_of_mtt %lld page_offset %d\n",
		 MLX5_GET(umem, umemptr, log_page_size),
		 MLX5_GET64(umem, umemptr, num_of_mtt),
		 MLX5_GET(umem, umemptr, page_offset));

	mtt = MLX5_ADDR_OF(create_umem_in, in, umem.mtt);
	for_each_sgtable_dma_sg(&umem->sgt, sg, i) {
		u64 dma_addr = sg_dma_address(sg);
		ssize_t len = sg_dma_len(sg);

		for (; n < umem->npages && len > 0; n++, mtt++) {
			*mtt = cpu_to_be64(dma_addr);
			MLX5_SET(mtt, mtt, wr_en, 1);
			MLX5_SET(mtt, mtt, rd_en, 1);
			dma_addr += PAGE_SIZE;
			len -= PAGE_SIZE;
		}
		WARN_ON_ONCE(n == umem->npages && len > 0);
	}

	err = mlx5_cmd_exec(mdev, in, inlen, out, sizeof(out));
	if (err)
		goto out;

	umem->umem_id = MLX5_GET(create_umem_out, out, umem_id);
	umem_dbg(mdev, "\tUMEM CREATED: umem_id %d\n", umem->umem_id);
out:
	kfree(in);
	return err;
}

static void mlx5ctl_umem_destroy(struct mlx5_core_dev *mdev,
				 struct mlx5ctl_umem *umem)
{
	u32 in[MLX5_ST_SZ_DW(destroy_umem_in)] = {};

	MLX5_SET(destroy_umem_in, in, opcode, MLX5_CMD_OP_DESTROY_UMEM);
	MLX5_SET(destroy_umem_in, in, umem_id, umem->umem_id);

	umem_dbg(mdev, "UMEM DESTROY: umem_id %d\n", umem->umem_id);
	mlx5_cmd_exec_in(mdev, destroy_umem, in);
}

int mlx5ctl_umem_reg(struct mlx5ctl_umem_db *umem_db, unsigned long addr,
		     size_t size)
{
	struct mlx5ctl_umem *umem;
	void *ret;
	int err;

	umem = mlx5ctl_umem_pin(umem_db, addr, size);
	if (IS_ERR(umem))
		return PTR_ERR(umem);

	err = mlx5ctl_umem_create(umem_db->mdev, umem, umem_db->uctx_uid);
	if (err)
		goto umem_create_err;

	ret = xa_store(&umem_db->xarray, umem->umem_id, umem, GFP_KERNEL_ACCOUNT);
	if (WARN(xa_is_err(ret), "Failed to store UMEM")) {
		err = xa_err(ret);
		goto xa_store_err;
	}

	return umem->umem_id;

xa_store_err:
	mlx5ctl_umem_destroy(umem_db->mdev, umem);
umem_create_err:
	mlx5ctl_umem_unpin(umem_db, umem);
	return err;
}

int mlx5ctl_umem_unreg(struct mlx5ctl_umem_db *umem_db, u32 umem_id)
{
	struct mlx5ctl_umem *umem;

	umem = xa_erase(&umem_db->xarray, umem_id);
	if (!umem)
		return -ENOENT;

	mlx5ctl_umem_destroy(umem_db->mdev, umem);
	mlx5ctl_umem_unpin(umem_db, umem);
	return 0;
}

struct mlx5ctl_umem_db *mlx5ctl_umem_db_create(struct mlx5_core_dev *mdev,
					       u32 uctx_uid)
{
	struct mlx5ctl_umem_db *umem_db;

	umem_db = kzalloc(sizeof(*umem_db), GFP_KERNEL_ACCOUNT);
	if (!umem_db)
		return ERR_PTR(-ENOMEM);

	xa_init(&umem_db->xarray);
	umem_db->mdev = mdev;
	umem_db->uctx_uid = uctx_uid;

	return umem_db;
}

void mlx5ctl_umem_db_destroy(struct mlx5ctl_umem_db *umem_db)
{
	struct mlx5ctl_umem *umem;
	unsigned long index;

	xa_for_each(&umem_db->xarray, index, umem)
		mlx5ctl_umem_unreg(umem_db, umem->umem_id);

	xa_destroy(&umem_db->xarray);
	kfree(umem_db);
}
