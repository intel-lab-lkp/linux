/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * AIE Controls Header
 *
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Bo Kong <bo.kong@mediatek.com>
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

#define MAX_FACE_NUM			1024
#define FLD_CUR_LANDMARK		11
#define FLD_MAX_FRAME			15

/**
 * struct v4l2_ctrl_aie_init - aie init parameters.
 *
 * @max_img_width: maximum width of the source image.
 * @max_img_height: maximum height of the source image.
 * @pyramid_width: maximum width of the base pyramid.
 * @pyramid_height: maximum height of the base pyramid.
 * @feature_threshold: The threshold for the face confidence.Range: 100 ~ 400.
 *                     The larger the value,the lower the face recognition rate
 */
struct v4l2_ctrl_aie_init {
	__u32 max_img_width;
	__u32 max_img_height;
	__u32 pyramid_width;
	__u32 pyramid_height;
	__s32 feature_threshold;
};

/**
 * struct aie_roi_coordinate - aie roi parameters.
 *
 * @x1: x1 of the roi coordinate.
 * @y1: y1 of the roi coordinate.
 * @x2: x2 of the roi coordinate.
 * @y2: y2 of the roi coordinate.
 */
struct aie_roi_coordinate {
	__u32 x1;
	__u32 y1;
	__u32 x2;
	__u32 y2;
};

/**
 * struct aie_padding_size - aie padding parameters.
 *
 * @left: the size of padding left.
 * @right: the size of padding right.
 * @down: the size of padding below.
 * @up: the size of padding above.
 */
