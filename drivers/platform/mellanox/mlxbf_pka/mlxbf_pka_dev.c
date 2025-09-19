// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved.

#include <linux/bitfield.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/types.h>

#include "mlxbf_pka_dev.h"

struct mlxbf_pka_dev_gbl_config_t mlxbf_pka_gbl_config;

/* Global PKA shim resource info table. */
static struct mlxbf_pka_dev_gbl_shim_res_info_t mlxbf_pka_gbl_res_tbl[MLXBF_PKA_MAX_NUM_IO_BLOCKS];

/* Add the resource to the global resource table. */
static int mlxbf_pka_dev_add_resource(struct mlxbf_pka_dev_res_t *res_ptr, u32 shim_idx)
{
	u8 res_cnt;

	res_cnt = mlxbf_pka_gbl_res_tbl[shim_idx].res_cnt;
	if (res_cnt >= MLXBF_PKA_DEV_SHIM_RES_CNT)
		return -ENOMEM;

	mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_cnt] = res_ptr;
	mlxbf_pka_gbl_res_tbl[shim_idx].res_cnt++;

	return 0;
}

/* Remove the resource from the global resource table. */
static int mlxbf_pka_dev_put_resource(struct mlxbf_pka_dev_res_t *res, u32 shim_idx)
{
	struct mlxbf_pka_dev_res_t *res_ptr;
	u8 res_idx;

	for (res_idx = 0; res_idx < MLXBF_PKA_DEV_SHIM_RES_CNT; res_idx++) {
		res_ptr = mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
		if (!res_ptr || strcmp(res_ptr->name, res->name))
			continue;

		mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_idx] = NULL;
		mlxbf_pka_gbl_res_tbl[shim_idx].res_cnt--;
		break;
	}

	/*
	 * Check whether the resource shares the same memory map; If so, the memory
	 * map shouldn't be released.
	 */
	for (res_idx = 0; res_idx < MLXBF_PKA_DEV_SHIM_RES_CNT; res_idx++) {
		res_ptr = mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
		if (res_ptr && res_ptr->base == res->base)
			return -EBUSY;
	}

	return 0;
}

static void __iomem *mlxbf_pka_dev_get_resource_ioaddr(u64 res_base, u32 shim_idx)
{
	struct mlxbf_pka_dev_res_t *res_ptr;
	u8 res_cnt, res_idx;

	res_cnt = mlxbf_pka_gbl_res_tbl[shim_idx].res_cnt;
	if (!res_cnt)
		return NULL;

	for (res_idx = 0; res_idx < res_cnt; res_idx++) {
		res_ptr = mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
		if (res_ptr->base == res_base)
			return res_ptr->ioaddr;
	}

	return NULL;
}

/* Set PKA device resource config and map io memory if needed. */
static int mlxbf_pka_dev_set_resource_config(struct device *dev,
					     struct mlxbf_pka_dev_shim_s *shim,
					     struct mlxbf_pka_dev_res_t *res_ptr,
					     u64 res_base,
					     u64 res_size,
					     u64 res_type,
					     char *res_name)
{
	if (res_ptr->status == MLXBF_PKA_DEV_RES_STATUS_MAPPED)
		return -EPERM;

	switch (res_type) {
	case MLXBF_PKA_DEV_RES_TYPE_REG:
		res_ptr->base = res_base;
		break;
	case MLXBF_PKA_DEV_RES_TYPE_MEM:
		res_ptr->base = shim->mem_res.eip154_base + res_base;
		break;
	default:
		return -EINVAL;
	}

	res_ptr->size = res_size;
	res_ptr->type = res_type;
	res_ptr->name = res_name;
	res_ptr->status = MLXBF_PKA_DEV_RES_STATUS_UNMAPPED;
	res_ptr->ioaddr = mlxbf_pka_dev_get_resource_ioaddr(res_ptr->base, shim->shim_id);
	if (!res_ptr->ioaddr) {
		if (!devm_request_mem_region(dev, res_ptr->base, res_ptr->size, res_ptr->name)) {
			dev_err(dev, "failed to get io memory region\n");
			return -EPERM;
		}

		res_ptr->ioaddr = devm_ioremap(dev, res_ptr->base, res_ptr->size);
		if (!res_ptr->ioaddr) {
			dev_err(dev, "unable to map io memory into CPU space\n");
			return -ENOMEM;
		}
	}

	res_ptr->status = MLXBF_PKA_DEV_RES_STATUS_MAPPED;

	if (!res_ptr->ioaddr || mlxbf_pka_dev_add_resource(res_ptr, shim->shim_id)) {
		dev_err(dev, "unable to map io memory\n");
		return -ENOMEM;
	}

	return 0;
}

