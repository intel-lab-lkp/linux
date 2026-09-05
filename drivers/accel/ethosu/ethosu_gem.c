// SPDX-License-Identifier: GPL-2.0-only or MIT
/* Copyright 2025 Arm, Ltd. */

#include <linux/bitmap.h>
#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include <drm/ethosu_accel.h>

#include "ethosu_device.h"
#include "ethosu_gem.h"

static void ethosu_gem_free_object(struct drm_gem_object *obj)
{
	struct ethosu_gem_object *bo = to_ethosu_bo(obj);

	kfree(bo->info);
	drm_gem_free_mmap_offset(&bo->base.base);
	drm_gem_dma_free(&bo->base);
}

static int ethosu_gem_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct ethosu_gem_object *bo = to_ethosu_bo(obj);

	/* Don't allow mmap on objects that have the NO_MMAP flag set. */
	if (bo->flags & DRM_ETHOSU_BO_NO_MMAP)
		return -EINVAL;

	return drm_gem_dma_object_mmap(obj, vma);
}

static const struct drm_gem_object_funcs ethosu_gem_funcs = {
	.free = ethosu_gem_free_object,
	.print_info = drm_gem_dma_object_print_info,
	.get_sg_table = drm_gem_dma_object_get_sg_table,
	.vmap = drm_gem_dma_object_vmap,
	.mmap = ethosu_gem_mmap,
	.vm_ops = &drm_gem_dma_vm_ops,
};

/**
 * ethosu_gem_create_object - Implementation of driver->gem_create_object.
 * @ddev: DRM device
 * @size: Size in bytes of the memory the object will reference
 *
 * This lets the GEM helpers allocate object structs for us, and keep
 * our BO stats correct.
 */
struct drm_gem_object *ethosu_gem_create_object(struct drm_device *ddev, size_t size)
{
	struct ethosu_gem_object *obj;

	obj = kzalloc_obj(*obj);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->base.base.funcs = &ethosu_gem_funcs;
	return &obj->base.base;
}

/**
 * ethosu_gem_create_with_handle() - Create a GEM object and attach it to a handle.
 * @file: DRM file.
 * @ddev: DRM device.
 * @size: Size of the GEM object to allocate.
 * @flags: Combination of drm_ethosu_bo_flags flags.
 * @handle: Pointer holding the handle pointing to the new GEM object.
 *
 * Return: Zero on success
 */
int ethosu_gem_create_with_handle(struct drm_file *file,
				  struct drm_device *ddev,
				  u64 *size, u32 flags, u32 *handle)
{
	struct drm_gem_dma_object *mem;
	struct ethosu_gem_object *bo;
	int ret;

	mem = drm_gem_dma_create(ddev, *size);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	bo = to_ethosu_bo(&mem->base);
	bo->flags = flags;

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file, &mem->base, handle);
	if (!ret)
		*size = bo->base.base.size;

	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&mem->base);

	return ret;
}

struct dma {
	s8 region;
	s8 mode;
	u64 len;
	u64 offset;
	s64 stride[2];
};

struct dma_state {
	u16 size0;
	u16 size1;
	struct dma src;
	struct dma dst;
};

struct buffer {
	u64 base;
	u32 length;
	s8 region;
};

struct feat_matrix {
	u64 base[4];
	s64 stride_x;
	s64 stride_y;
	s64 stride_c;
	s8 region;
	u8 broadcast;
	u16 stride_kernel;
	u16 precision;
	u16 depth;
	u16 width;
	u16 width0;
	u16 height[3];
	u8 pad_top;
	u8 pad_left;
	u8 pad_bottom;
	u8 pad_right;
};

#define NPU_CMD0_REGS	0x200
#define NPU_CMD1_REGS	0x100

struct cmd_state {
	DECLARE_BITMAP(cmd0, NPU_CMD0_REGS);
	DECLARE_BITMAP(cmd1, NPU_CMD1_REGS);
	struct dma_state dma;
	struct buffer scale[2];
	struct buffer weight[4];
	struct feat_matrix ofm;
	struct feat_matrix ifm;
	struct feat_matrix ifm2;
};

static void cmd_state_init(struct cmd_state *st)
{
	memset(st, 0, sizeof(*st));
}

static void cmd_state_set_reg(struct cmd_state *st, u16 cmd)
{
	u16 reg = cmd & ~NPU_CMD_CTRL_CMD1;

	if (cmd & NPU_CMD_CTRL_CMD1) {
		if (reg < NPU_CMD1_REGS)
			__set_bit(reg, st->cmd1);
	} else if (reg < NPU_CMD0_REGS) {
		__set_bit(reg, st->cmd0);
	}
}

static bool cmd_state_reg_is_set(struct cmd_state *st, u16 cmd)
{
	u16 reg = cmd & ~NPU_CMD_CTRL_CMD1;

	if (cmd & NPU_CMD_CTRL_CMD1)
		return reg < NPU_CMD1_REGS && test_bit(reg, st->cmd1);

	return reg < NPU_CMD0_REGS && test_bit(reg, st->cmd0);
}

