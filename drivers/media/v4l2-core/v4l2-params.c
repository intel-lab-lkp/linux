// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Video4Linux2 extensible parameters helpers
 *
 * Copyright (C) 2025 Ideas On Board Oy
 * Author: Jacopo Mondi <jacopo.mondi@ideasonboard.com>
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <media/videobuf2-core.h>

#include <media/v4l2-params.h>

int v4l2_params_buffer_validate(struct device *dev, struct vb2_buffer *vb,
				size_t max_size,
				v4l2_params_validate_buffer buffer_validate)
{
	size_t header_size = offsetof(struct v4l2_params_buffer, data);
	struct v4l2_params_buffer *buffer = vb2_plane_vaddr(vb, 0);
	size_t payload_size = vb2_get_plane_payload(vb, 0);
	size_t buffer_size;
	int ret;

	/* Payload size can't be greater than the destination buffer size */
	if (payload_size > max_size) {
		dev_dbg(dev, "Payload size is too large: %zu\n", payload_size);
		return -EINVAL;
	}

	/* Payload size can't be smaller than the header size */
	if (payload_size < header_size) {
		dev_dbg(dev, "Payload size is too small: %zu\n", payload_size);
		return -EINVAL;
	}

	/* Validate the size reported in the parameter buffer header */
	buffer_size = header_size + buffer->data_size;
	if (buffer_size != payload_size) {
		dev_dbg(dev, "Data size %zu and payload size %zu are different\n",
			buffer_size, payload_size);
		return -EINVAL;
	}

	/* Driver-specific buffer validation. */
	if (buffer_validate) {
		ret = buffer_validate(dev, buffer);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_params_buffer_validate);

int v4l2_params_blocks_validate(struct device *dev,
				const struct v4l2_params_buffer *buffer,
				const struct v4l2_params_handler *handlers,
				size_t num_handlers,
				v4l2_params_validate_block block_validate)
{
	size_t block_offset = 0;
	size_t buffer_size;
	int ret;

	/* Walk the list of parameter blocks and validate them. */
	buffer_size = buffer->data_size;
	while (buffer_size >= sizeof(struct v4l2_params_block_header)) {
		const struct v4l2_params_handler *handler;
		const struct v4l2_params_block_header *block;

		/* Validate block sizes and types against the handlers. */
		block = (const struct v4l2_params_block_header *)
			(buffer->data + block_offset);

		if (block->type >= num_handlers) {
			dev_dbg(dev, "Invalid parameters block type\n");
			return -EINVAL;
		}

		if (block->size > buffer_size) {
			dev_dbg(dev, "Premature end of parameters data\n");
			return -EINVAL;
		}

		/* It's invalid to specify both ENABLE and DISABLE. */
		if ((block->flags & (V4L2_PARAMS_FL_BLOCK_ENABLE |
				     V4L2_PARAMS_FL_BLOCK_DISABLE)) ==
		     (V4L2_PARAMS_FL_BLOCK_ENABLE |
		     V4L2_PARAMS_FL_BLOCK_DISABLE)) {
			dev_dbg(dev, "Invalid parameters block flags\n");
			return -EINVAL;
		}

		/*
		 * Match the block reported size against the handler's expected
		 * one, but allow the block to only contain the header in
		 * case it is going to be disabled.
		 */
		handler = &handlers[block->type];
		if (block->size != handler->size &&
		    (!(block->flags & V4L2_PARAMS_FL_BLOCK_DISABLE) ||
		    block->size != sizeof(*block))) {
			dev_dbg(dev, "Invalid parameters block size\n");
			return -EINVAL;
		}

		/* Driver-specific per-block validation. */
		if (block_validate) {
			ret = block_validate(dev, block);
			if (ret)
				return ret;
		}

		block_offset += block->size;
		buffer_size -= block->size;
	}

	if (buffer_size) {
		dev_dbg(dev, "Unexpected data after the parameters buffer end\n");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_params_blocks_validate);
