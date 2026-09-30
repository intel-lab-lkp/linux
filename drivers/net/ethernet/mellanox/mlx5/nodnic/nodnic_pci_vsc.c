// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "nodnic_ifc.h"
#include "nodnic_pci_vsc.h"

struct mlx5_nodnic_vsc_ctx {
	struct pci_dev		*pdev;
	u32			 addr;   /* VSC init segment address */
	u32			 offset; /* NODNIC config offset */
};

#define MLX5_NODNIC_ONES32(size)			\
	((size) ? (0xffffffff >> (32 - (size))) : 0)
#define MLX5_NODNIC_MASK32(offset, size)		\
	(MLX5_NODNIC_ONES32(size) << (offset))
#define MLX5_NODNIC_EXTRACT_C(source, offset, size)	\
	((((u32)(source)) >> (offset)) & MLX5_NODNIC_ONES32(size))
#define MLX5_NODNIC_EXTRACT(src, start, len)		\
	(((len) == 32) ? (src) : MLX5_NODNIC_EXTRACT_C(src, start, len))
#define MLX5_NODNIC_MERGE_C(rsrc1, rsrc2, start, len)  \
	((((rsrc2) << (start)) & (MLX5_NODNIC_MASK32((start), (len)))) | \
	((rsrc1) & (~MLX5_NODNIC_MASK32((start), (len)))))
#define MLX5_NODNIC_MERGE(rsrc1, rsrc2, start, len)	\
	(((len) == 32) ? (rsrc2) : \
			 MLX5_NODNIC_MERGE_C(rsrc1, rsrc2, start, len))
#define mlx5_nodnic_vsc_pci_read(ctx, offset, val) \
	pci_read_config_dword((ctx)->pdev, (ctx)->addr + (offset), (val))
#define mlx5_nodnic_vsc_pci_write(ctx, offset, val) \
	pci_write_config_dword((ctx)->pdev, (ctx)->addr + (offset), (val))
#define VSC_MAX_RETRIES 2048

enum {
	MLX5_NODNIC_VSC_CTRL_OFFSET = 0x4,
	MLX5_NODNIC_VSC_COUNTER_OFFSET = 0x8,
	MLX5_NODNIC_VSC_SEMAPHORE_OFFSET = 0xc,
	MLX5_NODNIC_VSC_ADDR_OFFSET = 0x10,
	MLX5_NODNIC_VSC_DATA_OFFSET = 0x14,

	MLX5_NODNIC_VSC_FLAG_BIT_OFFS = 31,
	MLX5_NODNIC_VSC_FLAG_BIT_LEN = 1,

	MLX5_NODNIC_VSC_SYND_BIT_OFFS = 30,
	MLX5_NODNIC_VSC_SYND_BIT_LEN = 1,

	MLX5_NODNIC_VSC_ADDR_BIT_OFFS = 0,
	MLX5_NODNIC_VSC_ADDR_BIT_LEN = 30,

	MLX5_NODNIC_VSC_SPACE_BIT_OFFS = 0,
	MLX5_NODNIC_VSC_SPACE_BIT_LEN = 16,

	MLX5_NODNIC_VSC_SIZE_VLD_BIT_OFFS = 28,
	MLX5_NODNIC_VSC_SIZE_VLD_BIT_LEN = 1,

	MLX5_NODNIC_VSC_STATUS_BIT_OFFS = 29,
	MLX5_NODNIC_VSC_STATUS_BIT_LEN = 3,
};

struct mlx5_nodnic_vsc_ctx *mlx5_nodnic_vsc_init(struct pci_dev *pdev)
{
	struct mlx5_nodnic_vsc_ctx *ctx;

	ctx = kzalloc_obj(struct mlx5_nodnic_vsc_ctx, GFP_KERNEL);
	if (!ctx)
		return NULL;

	ctx->pdev = pdev;
	ctx->addr = pci_find_capability(ctx->pdev, PCI_CAP_ID_VNDR);
	if (!ctx->addr) {
		dev_err(&ctx->pdev->dev,
			"failed to get valid vendor specific ID\n");
		kfree(ctx);
		return NULL;
	}

	return ctx;
}

void mlx5_nodnic_vsc_cleanup(struct mlx5_nodnic_vsc_ctx *ctx)
{
	kfree(ctx);
}

int mlx5_nodnic_vsc_read_offset(struct mlx5_nodnic_vsc_ctx *ctx)
{
	u32 offset;
	int err;

	err = mlx5_nodnic_vsc_read_cpu32(ctx,
					 MLX5_NODNIC_ISEG_NO_DRAM_NIC_OFFSET,
					 &offset);
	if (err) {
		dev_warn(&ctx->pdev->dev,
			 "failed to read no_dram_nic_offset err %d\n", err);
		return err;
	}

	ctx->offset = offset;
	return 0;
}