static u64 cmd_to_addr(u32 *cmd)
{
	return (((u64)cmd[0] & 0xff0000) << 16) | cmd[1];
}

static bool dma_use_src_stride(struct ethosu_device *edev,
			       const struct dma_state *dma_st, const struct dma *dma)
{
	return ethosu_is_u65(edev) || dma == &dma_st->src;
}

static bool dma_params_valid(struct ethosu_device *edev, struct cmd_state *st,
			     const struct dma_state *dma_st,
			     const struct dma *dma,
			     u16 region_cmd, u16 addr_cmd)
{
	s8 mode = dma->mode;

	if (!cmd_state_reg_is_set(st, region_cmd) ||
	    !cmd_state_reg_is_set(st, addr_cmd) ||
	    !cmd_state_reg_is_set(st, NPU_SET_DMA0_LEN) || mode < 0 || mode > 2)
		return false;

	if (mode >= 1 &&
	    !cmd_state_reg_is_set(st, dma_use_src_stride(edev, dma_st, dma) ?
				  NPU_SET_DMA0_SRC_STRIDE0 :
				  NPU_SET_DMA0_DST_STRIDE0))
		return U64_MAX;
	if (mode == 2 &&
	    !cmd_state_reg_is_set(st, dma_use_src_stride(edev, dma_st, dma) ?
				  NPU_SET_DMA0_SRC_STRIDE1 :
				  NPU_SET_DMA0_DST_STRIDE1))
		return U64_MAX;

	if (mode >= 1 &&
	    (!cmd_state_reg_is_set(st, NPU_SET_DMA0_SIZE0) || !dma_st->size0))
		return false;
	if (mode == 2 &&
	    (!cmd_state_reg_is_set(st, NPU_SET_DMA0_SIZE1) || !dma_st->size1))
		return false;

	return true;
}

static u64 dma_length(struct ethosu_device *edev,
		      struct ethosu_validated_cmdstream_info *info,
		      struct cmd_state *st, struct dma_state *dma_st,
		      struct dma *dma, u16 region_cmd, u16 addr_cmd)
{
	s8 mode = dma->mode;
	u64 len = dma->len;

	if (!dma_params_valid(edev, st, dma_st, dma, region_cmd, addr_cmd))
		return U64_MAX;

	if (mode >= 1) {
		if (dma->stride[0] < 0 && (u64)(-dma->stride[0]) > len)
			return U64_MAX;
		len += dma->stride[0];
		if (check_mul_overflow(len, (u64)dma_st->size0, &len))
			return U64_MAX;
	}
	if (mode == 2) {
		if (dma->stride[1] < 0 && (u64)(-dma->stride[1]) > len)
			return U64_MAX;
		len += dma->stride[1];
		if (check_mul_overflow(len, (u64)dma_st->size1, &len))
			return U64_MAX;
	}
	if (dma->region >= 0) {
		u64 end;

		if (check_add_overflow(len, dma->offset, &end))
			return U64_MAX;
		info->region_size[dma->region] = max(info->region_size[dma->region], end);
	}

	return len;
}

static bool feat_matrix_chained(struct ethosu_device *edev, struct feat_matrix *fm)
{
	u32 storage = fm->precision >> 14;

	return !ethosu_is_u65(edev) && storage == 2;
}

enum feat_matrix_type {
	FEAT_MATRIX_IFM,
	FEAT_MATRIX_OFM,
	FEAT_MATRIX_IFM2,
};

static u16 feat_matrix_base_cmd(enum feat_matrix_type type)
{
	switch (type) {
	case FEAT_MATRIX_IFM:
		return NPU_SET_IFM_BASE0;
	case FEAT_MATRIX_OFM:
		return NPU_SET_OFM_BASE0;
	case FEAT_MATRIX_IFM2:
		return NPU_SET_IFM2_BASE0;
	}

	return 0;
}

static int feat_matrix_validate(struct ethosu_device *edev,
				struct cmd_state *st, struct feat_matrix *fm,
				enum feat_matrix_type type)
{
	u32 format;
	u16 stride_cmd;

