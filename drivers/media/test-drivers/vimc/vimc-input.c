// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * vimc-input.c Virtual Media Controller Driver
 *
 * Copyright (C) 2025 Virtual Input Entity Implementation
 */

#include <linux/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "vimc-common.h"

struct vimc_input_device {
	struct vimc_ent_device ved;
	struct v4l2_subdev sd;
	struct media_pad pad;
};

static const struct v4l2_mbus_framefmt fmt_default = {
	.width = 640,
	.height = 480,
	.code = MEDIA_BUS_FMT_RGB888_1X24,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_SRGB,
};

static int vimc_input_init_state(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *mf;
	unsigned int i;

	for (i = 0; i < sd->entity.num_pads; i++) {
		mf = v4l2_subdev_state_get_format(sd_state, i);
		*mf = fmt_default;
	}

	return 0;
}

static int vimc_input_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_RGB888_1X24;

	return 0;
}

static int vimc_input_enum_frame_size(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_frame_size_enum *fse)
{
	const struct vimc_pix_map *vpix;

	if (fse->index)
		return -EINVAL;

	/* Only accept code in the pix map table */
	vpix = vimc_pix_map_by_code(fse->code);
	if (!vpix)
		return -EINVAL;

	fse->min_width = VIMC_FRAME_MIN_WIDTH;
	fse->max_width = VIMC_FRAME_MAX_WIDTH;
	fse->min_height = VIMC_FRAME_MIN_HEIGHT;
	fse->max_height = VIMC_FRAME_MAX_HEIGHT;

	return 0;
}

static int vimc_input_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *mf;

	mf = v4l2_subdev_state_get_format(sd_state, fmt->pad);

	fmt->format = *mf;

	return 0;
}

static int vimc_input_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *mf;

	mf = v4l2_subdev_state_get_format(sd_state, fmt->pad);

	/* Set the new format */
	*mf = fmt->format;

	return 0;
}

static int vimc_input_enable_streams(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     u32 pad, u64 streams_mask)
{
	/* For input entity, we don't allocate frames since we expect
	 * external frame injection. Just mark that streaming is active.
	 *
	 * TODO: For future enhancement, consider implementing frame generation
	 * or userspace frame injection mechanism. This would require:
	 * - Frame buffer allocation (similar to vimc-sensor.c)
	 * - Interface for userspace to inject frames (e.g., via sysfs/debugfs)
	 * - Frame rate control for generated test patterns
	 * - Integration with VIMC's streaming infrastructure
	 * This would make the input entity suitable for more testing scenarios.
	 */
	return 0;
}

static int vimc_input_disable_streams(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      u32 pad, u64 streams_mask)
{
	/* Streaming stopped - no cleanup needed for input entity */
	return 0;
}

static const struct v4l2_subdev_pad_ops vimc_input_pad_ops = {
	.enum_mbus_code		= vimc_input_enum_mbus_code,
	.enum_frame_size	= vimc_input_enum_frame_size,
	.get_fmt		= vimc_input_get_fmt,
	.set_fmt		= vimc_input_set_fmt,
	.enable_streams		= vimc_input_enable_streams,
	.disable_streams	= vimc_input_disable_streams,
};

static const struct v4l2_subdev_ops vimc_input_ops = {
	.pad = &vimc_input_pad_ops,
};

static const struct v4l2_subdev_internal_ops vimc_input_internal_ops = {
	.init_state = vimc_input_init_state,
};

static void vimc_input_release(struct vimc_ent_device *ved)
{
	struct vimc_input_device *vinput =
		container_of(ved, struct vimc_input_device, ved);

	v4l2_subdev_cleanup(&vinput->sd);
	media_entity_cleanup(vinput->ved.ent);
	kfree(vinput);
}

/*
 * Input process frame function
 * For an input entity, just return the received frame unchanged
 */
static void *vimc_input_process_frame(struct vimc_ent_device *ved,
				      const void *frame)
{
	/* For an input entity, just return the received frame unchanged.
	 *
	 * TODO: Future enhancement could implement:
	 * - Frame validation and format checking
	 * - Frame transformation or processing
	 * - Frame injection from userspace buffers
	 * - Frame rate limiting or buffering
	 * Currently, this is a simple pass-through for external frame sources.
	 */
	return (void *)frame;
}

static struct vimc_ent_device *vimc_input_add(struct vimc_device *vimc,
					      const char *vcfg_name)
{
	struct v4l2_device *v4l2_dev = &vimc->v4l2_dev;
	struct vimc_input_device *vinput;
	int ret;

	/* Allocate the vinput struct */
	vinput = kzalloc(sizeof(*vinput), GFP_KERNEL);
	if (!vinput)
		return ERR_PTR(-ENOMEM);

	/* Initialize the media pad */
	vinput->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = vimc_ent_sd_register(&vinput->ved, &vinput->sd, v4l2_dev,
				   vcfg_name,
				   MEDIA_ENT_F_IO_V4L, 1, &vinput->pad,
				   &vimc_input_internal_ops, &vimc_input_ops);
	if (ret)
		goto err_free_vinput;

	vinput->ved.process_frame = vimc_input_process_frame;
	vinput->ved.dev = vimc->mdev.dev;

	return &vinput->ved;

err_free_vinput:
	kfree(vinput);

	return ERR_PTR(ret);
}

const struct vimc_ent_type vimc_input_type = {
	.add = vimc_input_add,
	.release = vimc_input_release
};