/* Unset PKA device resource config - unmap io memory if needed. */
void mlxbf_pka_dev_unset_resource_config(struct device *dev,
					 struct mlxbf_pka_dev_shim_s *shim,
					 struct mlxbf_pka_dev_res_t *res_ptr)
{
	if (res_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED)
		return;

	if (!res_ptr->ioaddr)
		return;

	if (-EBUSY == mlxbf_pka_dev_put_resource(res_ptr, shim->shim_id))
		return;

	dev_dbg(dev, "PKA device resource released\n");
	res_ptr->status = MLXBF_PKA_DEV_RES_STATUS_UNMAPPED;
}

/*
 * mlxbf_pka_dev_create_shim - Create shim.
 *
 * Set shim parameters and configure shim resources.
 *
 * Return: 0 on success, a negative error code on failure.
 */
static int mlxbf_pka_dev_create_shim(struct device *dev,
				     struct mlxbf_pka_dev_shim_s *shim,
				     u32 shim_id,
				     u8 split,
				     struct mlxbf_pka_dev_mem_res *mem_res)
{
	u64 reg_base;
	u64 reg_size;
	int ret;

	if (shim->status == MLXBF_PKA_SHIM_STATUS_CREATED)
		return 0;

	if (shim->status != MLXBF_PKA_SHIM_STATUS_UNDEFINED) {
		dev_err(dev, "PKA device must be undefined\n");
		return -EPERM;
	}

	if (shim_id > MLXBF_PKA_MAX_NUM_IO_BLOCKS - 1) {
		dev_err(dev, "invalid shim identifier\n");
		return -EINVAL;
	}

	shim->shim_id = shim_id;
	shim->mem_res = *mem_res;

	if (split)
		shim->window_ram_split = MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_ENABLED;
	else
		shim->window_ram_split = MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_DISABLED;

	/* Set PKA device Buffer RAM config. */
	ret = mlxbf_pka_dev_set_resource_config(dev,
						shim,
						&shim->resources.buffer_ram,
						MLXBF_PKA_BUFFER_RAM_BASE,
						MLXBF_PKA_BUFFER_RAM_SIZE,
						MLXBF_PKA_DEV_RES_TYPE_MEM,
						"MLXBF_PKA_BUFFER_RAM");
	if (ret) {
		dev_err(dev, "unable to set Buffer RAM config\n");
		return ret;
	}

	/* Set PKA device Master Controller register. */
	reg_size = PAGE_SIZE;
	reg_base = mlxbf_pka_dev_get_register_base(shim->mem_res.eip154_base,
						   MLXBF_PKA_MASTER_SEQ_CTRL_ADDR);
	ret = mlxbf_pka_dev_set_resource_config(dev,
						shim,
						&shim->resources.master_seq_ctrl,
						reg_base,
						reg_size,
						MLXBF_PKA_DEV_RES_TYPE_REG,
						"MLXBF_PKA_MASTER_SEQ_CTRL");
	if (ret) {
		dev_err(dev, "unable to set Master Controller register config\n");
		return ret;
	}

	/* Set PKA device AIC registers. */
	reg_size = PAGE_SIZE;
	reg_base = mlxbf_pka_dev_get_register_base(shim->mem_res.eip154_base,
						   MLXBF_PKA_AIC_POL_CTRL_ADDR);
	ret = mlxbf_pka_dev_set_resource_config(dev,
						shim,
						&shim->resources.aic_csr,
						reg_base,
						reg_size,
						MLXBF_PKA_DEV_RES_TYPE_REG,
						"MLXBF_PKA_AIC_CSR");
	if (ret) {
		dev_err(dev, "unable to set AIC registers config\n");
		return ret;
	}

	shim->status = MLXBF_PKA_SHIM_STATUS_CREATED;

	return ret;
}

/* Delete shim and unset shim resources. */
static int mlxbf_pka_dev_delete_shim(struct device *dev, struct mlxbf_pka_dev_shim_s *shim)
{
	struct mlxbf_pka_dev_res_t *res_master_seq_ctrl, *res_aic_csr;
	struct mlxbf_pka_dev_res_t *res_buffer_ram;

	dev_dbg(dev, "PKA device delete shim\n");

	if (shim->status == MLXBF_PKA_SHIM_STATUS_UNDEFINED)
		return 0;

	if (shim->status != MLXBF_PKA_SHIM_STATUS_FINALIZED &&
	    shim->status != MLXBF_PKA_SHIM_STATUS_CREATED) {
		dev_err(dev, "PKA device status must be finalized\n");
		return -EPERM;
	}

	res_buffer_ram = &shim->resources.buffer_ram;
	res_master_seq_ctrl = &shim->resources.master_seq_ctrl;
	res_aic_csr = &shim->resources.aic_csr;

	mlxbf_pka_dev_unset_resource_config(dev, shim, res_buffer_ram);
	mlxbf_pka_dev_unset_resource_config(dev, shim, res_master_seq_ctrl);
	mlxbf_pka_dev_unset_resource_config(dev, shim, res_aic_csr);

	shim->status = MLXBF_PKA_SHIM_STATUS_UNDEFINED;

	return 0;
}

