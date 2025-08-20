/* SPDX-License-Identifier: ((GPL-2.0+ WITH Linux-syscall-note) OR MIT) */
/*
 * Video4Linux2 extensible configuration parameters base types
 *
 * Copyright (C) 2025 Ideas On Board Oy
 * Author: Jacopo Mondi <jacopo.mondi@ideasonboard.com>
 */

#ifndef _UAPI_V4L2_PARAMS_H_
#define _UAPI_V4L2_PARAMS_H_

#include <linux/stddef.h>
#include <linux/types.h>

#define V4L2_PARAMS_FL_BLOCK_DISABLE	(1U << 0)
#define V4L2_PARAMS_FL_BLOCK_ENABLE	(1U << 1)

/*
 * Reserve the first 8 bits for V4L2_PARAMS_FL_* flag.
 *
 * Platform-specific flags should be defined as:
 * #define PLATFORM_SPECIFIC_FLAG0     ((1U << V4L2_PARAMS_FL_PLATFORM_FLAGS(0))
 * #define PLATFORM_SPECIFIC_FLAG1     ((1U << V4L2_PARAMS_FL_PLATFORM_FLAGS(1))
 */
#define V4L2_PARAMS_FL_PLATFORM_FLAGS(n)       ((n) + 8)

/**
 * struct v4l2_params_block_header - V4L2 extensible parameters block header
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
 * The @flags field is a bitmask of per-block flags V4L2_PARAMS_FL_* and
 * platform-specific flags specified by the platform-specific header.
 *
 * Documentation of the platform-specific flags handling is specified by the
 * platform-specific block header type:
 *
 * - Rockchip RkISP1: :c:type:`rkisp1_ext_params_block_type`
 * - Amlogic C3: :c:type:`c3_isp_params_block_type`
 *
 * Userspace is responsible for correctly populating the parameters block header
 * fields (@type, @flags and @size) and the block-specific parameters.
 *
 * @type: The parameters block type (platform-specific)
 * @flags: A bitmask of block flags (platform-specific)
 * @size: Size (in bytes) of the parameters block, including this header
 */
struct v4l2_params_block_header {
	__u16 type;
	__u16 flags;
	__u32 size;
} __attribute__((aligned(8)));

/**
 * v4l2_params_buffer_size - Calculate size of v4l2_params_buffer for a platform
 *
 * Users of the v4l2 extensible parameters will have differing sized data arrays
 * depending on their specific parameter buffers. Drivers and userspace will
 * need to be able to calculate the appropriate size of the struct to
 * accommodate all ISP configuration blocks provided by the platform.
 * This macro provides a convenient tool for the calculation.
 *
 * Each driver shall provide a definition of their extensible parameters
 * implementation data buffer size. As an example:
 *
 * #define PLATFORM_BLOCKS_MAX_SIZE		\
 *	sizeof(platform_block_0)	+	\
 *	sizeof(platform_block_1)
 *
 * #define PLATFORM_BUFFER_SIZE			\
 *	v4l2_params_buffer_size(PLATFORM_BLOCKS_MAX_SIZE)
 *
 * Drivers are then responsible for allocating buffers of the proper size
 * by assigning PLATFORM_BUFFER_SIZE to the per-plane size of the videobuf2
 * .queue_setup() operation and userspace shall use PLATFORM_BUFFER_SIZE
 * when populating the ISP configuration data buffer.
 *
 * @max_params_size: The total size of the ISP configuration blocks
 */
#define v4l2_params_buffer_size(max_params_size) \
	(offsetof(struct v4l2_params_buffer, data) + (max_params_size))

/**
 * struct v4l2_params_buffer - V4L2 extensible parameters configuration
 *
 * This struct contains the configuration parameters of the ISP algorithms,
 * serialized by userspace into a data buffer. Each configuration parameter
 * block is represented by a block-specific structure which contains a
 * :c:type:`v4l2_params_block_header` entry as first member. Userspace populates
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
 * Drivers shall take care of properly sizing of the extensible parameters
 * buffer @data array. The v4l2_params_buffer type is defined with a
 * flexible-array-member at the end, which resolves to a size of 0 bytes when
 * inspected with sizeof(struct v4l2_params_buffer). This of course is not
 * suitable for neither buffer allocation in the kernel driver nor for proper
 * handling in userspace of the @data buffer it has to populate.
 *
 * Drivers using this type in their userspace API definition are responsible
 * for providing the exact definition of the @data buffer size using the
 * v4l2_params_buffer_size() macro. The size shall be used
 * by the driver for buffers allocation and by userspace for populating the
 * @data buffer before queueing it to the driver
 *
 * Drivers that were already using extensible-parameters before the introduction
 * of this file define their own type-convertible implementation of this
 * type, see:
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
	__u8 data[] __counted_by(data_size);
};

#endif /* _UAPI_V4L2_PARAMS_H_ */
