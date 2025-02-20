/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * AIE Controls Header
 *
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Fish Wu <fish.wu@mediatek.com>
 */

#ifndef __MTK_AIE_V4L2_CONTROLS_H__
#define __MTK_AIE_V4L2_CONTROLS_H__

#include <linux/types.h>

/*
 * The base for the mediatek Face Detection driver controls.
 * We reserve 16 controls for this driver.
 * Each CID represents different stages of AIE, with different structures and functions
 * and cannot be reused
 */
#define V4L2_CID_USER_MTK_FD_BASE (V4L2_CID_USER_BASE + 0x1fd0)

#define V4L2_CID_MTK_AIE_INIT (V4L2_CID_USER_MTK_FD_BASE + 1)
#define V4L2_CID_MTK_AIE_PARAM (V4L2_CID_USER_MTK_FD_BASE + 2)

#define V4L2_FLD_MAX_FRAME 15

/**
 * struct v4l2_ctrl_aie_init - aie init parameters.
 *
 * @max_img_width: maximum width of the source image.
 * @max_img_height: maximum height of the source image.
 * @pyramid_width: maximum width of the base pyramid.
 * @pyramid_height: maximum height of the base pyramid.
 * @feature_threshold: feature threshold for hareware.
 */
struct v4l2_ctrl_aie_init {
	__u32 max_img_width;
	__u32 max_img_height;
	__u32 pyramid_width;
	__u32 pyramid_height;
	__s32 feature_threshold;
};

/**
 * struct v4l2_aie_roi - aie roi parameters.
 *
 * @x1: x1 of the roi coordinate.
 * @y1: y1 of the roi coordinate.
 * @x2: x2 of the roi coordinate.
 * @y2: y2 of the roi coordinate.
 */
struct v4l2_aie_roi {
	__u32 x1;
	__u32 y1;
	__u32 x2;
	__u32 y2;
};

/**
 * struct v4l2_aie_padding - aie padding parameters.
 *
 * @left: the size of padding left.
 * @right: the size of padding right.
 * @down: the size of padding below.
 * @up: the size of padding above.
 */
struct v4l2_aie_padding {
	__u32 left;
	__u32 right;
	__u32 down;
	__u32 up;
};

/**
 * struct v4l2_fld_crop_rip_rop - aie fld parameters.
 *
 * @fld_in_crop_x1: x1 of the crop coordinate.
 * @fld_in_crop_y1: y1 of the crop coordinate.
 * @fld_in_crop_x2: x2 of the crop coordinate.
 * @fld_in_crop_y2: y2 of the crop coordinate.
 * @fld_in_rip: fld in rip.
 * @fld_in_rop: fld in rop.
 */
struct v4l2_fld_crop_rip_rop {
	__u32 fld_in_crop_x1;
	__u32 fld_in_crop_y1;
	__u32 fld_in_crop_x2;
	__u32 fld_in_crop_y2;
	__u32 fld_in_rip;
	__u32 fld_in_rop;
};

/**
 * struct v4l2_fld_crop_rip_rop - aie fld parameters.
 *
 * @fd_mode: select a mode(FDMODE, ATTRIBUTEMODE, FLDMODE) for current fd.
 * @src_img_fmt: source image format.
 * @src_img_width: the width of the source image.
 * @src_img_height: the height of the source image.
 * @src_img_stride: the stride of the source image.
 * @pyramid_base_width: pyramid is the size of resizer, the width of the base pyramid.
 * @pyramid_base_height: pyramid is the size of resizer, the width of the base pyramid.
 * @number_of_pyramid: number of pyramid, min: 1, max: 3.
 * @rotate_degree: the rotate degree of the image.
 * @en_roi: enable roi(roi is a box diagram that selects a rectangle in a picture).
 *          when en_roi is enable, AIE will return a rectangle face detection result
 * @src_roi: roi params.
 * @en_padding: enable padding.
 * @src_padding: padding params.
 * @freq_level: frequency level, Get value from user space.
 * @fld_face_num: the number of faces in fld.
 *                user space tells driver the number of detections.
 * @fld_input: fld input params.
 */
struct v4l2_ctrl_aie_param {
	__u32 fd_mode;
	__u32 src_img_fmt;
	__u32 src_img_width;
	__u32 src_img_height;
	__u32 src_img_stride;
	__u32 pyramid_base_width;
	__u32 pyramid_base_height;
	__u32 number_of_pyramid;
	__u32 rotate_degree;
	__s32 en_roi;
	struct v4l2_aie_roi src_roi;
	__s32 en_padding;
	struct v4l2_aie_padding src_padding;
	__u32 freq_level;
	__u32 fld_face_num;
	struct v4l2_fld_crop_rip_rop fld_input[V4L2_FLD_MAX_FRAME];
};

#endif /* __MTK_AIE_V4L2_CONTROLS_H__ */