	switch (type) {
	case FEAT_MATRIX_IFM:
		if (!cmd_state_reg_is_set(st, NPU_SET_IFM_REGION) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_PRECISION) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_DEPTH_M1))
			return -EINVAL;
		if (feat_matrix_chained(edev, fm))
			return 0;
		if (!cmd_state_reg_is_set(st, NPU_SET_IFM_WIDTH0_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_HEIGHT0_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_HEIGHT1_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_STRIDE_Y))
			return -EINVAL;
		break;
	case FEAT_MATRIX_OFM:
		if (!cmd_state_reg_is_set(st, NPU_SET_OFM_REGION) ||
		    !cmd_state_reg_is_set(st, NPU_SET_OFM_PRECISION) ||
		    !cmd_state_reg_is_set(st, NPU_SET_OFM_DEPTH_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_OFM_WIDTH_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_OFM_HEIGHT_M1))
			return -EINVAL;
		if (feat_matrix_chained(edev, fm))
			return 0;
		if (!cmd_state_reg_is_set(st, NPU_SET_OFM_WIDTH0_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_OFM_HEIGHT0_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_OFM_HEIGHT1_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_OFM_STRIDE_Y))
			return -EINVAL;
		break;
	case FEAT_MATRIX_IFM2:
		if (!cmd_state_reg_is_set(st, NPU_SET_IFM2_REGION) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM2_PRECISION))
			return -EINVAL;
		if (feat_matrix_chained(edev, fm))
			return 0;
		if (!cmd_state_reg_is_set(st, NPU_SET_IFM2_WIDTH0_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM2_HEIGHT0_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM2_HEIGHT1_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM2_STRIDE_Y))
			return -EINVAL;
		break;
	}

	format = (fm->precision >> 6) & 0x3;
	stride_cmd = feat_matrix_base_cmd(type) + (format ? 6 : 4);
	if (!cmd_state_reg_is_set(st, stride_cmd))
		return -EINVAL;

	return 0;
}
static u64 feat_matrix_length(struct ethosu_device *edev,
			      struct ethosu_validated_cmdstream_info *info,
			      struct cmd_state *st, struct feat_matrix *fm,
			      enum feat_matrix_type type,
			      u32 x, u32 y, u32 c, bool ofm)
{
	u32 element_size, storage = ethosu_is_u65(edev) ? 0 : fm->precision >> 14;
	int tile = 0;
	u64 addr;
	u64 offset;

	if (fm->region < 0)
		return U64_MAX;
	if (feat_matrix_validate(edev, st, fm, type))
		return U64_MAX;

	if (feat_matrix_chained(edev, fm))
		return 0;

	switch (storage) {
	case 0:
		if (x >= fm->width0 + 1) {
			x -= fm->width0 + 1;
			tile += 1;
		}
		if (y >= fm->height[tile] + 1) {
			y -= fm->height[tile] + 1;
			tile += 2;
		}
		break;
	case 1:
		if (y >= fm->height[1] + 1) {
			y -= fm->height[1] + 1;
			tile = 2;
		} else if (y >= fm->height[0] + 1) {
			y -= fm->height[0] + 1;
			tile = 1;
		}
		break;
	default:
		return U64_MAX;
	}
	if (!cmd_state_reg_is_set(st, feat_matrix_base_cmd(type) + tile))
		return U64_MAX;

	if (check_mul_overflow(y, (u64)fm->stride_y, &offset) ||
	    check_add_overflow(fm->base[tile], offset, &addr))
		return U64_MAX;

	switch ((fm->precision >> 6) & 0x3) { // format
	case 0: //nhwc:
		element_size = BIT((fm->precision >> (ofm ? 1 : 2)) & 0x3);
		if (check_mul_overflow(x, (u64)fm->stride_x, &offset) ||
		    check_add_overflow(addr, offset, &addr) ||
		    check_mul_overflow(c, element_size, &offset) ||
		    check_add_overflow(addr, offset, &addr))
			return U64_MAX;
		break;
	case 1: //nhcwb16:
		element_size = BIT((fm->precision >> (ofm ? 1 : 2)) & 0x3);

		if (check_mul_overflow(c / 16, (u64)fm->stride_c, &offset) ||
		    check_add_overflow(addr, offset, &addr) ||
		    check_mul_overflow(16 * x + (c & 0xf), element_size, &offset) ||
		    check_add_overflow(addr, offset, &addr))
			return U64_MAX;
		break;
	default:
		return U64_MAX;
	}

	if (check_add_overflow(addr, (u64)element_size, &offset))
		return U64_MAX;

	info->region_size[fm->region] = max(info->region_size[fm->region], offset);

	return addr;
}

static int feat_matrix_check_location(struct ethosu_device *edev,
				      struct ethosu_validated_cmdstream_info *info,
				      struct cmd_state *st, struct feat_matrix *fm,
				      enum feat_matrix_type type, u32 x, u32 y,
				      u32 c, bool ofm, u64 *max_len)
{
	u64 len;

	len = feat_matrix_length(edev, info, st, fm, type, x, y, c, ofm);
	if (len == U64_MAX)
		return -EINVAL;

	*max_len = max(*max_len, len);
	return 0;
}