/*
 * Initialize PKA IO block referred to as shim. It configures shim's
 * parameters and prepares resources by mapping corresponding memory. The
 * function also configures shim registers and loads firmware to shim
 * internal rams. The struct mlxbf_pka_dev_shim_s passed as input is also
 * an output. It returns 0 on success, a negative error code on failure.
 */
static int mlxbf_pka_dev_init_shim(struct device *dev, struct mlxbf_pka_dev_shim_s *shim)
{
	int ret;

	if (shim->status != MLXBF_PKA_SHIM_STATUS_CREATED) {
		dev_err(dev, "PKA device must be created\n");
		return -EPERM;
	}

	ret = devm_mutex_init(dev, &shim->mutex);
	if (ret)
		return ret;

	shim->status = MLXBF_PKA_SHIM_STATUS_INITIALIZED;

	return ret;
}

/* Release a given shim. */
static int mlxbf_pka_dev_release_shim(struct device *dev, struct mlxbf_pka_dev_shim_s *shim)
{
	int ret = 0;

	if (shim->status != MLXBF_PKA_SHIM_STATUS_INITIALIZED &&
	    shim->status != MLXBF_PKA_SHIM_STATUS_STOPPED) {
		dev_err(dev, "PKA device must be initialized or stopped\n");
		return -EPERM;
	}

	shim->status = MLXBF_PKA_SHIM_STATUS_FINALIZED;

	return ret;
}

/* Return the shim associated with the given identifier. */
struct mlxbf_pka_dev_shim_s *mlxbf_pka_dev_get_shim(u32 shim_id)
{
	return mlxbf_pka_gbl_config.dev_shims[shim_id];
}

int mlxbf_pka_dev_register_shim(struct device *dev,
				u32 shim_id,
				struct mlxbf_pka_dev_mem_res *mem_res,
				struct mlxbf_pka_dev_shim_s **shim)
{
	struct mlxbf_pka_dev_shim_s *shim_ptr;
	u8 split;
	int ret;

	if (!shim)
		return -EINVAL;

	dev_dbg(dev, "register shim id=%u\n", shim_id);

	shim_ptr = devm_kzalloc(dev, sizeof(*shim_ptr), GFP_KERNEL);
	if (!shim_ptr)
		return -ENOMEM;

	/*
	 * Shim state MUST be set to undefined before calling
	 * 'mlxbf_pka_dev_create_shim' function.
	 */
	shim_ptr->status = MLXBF_PKA_SHIM_STATUS_UNDEFINED;

	/* Set the Window RAM user mode. */
	split = MLXBF_PKA_SPLIT_WINDOW_RAM_MODE;

	/* Create PKA shim. */
	ret = mlxbf_pka_dev_create_shim(dev, shim_ptr, shim_id, split, mem_res);
	if (ret) {
		dev_err(dev, "failed to create shim %u\n", shim_id);
		mlxbf_pka_dev_delete_shim(dev, shim_ptr);
		goto exit_create_shim;
	}

	/* Initialize PKA shim. */
	ret = mlxbf_pka_dev_init_shim(dev, shim_ptr);
	if (ret) {
		dev_err(dev, "failed to init shim %u\n", shim_id);
		goto exit_init_shim;
	}

	mlxbf_pka_gbl_config.dev_shims[shim_ptr->shim_id] = shim_ptr;
	mlxbf_pka_gbl_config.dev_shims_cnt += 1;

	*shim = shim_ptr;
	return 0;

exit_init_shim:
	mlxbf_pka_dev_release_shim(dev, shim_ptr);

exit_create_shim:
	mlxbf_pka_dev_delete_shim(dev, shim_ptr);
	return ret;
}

int mlxbf_pka_dev_unregister_shim(struct device *dev, struct mlxbf_pka_dev_shim_s *shim)
{
	int ret;

	if (!shim)
		return -EINVAL;

	mlxbf_pka_gbl_config.dev_shims[shim->shim_id] = NULL;
	mlxbf_pka_gbl_config.dev_shims_cnt -= 1;

	/* Release shim. */
	ret = mlxbf_pka_dev_release_shim(dev, shim);
	if (ret)
		return ret;

	/* Delete shim. */
	return mlxbf_pka_dev_delete_shim(dev, shim);
}
