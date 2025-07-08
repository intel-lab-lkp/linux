/* SPDX-License-Identifier: ((GPL-2.0+ WITH Linux-syscall-note) OR MIT) */
/*
 * Video4Linux2 extensible configuration parameters base types
 *
 * Copyright (C) 2025 Ideas On Board Oy
 * Author: Jacopo Mondi <jacopo.mondi@ideasonboard.com>
 */

#ifndef _UAPI_V4L2_PARAMS_H_
#define _UAPI_V4L2_PARAMS_H_

#ifndef _UAPI_V4L2_EXTENSIBLE_PARAMS_GUARD_
/*
 * Note: each ISP driver exposes a different uAPI, where the types layout
 * match (more or less strictly) the hardware registers layout.
 *
 * This file defines the base types on which each ISP driver can implement its
 * own types that define its uAPI.
 *
 * This file is not meant to be included directly by applications which shall
 * instead only include the ISP-specific implementation.
 */
#error "This file should not be included directly by applications"
#endif

#include <linux/types.h>

/**
 * struct v4l2_params_block - V4L2 extensible parameters block header
 *
 * This structure represents the common part of all the ISP configuration
 * blocks. Each parameters block shall embed an instance of this structure type
 * as its first member, followed by the block-specific configuration data. The
 * driver inspects this common header to discern the block type and its size and
 * properly handle the block content by casting it to the correct block-specific
 * type.
 *
 * The @type field is one of the values enumerated by each platform-specific ISP
 * block types which specifies how the data should be interpreted by the driver.
 * The @size field specifies the size of the parameters block and is used by the
 * driver for validation purposes.
 *
 * The @flags field is a bitmask of platform-specific control flags.
 *
 * Userspace shall never use this type directly but use the platform specific
 * one with the associated data types.
 *
 * - Rockchip RkISP1: :c:type:`rkisp1_ext_params_block_type`
 * - Amlogic C3: :c:type:`c3_isp_params_block_type`
 *
 * @type: The parameters block type (platform-specific)
 * @flags: A bitmask of block flags (platform-specific)
 * @size: Size (in bytes) of the parameters block, including this header
 */
struct v4l2_params_block {
	__u16 type;
	__u16 flags;
	__u32 size;
} __attribute__((aligned(8)));

/**
 * struct v4l2_params_buffer - V4L2 extensible parameters configuration
 *
 * This struct contains the configuration parameters of the ISP algorithms,
 * serialized by userspace into a data buffer. Each configuration parameter
 * block is represented by a block-specific structure which contains a
 * :c:type:`v4l2_params_block` entry as first member. Userspace populates
 * the @data buffer with configuration parameters for the blocks that it intends
 * to configure. As a consequence, the data buffer effective size changes
 * according to the number of ISP blocks that userspace intends to configure and
 * is set by userspace in the @data_size field.
 *
 * The parameters buffer is versioned by the @version field to allow modifying
 * and extending its definition. Userspace shall populate the @version field to
 * inform the driver about the version it intends to use. The driver will parse
 * and handle the @data buffer according to the data layout specific to the
 * indicated version and return an error if the desired version is not
 * supported.
 *
 * For each ISP block that userspace wants to configure, a block-specific
 * structure is appended to the @data buffer, one after the other without gaps
 * in between nor overlaps. Userspace shall populate the @data_size field with
 * the effective size, in bytes, of the @data buffer.
 *
 * Each ISP driver using the extensible parameters format shall define a
 * type which is type-convertible to this one, with the difference that the
 * @data member shall actually a memory buffer of platform-specific size and
 * not a pointer.
 *
 * Userspace shall never use this type directly but use the platform specific
 * one with the associated data types.
 *
 * - Rockchip RkISP1: :c:type:`rkisp1_ext_params_cfg`
 * - Amlogic C3: :c:type:`c3_isp_params_cfg`
 *
 * @version: The parameters buffer version (platform-specific)
 * @data_size: The configuration data effective size, excluding this header
 * @data: The configuration data
 */
struct v4l2_params_buffer {
	__u32 version;
	__u32 data_size;
	__u8 data[];
};

#endif /* _UAPI_V4L2_PARAMS_H_ */