static int feat_matrix_size(struct ethosu_device *edev,
			    struct ethosu_validated_cmdstream_info *info,
			    struct cmd_state *st, struct feat_matrix *fm,
			    enum feat_matrix_type type,
			    u32 x, u32 y, u32 c, bool ofm, u64 *max_len)
{
	u32 storage = ethosu_is_u65(edev) ? 0 : fm->precision >> 14;
	int ret;

	*max_len = 0;

	if (ethosu_is_u65(edev) || storage == 0) {
		for (int xi = 0; xi < 2; xi++) {
			for (int yi = 0; yi < 2; yi++) {
				ret = feat_matrix_check_location(edev, info, st, fm, type,
								 xi ? x : 0,
								 yi ? y : 0, c, ofm,
								 max_len);
				if (ret)
					return ret;
			}
		}
		return 0;
	}

	if (storage == 1) {
		ret = feat_matrix_check_location(edev, info, st, fm, type, x, 0, c,
						 ofm, max_len);
		if (ret)
			return ret;
		if (fm->height[0] < fm->height[1] && fm->height[1] <= y) {
			ret = feat_matrix_check_location(edev, info, st, fm, type, x,
							 fm->height[1], c, ofm,
							 max_len);
			if (ret)
				return ret;
		}
		if (fm->height[1] < y) {
			ret = feat_matrix_check_location(edev, info, st, fm, type, x,
							 fm->height[1] + 1, c, ofm,
							 max_len);
			if (ret)
				return ret;
		}
		return feat_matrix_check_location(edev, info, st, fm, type, x, y, c,
						  ofm, max_len);
	}

	return feat_matrix_check_location(edev, info, st, fm, type, x, y, c, ofm,
					  max_len);
}

static int buffer_size(struct ethosu_validated_cmdstream_info *info,
		       struct cmd_state *st, struct buffer *buf, s8 region,
		       u16 region_cmd, u16 base_cmd, u16 length_cmd, bool optional)
{
	u64 end;
	bool base_set = cmd_state_reg_is_set(st, base_cmd);
	bool length_set = cmd_state_reg_is_set(st, length_cmd);

	if (optional && !base_set && !length_set)
		return 0;

	if (region < 0 || !cmd_state_reg_is_set(st, region_cmd) ||
	    !base_set || !length_set)
		return -EINVAL;

	if (check_add_overflow(buf->base, (u64)buf->length, &end))
		return -EINVAL;

	info->region_size[region] = max(info->region_size[region], end);

	return 0;
}

static int calc_sizes(struct drm_device *ddev,
		      struct ethosu_validated_cmdstream_info *info,
		      u16 op, struct cmd_state *st,
		      bool ifm, bool ifm2, bool weight, bool scale)
{
	struct ethosu_device *edev = to_ethosu_device(ddev);
	u64 len;
	int ret;

	if (ifm) {
		if (!cmd_state_reg_is_set(st, NPU_SET_KERNEL_WIDTH_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_KERNEL_HEIGHT_M1) ||
		    !cmd_state_reg_is_set(st, NPU_SET_KERNEL_STRIDE) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_PAD_TOP) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_PAD_LEFT) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_PAD_RIGHT) ||
		    !cmd_state_reg_is_set(st, NPU_SET_IFM_PAD_BOTTOM))
			return -EINVAL;
		u32 stride_y = ((st->ifm.stride_kernel >> 8) & 0x2) +
			((st->ifm.stride_kernel >> 1) & 0x1) + 1;
		u32 stride_x = ((st->ifm.stride_kernel >> 5) & 0x2) +
			(st->ifm.stride_kernel & 0x1) + 1;
		u32 dilation_y = 1 + !!(st->ifm.stride_kernel &
					 NPU_KERNEL_DILATION_Y);
		u32 dilation_x = 1 + !!(st->ifm.stride_kernel &
					 NPU_KERNEL_DILATION_X);
		s32 ifm_height = st->ofm.height[2] * stride_y +
			st->ifm.height[2] * dilation_y -
			(st->ifm.pad_top + st->ifm.pad_bottom);
		s32 ifm_width = st->ofm.width * stride_x +
			st->ifm.width * dilation_x -
			(st->ifm.pad_left + st->ifm.pad_right);

		if (ifm_height < 0 || ifm_width < 0)
			return -EINVAL;

		ret = feat_matrix_size(edev, info, st, &st->ifm, FEAT_MATRIX_IFM,
				       ifm_width, ifm_height, st->ifm.depth, false,
				       &len);
		dev_dbg(ddev->dev, "op %d: IFM:%d:0x%llx-0x%llx\n",
			op, st->ifm.region, st->ifm.base[0], len);
		if (ret)
			return ret;
	}

	if (ifm2) {
		ret = feat_matrix_size(edev, info, st, &st->ifm2, FEAT_MATRIX_IFM2,
				       st->ifm.depth, 0, st->ofm.depth, false, &len);
		dev_dbg(ddev->dev, "op %d: IFM2:%d:0x%llx-0x%llx\n",
			op, st->ifm2.region, st->ifm2.base[0], len);
		if (ret)
			return ret;
	}

	if (weight) {
		dev_dbg(ddev->dev, "op %d: W:%d:0x%llx-0x%llx\n",
			op, st->weight[0].region, st->weight[0].base,
			st->weight[0].base + st->weight[0].length - 1);
		if (buffer_size(info, st, &st->weight[0], st->weight[0].region,
				NPU_SET_WEIGHT_REGION, NPU_SET_WEIGHT_BASE,
				NPU_SET_WEIGHT_LENGTH, false))
			return -EINVAL;

		if (buffer_size(info, st, &st->weight[1], st->weight[0].region,
				NPU_SET_WEIGHT_REGION, NPU_SET_WEIGHT1_BASE,
				NPU_SET_WEIGHT1_LENGTH, true) ||
		    buffer_size(info, st, &st->weight[3], st->weight[0].region,
				NPU_SET_WEIGHT_REGION, NPU_SET_WEIGHT3_BASE,
				NPU_SET_WEIGHT3_LENGTH, true))
			return -EINVAL;
		if (!ethosu_is_u65(edev) &&
		    buffer_size(info, st, &st->weight[2], st->weight[0].region,
				NPU_SET_WEIGHT_REGION, NPU_SET_WEIGHT2_BASE,
				NPU_SET_WEIGHT2_LENGTH, true))
			return -EINVAL;
	}

	if (scale) {
		dev_dbg(ddev->dev, "op %d: S:%d:0x%llx-0x%llx\n",
			op, st->scale[0].region, st->scale[0].base,
			st->scale[0].base + st->scale[0].length - 1);
		if (buffer_size(info, st, &st->scale[0], st->scale[0].region,
				NPU_SET_SCALE_REGION, NPU_SET_SCALE_BASE,
				NPU_SET_SCALE_LENGTH, false))
			return -EINVAL;

		if (ethosu_is_u65(edev) &&
		    buffer_size(info, st, &st->scale[1], st->scale[0].region,
				NPU_SET_SCALE_REGION, NPU_SET_SCALE1_BASE,
				NPU_SET_SCALE1_LENGTH, true))
			return -EINVAL;
	}

	ret = feat_matrix_size(edev, info, st, &st->ofm, FEAT_MATRIX_OFM,
			       st->ofm.width, st->ofm.height[2], st->ofm.depth,
			       true, &len);
	dev_dbg(ddev->dev, "op %d: OFM:%d:0x%llx-0x%llx\n",
		op, st->ofm.region, st->ofm.base[0], len);
	if (ret)
		return ret;
	if (!feat_matrix_chained(edev, &st->ofm))
		info->output_region[st->ofm.region] = true;

	return 0;
}