struct aie_padding_size {
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
 * struct fd_ret - aie fd result parameters.
 *
 * @anchor_x0: X coordinate of the top-left corner of each detected face.
 * @anchor_x1: X coordinate of the bottom-right corner of each detected face.
 * @anchor_y0: Y coordinate of the top-left corner of each detected face.
 * @anchor_y1: Y coordinate of the bottom-right corner of each detected face.
 * @rop_landmark_score0: Score for the first rotation pose landmark.
 * @rop_landmark_score1: Score for the second rotation pose landmark.
 * @rop_landmark_score2: Score for the third rotation pose landmark.
 * @anchor_score: Score for the anchor points.
 * @rip_landmark_score0: Score for the first rotation-invariant pose landmark.
 * @rip_landmark_score1: Score for the second rotation-invariant pose landmark.
 * @rip_landmark_score2: Score for the third rotation-invariant pose landmark.
 * @rip_landmark_score3: Score for the fourth rotation-invariant pose landmark.
 * @rip_landmark_score4: Score for the fifth rotation-invariant pose landmark.
 * @rip_landmark_score5: Score for the sixth rotation-invariant pose landmark.
 * @rip_landmark_score6: Score for the seventh rotation-invariant pose landmark.
 * @face_result_index: Index of each detected face.
 * @anchor_index: Index of the anchor points.
 * @fd_partial_result: Partial face detection result.
 */
struct fd_ret {
	__u16 anchor_x0[MAX_FACE_NUM];
	__u16 anchor_x1[MAX_FACE_NUM];
	__u16 anchor_y0[MAX_FACE_NUM];
	__u16 anchor_y1[MAX_FACE_NUM];
	__s16 rop_landmark_score0[MAX_FACE_NUM];
	__s16 rop_landmark_score1[MAX_FACE_NUM];
	__s16 rop_landmark_score2[MAX_FACE_NUM];
	__s16 anchor_score[MAX_FACE_NUM];
	__s16 rip_landmark_score0[MAX_FACE_NUM];
	__s16 rip_landmark_score1[MAX_FACE_NUM];
	__s16 rip_landmark_score2[MAX_FACE_NUM];
	__s16 rip_landmark_score3[MAX_FACE_NUM];
	__s16 rip_landmark_score4[MAX_FACE_NUM];
	__s16 rip_landmark_score5[MAX_FACE_NUM];
	__s16 rip_landmark_score6[MAX_FACE_NUM];
	__u16 face_result_index[MAX_FACE_NUM];
	__u16 anchor_index[MAX_FACE_NUM];
	__u32 fd_partial_result;
};

/**
 * struct fd_result - Face detection results for different pyramid levels.
 *
 * @fd_pyramid0_num: Number of faces detected at pyramid level 0.
 * @fd_pyramid1_num: Number of faces detected at pyramid level 1.
 * @fd_pyramid2_num: Number of faces detected at pyramid level 2.
 * @fd_total_num: Total number of faces detected across all pyramid levels.
 * @pyramid0_result: Detection results for pyramid level 0.
 * @pyramid1_result: Detection results for pyramid level 1.
 * @pyramid2_result: Detection results for pyramid level 2.
 */
struct fd_result {
	__u16 fd_pyramid0_num;
	__u16 fd_pyramid1_num;
	__u16 fd_pyramid2_num;
	__u16 fd_total_num;
	struct fd_ret pyramid0_result;
	struct fd_ret pyramid1_result;
	struct fd_ret pyramid2_result;
};

/**
 * struct attr_result - Attribute detection results parameters.
 *
 * @gender_ret: Gender detection results.
 * @ethnicity_ret: Ethnicity detection results.
 * @merged_age_ret: Merged age detection results.
 * @merged_gender_ret: Merged gender detection results.
 * @merged_is_indian_ret: Merged results indicating if the subject is Indian.
 * @merged_ethnicity_ret: Merged ethnicity detection results.
 */
struct attr_result {
	__s16 gender_ret[2][64];
	__s16 ethnicity_ret[4][64];
	__s16 merged_age_ret[2];
	__s16 merged_gender_ret[2];
	__s16 merged_is_indian_ret[2];
	__s16 merged_ethnicity_ret[4];
};

/**
 * struct fld_landmark - FLD coordinates parameters.
 *
 * @x: X coordinate of the facial landmark.
 * @y: Y coordinate of the facial landmark.
 */
struct fld_landmark {
	__u16 x;
	__u16 y;
};

/**
 * struct fld_result - FLD detection results parameters.
 *
 * @fld_landmark: Array of facial landmarks, each with X and Y coordinates.
 * @fld_out_rip: Output rotation-invariant pose value.
 * @fld_out_rop: Output rotation pose value.
 * @confidence: Confidence score of the facial landmark detection.
 * @blinkscore: Blink score indicating the likelihood of eye blink.
 */
struct fld_result {
	struct fld_landmark fld_landmark[FLD_CUR_LANDMARK];
	__u16 fld_out_rip;
	__u16 fld_out_rop;
	__u16 confidence;
	__s16 blinkscore;
};

/**
 * struct aie_enq_info - V4L2 Kernelspace parameters.
 *
 * @sel_mode: select a mode(FDMODE, ATTRIBUTEMODE, FLDMODE) for current fd.
 *           FDMODE: Face Detection.
 *           ATTRIBUTEMODE: Gender and ethnicity detection
 *           FLDMODE: Locations of eyebrows, eyes, ears, nose,and mouth
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
 * @en_padding: enable padding, this is only used on the hardware of yuv to rgb.
 *              and has noting to do with fd_mode
 * @src_padding: padding params.
 * @freq_level: frequency level, Get value from user space enque.
 * @fld_face_num: the number of faces in fld.
 *                user space tells driver the number of detections.
 * @fld_input: fld input params.
 * @src_img_addr: Source image address.
 * @src_img_addr_uv: Source image address for UV plane.
 * @fd_out: Face detection results.
 * @attr_out: Attribute detection results.
 * @fld_out: Array of facial landmark detection results for multiple frames.
 * @irq_status: Interrupt request status.
 */
struct aie_enq_info {
	__u32 sel_mode;
	__u32 src_img_fmt;
	__u32 src_img_width;
	__u32 src_img_height;
	__u32 src_img_stride;
	__u32 pyramid_base_width;
	__u32 pyramid_base_height;
	__u32 number_of_pyramid;
	__u32 rotate_degree;
	int en_roi;
	struct aie_roi_coordinate src_roi;
	int en_padding;
	struct aie_padding_size src_padding;
	unsigned int freq_level;
	unsigned int fld_face_num;
	struct v4l2_fld_crop_rip_rop fld_input[FLD_MAX_FRAME];
	__u32 src_img_addr;
	__u32 src_img_addr_uv;
	struct fd_result fd_out;
	struct attr_result attr_out;
	struct fld_result fld_out[FLD_MAX_FRAME];
	__u32 irq_status;
};

/**
 * struct v4l2_ctrl_aie_param - V4L2 Userspace parameters.
 *
 * @fd_mode: select a mode(FDMODE, ATTRIBUTEMODE, FLDMODE) for current fd.
 *           FDMODE: Face Detection.
 *           ATTRIBUTEMODE: Gender and ethnicity detection
 *           FLDMODE: Locations of eyebrows, eyes, ears, nose,and mouth
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
 * @en_padding: enable padding, this is only used on the hardware of yuv to rgb.
 *              and has noting to do with fd_mode
 * @src_padding: padding params.
 * @freq_level: frequency level, Get value from user space enque.
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
	struct aie_roi_coordinate src_roi;
	__s32 en_padding;
	struct aie_padding_size src_padding;
	__u32 freq_level;
	__u32 fld_face_num;
	struct v4l2_fld_crop_rip_rop fld_input[FLD_MAX_FRAME];
};

#endif /* __MTK_AIE_V4L2_CONTROLS_H__ */
