/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Video4Linux2 generic ISP parameters and statistics support
 *
 * Copyright (C) 2025 Ideas On Board Oy
 * Author: Jacopo Mondi <jacopo.mondi@ideasonboard.com>
 */

#ifndef V4L2_PARAMS_H_
#define V4L2_PARAMS_H_

#include <linux/media/v4l2-isp.h>

struct device;
struct vb2_buffer;

/**
 * typedef v4l2_params_block_handler - V4L2 extensible format block handler
 * @arg: pointer the driver-specific argument
 * @block: the ISP configuration block to handle
 *
 * Defines the function signature of the functions that handle an ISP block
 * configuration.
 */
typedef void (*v4l2_params_block_handler)(void *arg,
					  const struct v4l2_params_block_header *block);

/**
 * struct v4l2_params_handler - V4L2 extensible format handler
 * @size: the block expected size
 * @handler: the block handler function
 *
 * The v4l2_params_handler defines the type that driver making use of the
 * V4L2 extensible parameters shall use to define their own ISP block
 * handlers.
 *
 * Drivers shall prepare a list of handlers, one for each supported ISP block
 * and correctly populate the structure's field with the expected block @size
 * (used for validation) and a pointer to each block @handler function.
 */
struct v4l2_params_handler {
	size_t size;
	v4l2_params_block_handler handler;
};

/**
 * v4l2_params_buffer_validate - Validate a V4L2 extensible parameters buffer
 * @dev: the driver's device pointer
 * @vb: the videobuf2 buffer
 * @max_size: the maximum allowed buffer size
 * @buffer_validate: callback to the driver-specific buffer validation
 *
 * Helper function that performs validation of an extensible parameters buffer.
 *
 * The helper is meant to be used by drivers to perform validation of the
 * extensible parameters buffer size correctness.
 *
 * The @vb buffer as received from the vb2 .buf_prepare() operation is checked
 * against @max_size and its validated to be large enough to accommodate at
 * least one ISP configuration block. The effective buffer size is compared
 * with the reported data size to make sure they match.
 *
 * Drivers should use this function to validate the buffer size correctness
 * before performing a copy of the user-provided videobuf2 buffer content into a
 * kernel-only memory buffer to prevent userspace from modifying the buffer
 * content after it has been submitted to the driver.
 */
int v4l2_params_buffer_validate(struct device *dev, struct vb2_buffer *vb,
				size_t max_size);

/**
 * v4l2_params_blocks_validate - Validate V4L2 extensible parameters ISP
 *				 configuration blocks
 * @dev: the driver's device pointer
 * @buffer: the extensible parameters configuration buffer
 * @handlers: the list of block handlers
 * @num_handlers: the number of block handlers
 *
 * Helper function that performs validation of the ISP configuration blocks in
 * an extensible parameters buffer.
 *
 * The helper is meant to be used by drivers to perform validation of the
 * ISP configuration data blocks. For each block in the extensible parameters
 * buffer, its size and correctness are validated against its associated handler
 * in the @handlers list.
 *
 * Drivers should use this function to validate the ISP configuration blocks
 * after having validated the correctness of the vb2 buffer sizes by using the
 * v4l2_params_buffer_validate() helper first. Once the buffer size has been
 * validated, drivers should perform a copy of the user-provided buffer into a
 * kernel-only memory buffer to prevent userspace from modifying the buffer
 * content after it has been submitted to the driver, and then call this
 * function to perform per-block validation.
 */
int v4l2_params_blocks_validate(struct device *dev,
				const struct v4l2_params_buffer *buffer,
				const struct v4l2_params_handler *handlers,
				size_t num_handlers);

#endif /* V4L2_PARAMS_H_ */