static int calc_sizes_elemwise(struct drm_device *ddev,
			       struct ethosu_validated_cmdstream_info *info,
			       u16 op, struct cmd_state *st,
			       bool ifm, bool ifm2)
{
	struct ethosu_device *edev = to_ethosu_device(ddev);
	u32 height, width, depth;
	u64 len;
	int ret;

	if (ifm) {
		height = st->ifm.broadcast & 0x1 ? 0 : st->ofm.height[2];
		width = st->ifm.broadcast & 0x2 ? 0 : st->ofm.width;
		depth = st->ifm.broadcast & 0x4 ? 0 : st->ofm.depth;

		ret = feat_matrix_size(edev, info, st, &st->ifm, FEAT_MATRIX_IFM,
				       width, height, depth, false, &len);
		dev_dbg(ddev->dev, "op %d: IFM:%d:0x%llx-0x%llx\n",
			op, st->ifm.region, st->ifm.base[0], len);
		if (ret)
			return ret;
	}

	if (ifm2) {
		height = st->ifm2.broadcast & 0x1 ? 0 : st->ofm.height[2];
		width = st->ifm2.broadcast & 0x2 ? 0 : st->ofm.width;
		depth = st->ifm2.broadcast & 0x4 ? 0 : st->ofm.depth;

		ret = feat_matrix_size(edev, info, st, &st->ifm2, FEAT_MATRIX_IFM2,
				       width, height, depth, false, &len);
		dev_dbg(ddev->dev, "op %d: IFM2:%d:0x%llx-0x%llx\n",
			op, st->ifm2.region, st->ifm2.base[0], len);
		if (ret)
			return ret;
	}

	ret = feat_matrix_size(edev, info, st, &st->ofm, FEAT_MATRIX_OFM,
			       st->ofm.width, st->ofm.height[2], st->ofm.depth,
			       true, &len);
	dev_dbg(ddev->dev, "op %d: OFM:%d:0x%llx-0x%llx\n",
		op, st->ofm.region, st->ofm.base[0], len);
	if (ret)
		return ret;
	if (!feat_matrix_chained(edev, &st->ofm))
		info->output_region[st->ofm.region] = true;

	return 0;
}

