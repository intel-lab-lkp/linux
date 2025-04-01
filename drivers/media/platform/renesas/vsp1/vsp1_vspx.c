// SPDX-License-Identifier: GPL-2.0+
/*
 * vsp1_vspx.c  --  R-Car Gen 4 VSPX
 *
 * Copyright (C) 2025 Ideas On Board Oy
 * Copyright (C) 2025 Renesas Electronics Corporation
 */

#include "vsp1_vspx.h"

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include <media/media-entity.h>
#include <media/v4l2-subdev.h>
#include <media/vsp1.h>

#include "vsp1.h"
#include "vsp1_dl.h"
#include "vsp1_iif.h"
#include "vsp1_pipe.h"
#include "vsp1_rwpf.h"

static const struct v4l2_pix_format_mplane vspx_default_fmt = {
	.width = 1920,
	.height = 1080,
	.pixelformat = V4L2_PIX_FMT_SRGGB8,
	.field = V4L2_FIELD_NONE,
	.num_planes = 1,
	.plane_fmt = {
		[0] = {
			.sizeimage = 1920 * 1080,
			.bytesperline = 1920,
		},
	},
};

/*
 * Apply the given width, height and fourcc to the subdevice inside the
 * VSP1 entity.
 */
static int vsp1_vspx_rwpf_set_subdev_fmt(struct vsp1_device *vsp1,
					 struct vsp1_rwpf *rwpf,
					 u32 isp_fourcc,
					 unsigned int width,
					 unsigned int height)
{
	struct vsp1_entity *ent = &rwpf->entity;
	const struct vsp1_format_info *fmtinfo;
	struct v4l2_subdev_format format = {};
	u32 vspx_fourcc;
	int ret;

	switch (isp_fourcc) {
	case V4L2_PIX_FMT_GREY:
		/* 8 bit RAW Bayer */
		vspx_fourcc = V4L2_PIX_FMT_RGB332;
		break;
	case V4L2_PIX_FMT_Y10:
	case V4L2_PIX_FMT_Y12:
	case V4L2_PIX_FMT_Y16:
		/* 10, 12 and 16 bit RAW Bayer */
		vspx_fourcc = V4L2_PIX_FMT_RGB565;
		break;
	case V4L2_PIX_FMT_XBGR32:
		/* ConfigDMA */
		vspx_fourcc = V4L2_PIX_FMT_XBGR32;
		break;
	default:
		return -EINVAL;
	}

	fmtinfo = vsp1_get_format_info(vsp1, vspx_fourcc);
	if (!fmtinfo) {
		dev_dbg(vsp1->dev, "Unsupported pixel format %08x\n",
			vspx_fourcc);
		return -EINVAL;
	}

	rwpf->fmtinfo = fmtinfo;

	format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	format.pad = RWPF_PAD_SINK;
	format.format.width = width;
	format.format.height = height;
	format.format.field = V4L2_FIELD_NONE;
	format.format.code = fmtinfo->mbus;

	ret = v4l2_subdev_call(&ent->subdev, pad, set_fmt, NULL, &format);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * Configure RPF0 for ConfigDMA or RAW image transfer.
 */
static int vsp1_vspx_rpf0_configure(struct vsp1_device *vsp1,
				    dma_addr_t addr, u32 isp_fourcc,
				    unsigned int width, unsigned int height,
				    unsigned int stride,
				    unsigned int iif_sink_pad,
				    struct vsp1_dl_list *dl,
				    struct vsp1_dl_body *dlb)
{
	struct vsp1_vspx_pipeline *vspx_pipe = &vsp1->vspx->pipe;
	struct vsp1_pipeline *pipe = &vspx_pipe->pipe;
	struct vsp1_rwpf *rpf0 = pipe->inputs[0];
	int ret;

	ret = vsp1_vspx_rwpf_set_subdev_fmt(vsp1, rpf0, isp_fourcc, width,
					    height);
	if (ret)
		return ret;

	rpf0->format.plane_fmt[0].bytesperline = stride;

	/*
	 * Connect RPF0 to the IIF sink pad corresponding to the config or image
	 * path.
	 */
	rpf0->entity.sink_pad = iif_sink_pad;

	pipe->part_table[0].rpf[0].width = width;
	pipe->part_table[0].rpf[0].height = height;

	rpf0->mem.addr[0] = addr;
	rpf0->mem.addr[1] = 0;
	rpf0->mem.addr[2] = 0;

	vsp1_entity_route_setup(&rpf0->entity, pipe, dlb);
	vsp1_entity_configure_stream(&rpf0->entity, rpf0->entity.state, pipe,
				     dl, dlb);
	vsp1_entity_configure_partition(&rpf0->entity, pipe,
					&pipe->part_table[0], dl, dlb);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Interrupt handling
 */
static void vsp1_vspx_pipeline_frame_end(struct vsp1_pipeline *pipe,
					 unsigned int completion)
{
	struct vsp1_vspx_pipeline *vspx_pipe = to_vsp1_vspx_pipeline(pipe);
	struct vsp1_vspx *vspx = to_vsp1_vspx(vspx_pipe);

	if (vspx_pipe->vspx_frame_end)
		vspx_pipe->vspx_frame_end(vspx_pipe->frame_end_data);

	/*
	 * Set the pipeline state to stopped to ensure the next call to
	 * vsp1_pipeline_run() actually starts the VSPX.
	 */
	scoped_guard(spinlock_irqsave, &pipe->irqlock) {
		pipe->state = VSP1_PIPELINE_STOPPED;
	}

	scoped_guard(spinlock_irqsave, &vspx_pipe->vspx_lock) {
		vspx_pipe->processing = false;
	}

	/* Try schedule a new job from the queue. */
	vsp1_isp_job_run(vspx->vsp1->dev);
}

/* -----------------------------------------------------------------------------
 * ISP Driver API (include/media/vsp1.h)
 */

/**
 * vsp1_isp_init() - Initialize the VSPX
 *
 * @dev: The VSP1 struct device
 *
 * Return: %0 on success or a negative error code on failure
 */
int vsp1_isp_init(struct device *dev)
{
	struct vsp1_device *vsp1 = dev_get_drvdata(dev);

	if (!vsp1)
		return -EPROBE_DEFER;

	return 0;
}
EXPORT_SYMBOL_GPL(vsp1_isp_init);

/**
 * vsp1_isp_get_bus_master - Get VSPX bus master
 *
 * The VSPX access memory through an FCPX instance. When allocating memory
 * buffers that will have to be accessed by the VSPX the 'struct device' of
 * the FCPX should be used. Use this function to get a reference to it.
 *
 * @dev: The VSP1 struct device
 *
 * Return: a pointer to the bus master's device
 */
struct device *vsp1_isp_get_bus_master(struct device *dev)
{
	struct vsp1_device *vsp1 = dev_get_drvdata(dev);

	if (!vsp1)
		return ERR_PTR(-ENODEV);

	return vsp1->bus_master;
}
EXPORT_SYMBOL_GPL(vsp1_isp_get_bus_master);

/**
 * vsp1_isp_alloc_buffers - Allocate buffers in the VSPX address space
 *
 * Allocate buffers that will be later accessed by the VSPX.
 *
 * @dev: The VSP1 struct device
 * @size: The size of the buffer to be allocated by the VSPX
 * @buffer_desc: The allocated buffer description, will be filled with the
 *		 buffer CPU-mapped address and the bus address
 *
 * Return: %0 on success or a negative error code on failure
 */
int vsp1_isp_alloc_buffers(struct device *dev, size_t size,
			   struct vsp1_isp_buffer_desc *buffer_desc)
{
	struct device *bus_master = vsp1_isp_get_bus_master(dev);

	if (IS_ERR_OR_NULL(bus_master))
		return -ENODEV;

	buffer_desc->cpu_addr = dma_alloc_coherent(bus_master, size,
						   &buffer_desc->dma_addr,
						   GFP_KERNEL);
	if (IS_ERR_OR_NULL(buffer_desc->cpu_addr))
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(vsp1_isp_alloc_buffers);

/**
 * vsp1_isp_configure - Configure the VSPX with the RAW image format
 *
 * Apply to the VSPX RPF/WPF the size of the RAW image that will be transferred
 * to the ISP.
 *
 * @dev: The VSP1 struct device
 * @fmt: The RAW image format description
 *
 * Return: %0 on success or a negative error code on failure
 */
int vsp1_isp_configure(struct device *dev,
		       const struct v4l2_pix_format_mplane *fmt)
{
	struct vsp1_device *vsp1 = dev_get_drvdata(dev);
	struct vsp1_vspx_pipeline *vspx_pipe;
	struct vsp1_pipeline *pipe;
	int ret;

	vspx_pipe = &vsp1->vspx->pipe;
	pipe = &vspx_pipe->pipe;

	/*
	 * Apply the same format to the RPF0 and WPF0 so that the partition
	 * calculation results in a single partition.
	 */
	ret = vsp1_vspx_rwpf_set_subdev_fmt(vsp1, pipe->inputs[0],
					    fmt->pixelformat, fmt->width,
					    fmt->height);
	if (ret)
		return ret;

	ret = vsp1_vspx_rwpf_set_subdev_fmt(vsp1, pipe->output, fmt->pixelformat,
					    fmt->width, fmt->height);
	if (ret)
		return ret;

	vsp1_pipeline_calculate_partition(pipe, &pipe->part_table[0],
					  fmt->width, 0);

	return 0;
}
EXPORT_SYMBOL_GPL(vsp1_isp_configure);

static void vsp1_vspx_release_jobs(struct vsp1_device *vsp1)
{
	struct vsp1_vspx_pipeline *vspx_pipe = &vsp1->vspx->pipe;
	struct vsp1_vspx_job *job, *tmp;

	guard(spinlock_irqsave)(&vspx_pipe->jobs_lock);

	list_for_each_entry_safe(job, tmp, &vspx_pipe->jobs, job_queue) {
		list_del(&job->job_queue);
		vsp1_dl_list_put(job->dl);
		kfree(job);
	}
}

/**
 * vsp1_isp_start_streaming - Start processing VSPX jobs
 *
 * Start the VSPX and prepare for accepting buffer transfer job requests.
 *
 * @dev: The VSP1 struct device
 * @frame_end: The frame end callback description
 * @enable: The enable/disable streaming flag
 *
 * Return: %0 on success or a negative error code on failure
 */
int vsp1_isp_start_streaming(struct device *dev,
			     struct vsp1_vspx_frame_end *frame_end,
			     bool enable)
{
	struct vsp1_device *vsp1 = dev_get_drvdata(dev);
	struct vsp1_vspx_pipeline *vspx_pipe = &vsp1->vspx->pipe;
	struct vsp1_pipeline *pipe = &vspx_pipe->pipe;
	u32 value;
	int ret;

	scoped_guard(spinlock_irqsave, &vspx_pipe->vspx_lock) {
		if (vspx_pipe->enabled == enable)
			return 0;

		vspx_pipe->enabled = enable;
	}

	if (!enable) {
		pipe->state = VSP1_PIPELINE_STOPPED;
		vsp1_pipeline_stop(pipe);
		vsp1_vspx_release_jobs(vsp1);
		vspx_pipe->processing = false;
		vspx_pipe->vspx_frame_end = NULL;
		vsp1_dlm_reset(pipe->output->dlm);
		vsp1_device_put(vsp1);
		return 0;
	}

	if (!frame_end) {
		ret = -EINVAL;
		goto error_stop_pipe;
	}

	vspx_pipe->vspx_frame_end = frame_end->vspx_frame_end;
	vspx_pipe->frame_end_data = frame_end->frame_end_data;

	/* Make sure VSPX is not active. */
	value = vsp1_read(vsp1, VI6_CMD(0));
	if (value & VI6_CMD_STRCMD) {
		dev_err(vsp1->dev,
			"%s: Starting of WPF0 already reserved\n", __func__);
		ret = -EBUSY;
		goto error_stop_pipe;
	}

	value = vsp1_read(vsp1, VI6_STATUS);
	if (value & VI6_STATUS_SYS_ACT(0)) {
		dev_err(vsp1->dev,
			"%s: WPF0 has not entered idle state\n", __func__);
		ret = -EBUSY;
		goto error_stop_pipe;
	}

	/* Enable the VSP1 and prepare for streaming. */
	vsp1_pipeline_dump(pipe, "VSPX job");

	ret = vsp1_device_get(vsp1);
	if (ret < 0)
		goto error_stop_pipe;

	return 0;

error_stop_pipe:
	scoped_guard(spinlock_irqsave, &vspx_pipe->vspx_lock) {
		vspx_pipe->enabled = false;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(vsp1_isp_start_streaming);

/**
 * vsp1_vspx_job_prepare - Prepare a display list with the content of the buffer
 *
 * @dev: The VSP1 struct device
 * @job: The job description
 *
 * Return: %0 on success or a negative error code on failure
 */
int vsp1_isp_job_prepare(struct device *dev,
			 const struct vsp1_isp_job_desc *desc)
{
	struct vsp1_device *vsp1 = dev_get_drvdata(dev);
	struct vsp1_vspx_pipeline *vspx_pipe = &vsp1->vspx->pipe;
	struct vsp1_pipeline *pipe = &vspx_pipe->pipe;
	const struct v4l2_pix_format_mplane *pix_mp;
	struct vsp1_dl_list *second_dl = NULL;
	struct vsp1_vspx_job *job;
	struct vsp1_dl_body *dlb;
	struct vsp1_dl_list *dl;
	int ret;

	/*
	 * Populate a display list and append it to the jobs queue.
	 * Memory is released when the job is consumed.
	 */
	job = kmalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;

	/*
	 * Transfer the buffers described in the job: (optional) ConfigDMA and
	 * RAW image.
	 */

	job->dl = vsp1_dl_list_get(pipe->output->dlm);
	if (!job->dl) {
		ret = -ENOMEM;
		goto error_free_job;
	}

	dl = job->dl;
	dlb = vsp1_dl_list_get_body0(dl);

	/* Disable RPF1. */
	vsp1_dl_body_write(dlb, vsp1->rpf[1]->entity.route->reg,
			   VI6_DPR_NODE_UNUSED);

	/* Configure IIF routing and enable IIF function */
	vsp1_entity_route_setup(pipe->iif, pipe, dlb);
	vsp1_entity_configure_stream(pipe->iif, pipe->iif->state, pipe,
				     dl, dlb);

	/* Configure WPF0 to enable RPF0 as source*/
	vsp1_entity_route_setup(&pipe->output->entity, pipe, dlb);
	vsp1_entity_configure_stream(&pipe->output->entity,
				     pipe->output->entity.state, pipe,
				     dl, dlb);

	if (desc->config.pairs) {
		/*
		 * Configure RPF0 for config data. Transfer the number of
		 * configuration pairs plus 2 words for the header.
		 */
		ret = vsp1_vspx_rpf0_configure(vsp1, desc->config.mem,
					       V4L2_PIX_FMT_XBGR32,
					       desc->config.pairs * 2 + 2, 1,
					       desc->config.pairs * 2 + 2,
					       VSPX_IIF_SINK_PAD_CONFIG,
					       dl, dlb);
		if (ret)
			goto error_put_dl;

		second_dl = vsp1_dl_list_get(pipe->output->dlm);
		if (!second_dl) {
			ret = -ENOMEM;
			goto error_put_dl;
		}

		dl = second_dl;
		dlb = vsp1_dl_list_get_body0(dl);
	}

	/* Configure RPF0 for RAW image transfer. */
	pix_mp = &desc->img.fmt.fmt.pix_mp;
	ret = vsp1_vspx_rpf0_configure(vsp1, desc->img.mem,
				       pix_mp->pixelformat,
				       pix_mp->width, pix_mp->height,
				       pix_mp->plane_fmt[0].bytesperline,
				       VSPX_IIF_SINK_PAD_IMG, dl, dlb);
	if (ret)
		goto error_put_dl;

	if (second_dl)
		vsp1_dl_list_add_chain(job->dl, second_dl);

	scoped_guard(spinlock_irqsave, &vspx_pipe->jobs_lock) {
		list_add_tail(&job->job_queue, &vspx_pipe->jobs);
	}

	return 0;

error_put_dl:
	if (second_dl)
		vsp1_dl_list_put(second_dl);
	vsp1_dl_list_put(job->dl);
error_free_job:
	kfree(job);
	return ret;
}
EXPORT_SYMBOL_GPL(vsp1_isp_job_prepare);

/**
 * vsp1_isp_job_run - Run a buffer transfer on behalf of the ISP
 *
 * @dev: The VSP1 struct device
 */
void vsp1_isp_job_run(struct device *dev)
{
	struct vsp1_device *vsp1 = dev_get_drvdata(dev);
	struct vsp1_vspx_pipeline *vspx_pipe = &vsp1->vspx->pipe;
	struct vsp1_pipeline *pipe = &vspx_pipe->pipe;
	struct vsp1_vspx_job *job;

	scoped_guard(spinlock_irqsave, &vspx_pipe->vspx_lock) {

		if (vspx_pipe->processing)
			return;

		/* Extract one job, if available, from the jobs list. */
		scoped_guard(spinlock_irqsave, &vspx_pipe->jobs_lock) {
			job = list_first_entry_or_null(&vspx_pipe->jobs,
						       struct vsp1_vspx_job,
						       job_queue);
			if (!job)
				return;

			list_del(&job->job_queue);
		}

		vspx_pipe->processing = true;
		vsp1_dl_list_commit(job->dl, 0);
		kfree(job);
	}

	/* Trigger VSPX processing by setting VI6_CMD[STRCMD]. */
	scoped_guard(spinlock_irqsave, &pipe->irqlock) {
		vsp1_pipeline_run(pipe);
	}
}
EXPORT_SYMBOL_GPL(vsp1_isp_job_run);

/* -----------------------------------------------------------------------------
 * Initialization and cleanup
 */

int vsp1_vspx_init(struct vsp1_device *vsp1)
{
	struct vsp1_vspx_pipeline *vspx_pipe;
	struct vsp1_pipeline *pipe;

	vsp1->vspx = devm_kzalloc(vsp1->dev, sizeof(*vsp1->vspx), GFP_KERNEL);
	if (!vsp1->vspx)
		return -ENOMEM;

	vsp1->vspx->vsp1 = vsp1;

	vspx_pipe = &vsp1->vspx->pipe;
	vspx_pipe->processing = false;
	vspx_pipe->enabled = false;

	pipe = &vspx_pipe->pipe;

	vsp1_pipeline_init(pipe);

	pipe->partitions = 1;
	pipe->part_table = &vspx_pipe->partition;
	pipe->interlaced = false;
	pipe->frame_end = vsp1_vspx_pipeline_frame_end;

	INIT_LIST_HEAD(&vspx_pipe->jobs);
	spin_lock_init(&vspx_pipe->vspx_lock);
	spin_lock_init(&vspx_pipe->jobs_lock);

	/*
	 * Initialize RPF0 as inputs for VSPX and use it unconditionally for
	 * now.
	 */
	pipe->inputs[0] = vsp1->rpf[0];
	pipe->inputs[0]->entity.pipe = pipe;
	pipe->inputs[0]->entity.sink = &vsp1->iif->entity;
	vsp1_vspx_rwpf_set_subdev_fmt(vsp1, pipe->inputs[0],
				      vspx_default_fmt.pixelformat,
				      vspx_default_fmt.width,
				      vspx_default_fmt.height);
	list_add(&pipe->inputs[0]->entity.list_pipe, &pipe->entities);

	pipe->iif = &vsp1->iif->entity;
	pipe->iif->pipe = pipe;
	pipe->iif->sink = &vsp1->wpf[0]->entity;
	list_add(&pipe->iif->list_pipe, &pipe->entities);

	pipe->output = vsp1->wpf[0];
	pipe->output->entity.pipe = pipe;
	vsp1_vspx_rwpf_set_subdev_fmt(vsp1, pipe->output,
				      vspx_default_fmt.pixelformat,
				      vspx_default_fmt.width,
				      vspx_default_fmt.height);
	list_add(&pipe->output->entity.list_pipe, &pipe->entities);

	return 0;
}

void vsp1_vspx_cleanup(struct vsp1_device *vsp1)
{
	struct vsp1_vspx_pipeline *vspx_pipe;

	vspx_pipe = &vsp1->vspx->pipe;

	vsp1_vspx_release_jobs(vsp1);
}