static int mlx5_nodnic_vsc_gw_lock(struct mlx5_nodnic_vsc_ctx *ctx)
{
	struct pci_dev *pdev = ctx->pdev;
	u32 lock_val, counter = 0;
	int err, retries = 0;

	pci_cfg_access_lock(pdev);

	do {
		if (retries > VSC_MAX_RETRIES) {
			err = -EBUSY;
			goto pci_unlock;
		}
		if (pci_channel_offline(pdev)) {
			err = -EACCES;
			goto pci_unlock;
		}

		/* Check if semaphore is already locked */
		err = mlx5_nodnic_vsc_pci_read(ctx,
					       MLX5_NODNIC_VSC_SEMAPHORE_OFFSET,
					       &lock_val);
		if (err)
			goto pci_unlock;

		if (lock_val) {
			retries++;
			usleep_range(1000, 2000);
			continue;
		}

		/* Read and write counter value, if written value is
		 * the same, semaphore was acquired successfully.
		 */
		err = mlx5_nodnic_vsc_pci_read(ctx,
					       MLX5_NODNIC_VSC_COUNTER_OFFSET,
					       &counter);
		if (err)
			goto pci_unlock;

		err = mlx5_nodnic_vsc_pci_write(
			ctx, MLX5_NODNIC_VSC_SEMAPHORE_OFFSET, counter);
		if (err)
			goto pci_unlock;

		err = mlx5_nodnic_vsc_pci_read(
			ctx, MLX5_NODNIC_VSC_SEMAPHORE_OFFSET, &lock_val);
		if (err)
			goto pci_unlock;

		retries++;
	} while (counter != lock_val);

	return 0;

pci_unlock:
	pci_cfg_access_unlock(pdev);
	return err;
}

static int mlx5_nodnic_vsc_gw_unlock(struct mlx5_nodnic_vsc_ctx *ctx)
{
	int err;

	err = mlx5_nodnic_vsc_pci_write(
		ctx, MLX5_NODNIC_VSC_SEMAPHORE_OFFSET, MLX5_NODNIC_VSC_UNLOCK);
	pci_cfg_access_unlock(ctx->pdev);

	return err;
}

static int
mlx5_nodnic_vsc_gw_set_space(struct mlx5_nodnic_vsc_ctx *ctx, u16 space)
{
	u32 val;
	int err;

	err = mlx5_nodnic_vsc_pci_read(ctx, MLX5_NODNIC_VSC_CTRL_OFFSET, &val);
	if (err)
		return err;

	val = MLX5_NODNIC_MERGE(
		val, space, MLX5_NODNIC_VSC_SPACE_BIT_OFFS,
		MLX5_NODNIC_VSC_SPACE_BIT_LEN);
	err = mlx5_nodnic_vsc_pci_write(ctx, MLX5_NODNIC_VSC_CTRL_OFFSET, val);
	if (err)
		return err;

	err = mlx5_nodnic_vsc_pci_read(ctx, MLX5_NODNIC_VSC_CTRL_OFFSET, &val);
	if (err)
		return err;

	if (MLX5_NODNIC_EXTRACT(
		val, MLX5_NODNIC_VSC_STATUS_BIT_OFFS,
		MLX5_NODNIC_VSC_STATUS_BIT_LEN) == 0)
		return -EINVAL;

	return 0;
}

int mlx5_nodnic_vsc_context_acquire(struct mlx5_nodnic_vsc_ctx *ctx)
{
	int err;

	err = mlx5_nodnic_vsc_gw_lock(ctx);
	if (err) {
		dev_warn(&ctx->pdev->dev, "failed to lock vsc, err %d\n", err);
		return err;
	}

	err = mlx5_nodnic_vsc_gw_set_space(ctx, MLX5_NODNIC_VSC_SPACE_NODNIC);
	if (err) {
		dev_warn(&ctx->pdev->dev,
			 "failed to set vsc space, err %d\n", err);
		mlx5_nodnic_vsc_gw_unlock(ctx);
		return err;
	}

	return err;
}

void mlx5_nodnic_vsc_context_release(struct mlx5_nodnic_vsc_ctx *ctx)
{
	mlx5_nodnic_vsc_gw_unlock(ctx);
}

static int mlx5_nodnic_vsc_wait_on_flag(struct mlx5_nodnic_vsc_ctx *ctx,
					u8 expected_val)
{
	int err, retries = 0;
	u32 flag;

	do {
		if (retries > VSC_MAX_RETRIES)
			return -EBUSY;

		err = mlx5_nodnic_vsc_pci_read(ctx,
					       MLX5_NODNIC_VSC_ADDR_OFFSET,
					       &flag);
		if (err)
			return err;
		flag = MLX5_NODNIC_EXTRACT(flag,
					   MLX5_NODNIC_VSC_FLAG_BIT_OFFS,
					   MLX5_NODNIC_VSC_FLAG_BIT_LEN);
		retries++;

		if ((retries & 0xf) == 0)
			usleep_range(1000, 2000);

	} while (flag != expected_val);

	return 0;
}