static int ethosu_gem_cmdstream_copy_and_validate(struct drm_device *ddev,
						  u32 __user *ucmds,
						  struct ethosu_gem_object *bo,
						  u32 size)
{
	struct ethosu_validated_cmdstream_info __free(kfree) *info = kzalloc_obj(*info);
	struct ethosu_device *edev = to_ethosu_device(ddev);
	u32 *bocmds = bo->base.vaddr;
	bool ends_with_stop = false;
	struct cmd_state st;
	int i, ret;

	if (!info)
		return -ENOMEM;
	info->cmd_size = size;

	cmd_state_init(&st);

	for (i = 0; i < size / 4; i++) {
		bool use_ifm, use_ifm2, use_scale;
		u64 dstlen, srclen;
		u16 cmd, param;
		u32 cmds[2];
		u64 addr;

		if (get_user(cmds[0], ucmds++))
			return -EFAULT;

		bocmds[i] = cmds[0];

		cmd = cmds[0];
		param = cmds[0] >> 16;

		if (cmd & NPU_CMD_CTRL_CMD1) {
			if (get_user(cmds[1], ucmds++))
				return -EFAULT;

			i++;
			if (i >= size / 4)
				return -EINVAL;
			bocmds[i] = cmds[1];
			addr = cmd_to_addr(cmds);
		}

		cmd_state_set_reg(&st, cmd);

		switch (cmd) {
		case NPU_OP_BRANCH:
		case NPU_OP_IRQ:
			return -EINVAL;
		case NPU_OP_STOP:
			if (i != size / 4 - 1)
				return -EINVAL;
			ends_with_stop = true;
			break;
		case NPU_OP_DMA_START:
			srclen = dma_length(edev, info, &st, &st.dma, &st.dma.src,
					    NPU_SET_DMA0_SRC_REGION, NPU_SET_DMA0_SRC);
			dstlen = dma_length(edev, info, &st, &st.dma, &st.dma.dst,
					    NPU_SET_DMA0_DST_REGION, NPU_SET_DMA0_DST);
			if (srclen == U64_MAX || dstlen == U64_MAX)
				return -EINVAL;

			if (st.dma.dst.region >= 0)
				info->output_region[st.dma.dst.region] = true;
			dev_dbg(ddev->dev, "cmd: DMA SRC:%d:0x%llx+0x%llx DST:%d:0x%llx+0x%llx\n",
				st.dma.src.region, st.dma.src.offset, srclen,
				st.dma.dst.region, st.dma.dst.offset, dstlen);
			break;
		case NPU_OP_CONV:
			if ((ethosu_is_u65(edev) && param) || (param & ~NPU_OP_CONV_WEIGHTS_IFM2))
				return -EINVAL;

			use_ifm2 = param & NPU_OP_CONV_WEIGHTS_IFM2;
			if (!cmd_state_reg_is_set(&st, NPU_SET_OFM_PRECISION))
				return -EINVAL;
			use_scale = !(st.ofm.precision & 0x100);
			ret = calc_sizes(ddev, info, cmd, &st, true, use_ifm2,
					 !use_ifm2, use_scale);
			if (ret)
				return ret;
			break;
		case NPU_OP_DEPTHWISE:
			if (!cmd_state_reg_is_set(&st, NPU_SET_OFM_PRECISION))
				return -EINVAL;
			use_scale = !(st.ofm.precision & 0x100);
			ret = calc_sizes(ddev, info, cmd, &st, true, false, true,
					 use_scale);
			if (ret)
				return ret;
			break;
		case NPU_OP_POOL:
			use_ifm = param != 0x4;  // pooling mode
			if (!cmd_state_reg_is_set(&st, NPU_SET_OFM_PRECISION))
				return -EINVAL;
			use_scale = !(st.ofm.precision & 0x100);
			ret = calc_sizes(ddev, info, cmd, &st, use_ifm, false,
					 false, use_scale);
			if (ret)
				return ret;
			break;
		case NPU_OP_ELEMENTWISE:
			if (!ethosu_is_u65(edev) &&
			    !cmd_state_reg_is_set(&st, NPU_SET_IFM_BROADCAST))
				return -EINVAL;
			use_ifm2 = (param != 5) && (param != 6) &&
				(param != 7) && (param != 0x24);
			if (use_ifm2 &&
			    !cmd_state_reg_is_set(&st, NPU_SET_IFM2_BROADCAST))
				return -EINVAL;
			use_scale = use_ifm2 && (ethosu_is_u65(edev) ?
				    (st.ifm2.broadcast & 0x80) :
				    (st.ifm2.broadcast == 8));
			use_ifm2 = use_ifm2 && !use_scale;
			use_ifm = st.ifm.broadcast != 8;
			ret = calc_sizes_elemwise(ddev, info, cmd, &st, use_ifm, use_ifm2);
			if (ret)
				return ret;
			break;
		case NPU_OP_RESIZE: // U85 only
			return -EINVAL;
		case NPU_SET_KERNEL_WIDTH_M1:
			st.ifm.width = param;
			break;
		case NPU_SET_KERNEL_HEIGHT_M1:
			st.ifm.height[2] = param;
			break;
		case NPU_SET_KERNEL_STRIDE:
			st.ifm.stride_kernel = param;
			break;
		case NPU_SET_IFM_PAD_TOP:
			st.ifm.pad_top = param & 0x7f;
			break;
		case NPU_SET_IFM_PAD_LEFT:
			st.ifm.pad_left = param & 0x7f;
			break;
		case NPU_SET_IFM_PAD_RIGHT:
			st.ifm.pad_right = param & 0xff;
			break;
		case NPU_SET_IFM_PAD_BOTTOM:
			st.ifm.pad_bottom = param & 0xff;
			break;
		case NPU_SET_IFM_DEPTH_M1:
			st.ifm.depth = param;
			break;
		case NPU_SET_IFM_PRECISION:
			if (((param >> 6) & 0x3) > 1)
				return -EINVAL;
			st.ifm.precision = param;
			break;
		case NPU_SET_IFM_BROADCAST:
			st.ifm.broadcast = param;
			break;
		case NPU_SET_IFM_REGION:
			st.ifm.region = param & 0x7;
			break;
		case NPU_SET_IFM_WIDTH0_M1:
			st.ifm.width0 = param;
			break;
		case NPU_SET_IFM_HEIGHT0_M1:
			st.ifm.height[0] = param;
			break;
		case NPU_SET_IFM_HEIGHT1_M1:
			st.ifm.height[1] = param;
			break;
		case NPU_SET_IFM_BASE0:
		case NPU_SET_IFM_BASE1:
		case NPU_SET_IFM_BASE2:
		case NPU_SET_IFM_BASE3:
			st.ifm.base[cmd & 0x3] = addr;
			break;
		case NPU_SET_IFM_STRIDE_X:
			st.ifm.stride_x = addr;
			break;
		case NPU_SET_IFM_STRIDE_Y:
			st.ifm.stride_y = addr;
			break;
		case NPU_SET_IFM_STRIDE_C:
			st.ifm.stride_c = addr;
			break;

		case NPU_SET_OFM_WIDTH_M1:
			st.ofm.width = param;
			break;
		case NPU_SET_OFM_HEIGHT_M1:
			st.ofm.height[2] = param;
			break;
		case NPU_SET_OFM_DEPTH_M1:
			st.ofm.depth = param;
			break;
		case NPU_SET_OFM_PRECISION:
			if (((param >> 6) & 0x3) > 1)
				return -EINVAL;
			if (!ethosu_is_u65(edev) && (param & GENMASK(13, 11)))
				return -EINVAL;
			st.ofm.precision = param;
			break;
		case NPU_SET_OFM_REGION:
			st.ofm.region = param & 0x7;
			break;
		case NPU_SET_OFM_WIDTH0_M1:
			st.ofm.width0 = param;
			break;
		case NPU_SET_OFM_HEIGHT0_M1:
			st.ofm.height[0] = param;
			break;
		case NPU_SET_OFM_HEIGHT1_M1:
			st.ofm.height[1] = param;
			break;
		case NPU_SET_OFM_BASE0:
		case NPU_SET_OFM_BASE1:
		case NPU_SET_OFM_BASE2:
		case NPU_SET_OFM_BASE3:
			st.ofm.base[cmd & 0x3] = addr;
			break;
		case NPU_SET_OFM_STRIDE_X:
			st.ofm.stride_x = addr;
			break;
		case NPU_SET_OFM_STRIDE_Y:
			st.ofm.stride_y = addr;
			break;
		case NPU_SET_OFM_STRIDE_C:
			st.ofm.stride_c = addr;
			break;

		case NPU_SET_IFM2_BROADCAST:
			st.ifm2.broadcast = param;
			break;
		case NPU_SET_IFM2_PRECISION:
			if (((param >> 6) & 0x3) > 1)
				return -EINVAL;
			st.ifm2.precision = param;
			break;
		case NPU_SET_IFM2_REGION:
			st.ifm2.region = param & 0x7;
			break;
		case NPU_SET_IFM2_WIDTH0_M1:
			st.ifm2.width0 = param;
			break;
		case NPU_SET_IFM2_HEIGHT0_M1:
			st.ifm2.height[0] = param;
			break;
		case NPU_SET_IFM2_HEIGHT1_M1:
			st.ifm2.height[1] = param;
			break;
		case NPU_SET_IFM2_BASE0:
		case NPU_SET_IFM2_BASE1:
		case NPU_SET_IFM2_BASE2:
		case NPU_SET_IFM2_BASE3:
			st.ifm2.base[cmd & 0x3] = addr;
			break;
		case NPU_SET_IFM2_STRIDE_X:
			st.ifm2.stride_x = addr;
			break;
		case NPU_SET_IFM2_STRIDE_Y:
			st.ifm2.stride_y = addr;
			break;
		case NPU_SET_IFM2_STRIDE_C:
			st.ifm2.stride_c = addr;
			break;

		case NPU_SET_WEIGHT_REGION:
			st.weight[0].region = param & 0x7;
			break;
		case NPU_SET_SCALE_REGION:
			st.scale[0].region = param & 0x7;
			break;
		case NPU_SET_WEIGHT_BASE:
			st.weight[0].base = addr;
			break;
		case NPU_SET_WEIGHT_LENGTH:
			st.weight[0].length = cmds[1];
			break;
		case NPU_SET_SCALE_BASE:
			st.scale[0].base = addr;
			break;
		case NPU_SET_SCALE_LENGTH:
			st.scale[0].length = cmds[1];
			break;
		case NPU_SET_WEIGHT1_BASE:
			st.weight[1].base = addr;
			break;
		case NPU_SET_WEIGHT1_LENGTH:
			st.weight[1].length = cmds[1];
			break;
		case NPU_SET_SCALE1_BASE: // NPU_SET_WEIGHT2_BASE (U85)
			if (ethosu_is_u65(edev))
				st.scale[1].base = addr;
			else
				st.weight[2].base = addr;
			break;
		case NPU_SET_SCALE1_LENGTH: // NPU_SET_WEIGHT2_LENGTH (U85)
			if (ethosu_is_u65(edev))
				st.scale[1].length = cmds[1];
			else
				st.weight[2].length = cmds[1];
			break;
		case NPU_SET_WEIGHT3_BASE:
			st.weight[3].base = addr;
			break;
		case NPU_SET_WEIGHT3_LENGTH:
			st.weight[3].length = cmds[1];
			break;

		case NPU_SET_DMA0_SRC_REGION:
			if (param & NPU_DMA_REGION_INDEX_MODE)
				return -EINVAL;
			if (param & 0x100)
				st.dma.src.region = -1;
			else
				st.dma.src.region = param & 0x7;
			st.dma.src.mode = (param >> 9) & 0x3;
			if (st.dma.src.mode == 3)
				return -EINVAL;
			break;
		case NPU_SET_DMA0_DST_REGION:
			if (param & NPU_DMA_REGION_INDEX_MODE)
				return -EINVAL;
			if (param & 0x100)
				st.dma.dst.region = -1;
			else
				st.dma.dst.region = param & 0x7;
			st.dma.dst.mode = (param >> 9) & 0x3;
			if (st.dma.dst.mode == 3)
				return -EINVAL;
			break;
		case NPU_SET_DMA0_SIZE0:
			st.dma.size0 = param;
			break;
		case NPU_SET_DMA0_SIZE1:
			st.dma.size1 = param;
			break;
		case NPU_SET_DMA0_SRC_STRIDE0:
			st.dma.src.stride[0] = ((s64)addr << 24) >> 24;
			break;
		case NPU_SET_DMA0_SRC_STRIDE1:
			st.dma.src.stride[1] = ((s64)addr << 24) >> 24;
			break;
		case NPU_SET_DMA0_DST_STRIDE0:
			st.dma.dst.stride[0] = ((s64)addr << 24) >> 24;
			break;
		case NPU_SET_DMA0_DST_STRIDE1:
			st.dma.dst.stride[1] = ((s64)addr << 24) >> 24;
			break;
		case NPU_SET_DMA0_SRC:
			st.dma.src.offset = addr;
			break;
		case NPU_SET_DMA0_DST:
			st.dma.dst.offset = addr;
			break;
		case NPU_SET_DMA0_LEN:
			st.dma.src.len = st.dma.dst.len = addr;
			break;
		default:
			if (cmd & NPU_CMD_RESERVED_MASK)
				return -EINVAL;
			break;
		}
	}

	if (!ends_with_stop)
		return -EINVAL;

	for (i = 0; i < NPU_BASEP_REGION_MAX; i++) {
		if (!info->region_size[i])
			continue;
		dev_dbg(ddev->dev, "region %d max size: 0x%llx\n",
			i, info->region_size[i]);
	}

	bo->info = no_free_ptr(info);
	return 0;
}

/**
 * ethosu_gem_cmdstream_create() - Create a GEM object and attach it to a handle.
 * @file: DRM file.
 * @ddev: DRM device.
 * @exclusive_vm: Exclusive VM. Not NULL if the GEM object can't be shared.
 * @size: Size of the GEM object to allocate.
 * @flags: Combination of drm_ethosu_bo_flags flags.
 * @handle: Pointer holding the handle pointing to the new GEM object.
 *
 * Return: Zero on success
 */
int ethosu_gem_cmdstream_create(struct drm_file *file,
				struct drm_device *ddev,
				u32 size, u64 data, u32 flags, u32 *handle)
{
	int ret;
	struct drm_gem_dma_object *mem;
	struct ethosu_gem_object *bo;

	mem = drm_gem_dma_create(ddev, size);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	bo = to_ethosu_bo(&mem->base);
	bo->flags = flags;

	ret = ethosu_gem_cmdstream_copy_and_validate(ddev,
						     (void __user *)(uintptr_t)data,
						     bo, size);
	if (ret)
		goto fail;

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file, &mem->base, handle);

fail:
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&mem->base);

	return ret;
}