static int mlx5_nodnic_vsc_read(struct mlx5_nodnic_vsc_ctx *ctx,
				unsigned int address, u32 *data)
{
	int err;

	/*  Write address and CLEAR flag and syndrome.
	 *  We mask the real_address to ensure the Flag and Syndrome bit
	 *  positions are 0x0. Assuming MLX5_NODNIC_VSC_SYND_BIT_OFFS is
	 *  the start of the flag/syndrome field.
	 */
	u32 mask = ~(((1U << MLX5_NODNIC_VSC_FLAG_BIT_LEN) - 1) <<
		     MLX5_NODNIC_VSC_SYND_BIT_OFFS) &
		   ~(((1U << MLX5_NODNIC_VSC_SYND_BIT_LEN) - 1) <<
		     (MLX5_NODNIC_VSC_SYND_BIT_OFFS +
		      MLX5_NODNIC_VSC_FLAG_BIT_LEN));

	/* Apply the mask to clear the bits */
	address &= mask;

	err = mlx5_nodnic_vsc_pci_write(ctx,
					MLX5_NODNIC_VSC_ADDR_OFFSET,
					address);
	if (err)
		goto out;

	err = mlx5_nodnic_vsc_wait_on_flag(ctx, 1);
	if (err)
		goto out;

	err = mlx5_nodnic_vsc_pci_read(ctx, MLX5_NODNIC_VSC_DATA_OFFSET, data);
out:
	return err;
}

static int mlx5_nodnic_vsc_write(struct mlx5_nodnic_vsc_ctx *ctx,
				 unsigned int address, u32 data)
{
	u32 clear_mask;
	int err;

	err = mlx5_nodnic_vsc_pci_write(ctx, MLX5_NODNIC_VSC_DATA_OFFSET, data);
	if (err)
		goto out;

	/* Prepare the address register.
	 * We must:
	 * 1. Set the Address bits.
	 * 2. Set the Flag bit to 0x1.
	 * 3. Clear the Syndrome bits to 0x0.
	 */

	clear_mask = ~(((1U << MLX5_NODNIC_VSC_FLAG_BIT_LEN) - 1) <<
		       MLX5_NODNIC_VSC_FLAG_BIT_OFFS) &
		     ~(((1U << MLX5_NODNIC_VSC_SYND_BIT_LEN) - 1) <<
		       MLX5_NODNIC_VSC_SYND_BIT_OFFS);

	address &= clear_mask;
	address |= (1U << MLX5_NODNIC_VSC_FLAG_BIT_OFFS);

	err = mlx5_nodnic_vsc_pci_write(ctx,
					MLX5_NODNIC_VSC_ADDR_OFFSET,
					address);
	if (err)
		return err;

	/* Wait for the hardware to clear the flag to 0x0,
	 * indicating the write operation has completed.
	 */
	err = mlx5_nodnic_vsc_wait_on_flag(ctx, 0);

out:
	return err;
}

static u32 mlx5_nodnic_le32_to_be32(u32 data)
{
	return cpu_to_be32(le32_to_cpu(data));
}

static u32 mlx5_nodnic_be32_to_le32(u32 data)
{
	return cpu_to_le32(be32_to_cpu(data));
}

int mlx5_nodnic_vsc_read_be32(struct mlx5_nodnic_vsc_ctx *ctx,
			      unsigned int address, u32 *data)
{
	int err;

	err = mlx5_nodnic_vsc_read(ctx, address, data);
	if (!err)
		*data = mlx5_nodnic_le32_to_be32(*data);

	return err;
}

int mlx5_nodnic_vsc_read_cpu32(struct mlx5_nodnic_vsc_ctx *ctx,
			       unsigned int address, u32 *data)
{
	int err;

	err = mlx5_nodnic_vsc_read(ctx, address, data);
	if (!err)
		*data = le32_to_cpu(*data);

	return err;
}

int mlx5_nodnic_vsc_write_be32(struct mlx5_nodnic_vsc_ctx *ctx,
			       unsigned int address, u32 data)
{
	data = mlx5_nodnic_be32_to_le32(data);

	return mlx5_nodnic_vsc_write(ctx, address, data);
}

int mlx5_nodnic_vsc_cfg_read_be32(struct mlx5_nodnic_vsc_ctx *ctx,
				  unsigned int address, u32 *data)
{
	int err;

	err = mlx5_nodnic_vsc_read(ctx, ctx->offset + address, data);
	if (err)
		return err;

	*data = mlx5_nodnic_le32_to_be32(*data);

	return 0;
}

int mlx5_nodnic_vsc_cfg_write_be32(struct mlx5_nodnic_vsc_ctx *ctx,
				   unsigned int address, u32 data)
{
	data = mlx5_nodnic_be32_to_le32(data);

	return mlx5_nodnic_vsc_write(ctx, ctx->offset + address, data);
}

int mlx5_nodnic_vsc_cfg_read_cpu32(struct mlx5_nodnic_vsc_ctx *ctx,
				   unsigned int address, u32 *data)
{
	int err;

	err = mlx5_nodnic_vsc_read(ctx, ctx->offset + address, data);
	if (err)
		return err;

	*data = le32_to_cpu(*data);

	return 0;
}

int mlx5_nodnic_vsc_cfg_write_cpu32(struct mlx5_nodnic_vsc_ctx *ctx,
				    unsigned int address, u32 data)
{
	data = cpu_to_le32(data);

	return mlx5_nodnic_vsc_write(ctx, ctx->offset + address, data);
}
