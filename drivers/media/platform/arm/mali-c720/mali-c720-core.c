// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <media/media-devnode.h>
#include <media/media-entity.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>

#include "mali-c720-common.h"

static void mali_c720_remove_links(struct mali_c720 *mali_c720)
{
	struct v4l2_subdev *input_sd = &mali_c720->input.mdev.sd;
	struct v4l2_subdev *isp_sd = &mali_c720->isp.mdev.sd;
	struct v4l2_subdev *hv_sd = &mali_c720->vision_pipe.mdev.sd;
	struct v4l2_subdev *fr_sd = &mali_c720->fr_pipe.mdev.sd;
	unsigned int i;

	media_entity_remove_links(&input_sd->entity);
	media_entity_remove_links(&isp_sd->entity);
	media_entity_remove_links(&hv_sd->entity);
	media_entity_remove_links(&fr_sd->entity);

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++)
		media_entity_remove_links(&mali_c720->cap_devs[i].vdev.entity);
}

static int mali_c720_create_isp_input_links(struct mali_c720 *mali_c720)
{
	struct v4l2_subdev *input_sd = &mali_c720->input.mdev.sd;
	struct v4l2_subdev *isp_sd = &mali_c720->isp.mdev.sd;
	int ret;

	ret = media_create_pad_link(&input_sd->entity,
				    MALI_C720_INPUT_PAD_SOURCE_SLOT_INPUT,
				    &isp_sd->entity,
				    MALI_C720_ISP_PAD_SINK_VIDEO,
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		dev_err(mali_c720->dev, "failed to link input and ISP sink\n");

	return ret;
}

static int mali_c720_create_vision_pipe_links(struct mali_c720 *mali_c720)
{
	struct v4l2_subdev *isp_sd = &mali_c720->isp.mdev.sd;
	struct v4l2_subdev *hv_sd = &mali_c720->vision_pipe.mdev.sd;
	int ret;

	ret = media_create_pad_link(&isp_sd->entity,
				    MALI_C720_ISP_PAD_SOURCE_HV,
				    &hv_sd->entity,
				    MALI_C720_VISION_PIPE_PAD_SINK_VIDEO,
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		dev_err(mali_c720->dev, "failed to link ISP and HV pipe\n");

	return ret;
}

static int mali_c720_create_fr_pipe_links(struct mali_c720 *mali_c720)
{
	struct v4l2_subdev *hv_sd = &mali_c720->vision_pipe.mdev.sd;
	struct v4l2_subdev *fr_sd = &mali_c720->fr_pipe.mdev.sd;
	int ret;

	ret = media_create_pad_link(&hv_sd->entity, MALI_C720_VISION_PIPE_PAD_SOURCE_RGB,
				    &fr_sd->entity, MALI_C720_FR_PIPE_PAD_SINK_VIDEO,
				    MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(mali_c720->dev, "failed to link HV and FR pipe\n");
		return ret;
	}

	ret = media_create_pad_link(&fr_sd->entity,
				    MALI_C720_FR_PIPE_PAD_SOURCE_RGB,
				    &mali_c720->cap_devs[MALI_C720_CAP_DEV_FR0_RGB].vdev.entity,
				    0, MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(mali_c720->dev,
			"failed to link FR RGB and video device\n");
		return ret;
	}

	ret = media_create_pad_link(&fr_sd->entity,
				    MALI_C720_FR_PIPE_PAD_SOURCE_YUV,
				    &mali_c720->cap_devs[MALI_C720_CAP_DEV_FR0_YUV].vdev.entity,
				    0, 0);
	if (ret)
		dev_err(mali_c720->dev,
			"failed to link FR YUV and video device\n");

	return ret;
}

static int mali_c720_create_links(struct mali_c720 *mali_c720)
{
	int ret;

	ret = mali_c720_create_isp_input_links(mali_c720);
	if (ret)
		goto err_remove_links;

	ret = mali_c720_create_vision_pipe_links(mali_c720);
	if (ret)
		goto err_remove_links;

	ret = mali_c720_create_fr_pipe_links(mali_c720);
	if (ret)
		goto err_remove_links;

	return 0;

err_remove_links:
	mali_c720_remove_links(mali_c720);
	return ret;
}

static void mali_c720_unregister_video_devices(struct mali_c720 *mali_c720)
{
	/* Prevent request queueing from racing with video device unregistration. */
	guard(mutex)(&mali_c720->media_dev.req_queue_mutex);

	mali_c720_unregister_capture_devs(mali_c720);
}

static void mali_c720_unregister_entities(struct mali_c720 *mali_c720)
{
	mali_c720_remove_links(mali_c720);
	mali_c720_unregister_fr_pipe(mali_c720);
	mali_c720_unregister_vision_pipe(mali_c720);
	mali_c720_unregister_isp(mali_c720);
	mali_c720_unregister_input(mali_c720);
}

static int mali_c720_register_entities(struct mali_c720 *mali_c720)
{
	int ret;

	ret = mali_c720_register_input(mali_c720);
	if (ret)
		goto err_unregister_entities;

	ret = mali_c720_register_isp(mali_c720);
	if (ret)
		goto err_unregister_entities;

	ret = mali_c720_register_vision_pipe(mali_c720);
	if (ret)
		goto err_unregister_entities;

	ret = mali_c720_register_fr_pipe(mali_c720);
	if (ret)
		goto err_unregister_entities;

	ret = mali_c720_register_capture_devs(mali_c720);
	if (ret)
		goto err_unregister_entities;

	ret = mali_c720_create_links(mali_c720);
	if (ret)
		goto err_unregister_entities;

	return 0;

err_unregister_entities:
	mali_c720_unregister_video_devices(mali_c720);
	mali_c720_unregister_entities(mali_c720);

	return ret;
}

static int mali_c720_register_media_nodes(struct mali_c720 *mali_c720)
{
	int ret;

	/* Keep STREAMON out until media device registration completes. */
	guard(mutex)(&mali_c720->device_lock);

	ret = v4l2_device_register_subdev_nodes(&mali_c720->v4l2_dev);
	if (ret)
		return ret;

	/* The notifier may complete again after an external subdevice rebinds. */
	if (media_devnode_is_registered(mali_c720->media_dev.devnode))
		return 0;

	return media_device_register(&mali_c720->media_dev);
}

static int mali_c720_notifier_bound(struct v4l2_async_notifier *notifier,
				    struct v4l2_subdev *subdev,
				    struct v4l2_async_connection *asc)
{
	struct mali_c720 *mali_c720 = container_of(notifier,
						struct mali_c720, notifier);

	return v4l2_create_fwnode_links(subdev, &mali_c720->input.mdev.sd);
}

static int mali_c720_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct mali_c720 *mali_c720 = container_of(notifier,
						struct mali_c720, notifier);

	return mali_c720_register_media_nodes(mali_c720);
}

static const struct v4l2_async_notifier_operations mali_c720_notifier_ops = {
	.bound = mali_c720_notifier_bound,
	.complete = mali_c720_notifier_complete,
};

static int mali_c720_parse_endpoint(struct mali_c720 *mali_c720)
{
	struct v4l2_async_notifier *ntf = &mali_c720->notifier;
	struct fwnode_handle *fwnode = dev_fwnode(mali_c720->dev);
	struct fwnode_handle *ep;
	unsigned int index = 0;
	int ret = 0;

	fwnode_graph_for_each_endpoint(fwnode, ep) {
		struct v4l2_async_connection *asc;
		struct fwnode_handle *port;
		u32 input_id = 0;

		port = fwnode_get_parent(ep);
		fwnode_property_read_u32(port, "reg", &input_id);
		fwnode_handle_put(port);

		if (input_id != 0) {
			dev_err(mali_c720->dev,
				"endpoint %pfw has invalid input port %u\n",
				ep, input_id);
			ret = -EINVAL;
			break;
		}

		asc = v4l2_async_nf_add_fwnode_remote(ntf, ep,
						      struct v4l2_async_connection);
		ret = PTR_ERR_OR_ZERO(asc);
		if (ret) {
			dev_err(mali_c720->dev,
				"failed to add remote endpoint for %pfw\n",
				ep);
			break;
		}

		index++;
	}

	mali_c720->num_async_endpoints = index;

	if (ret) {
		fwnode_handle_put(ep);
		return ret;
	}

	if (!index)
		dev_dbg(mali_c720->dev, "no remote subdevice found\n");

	return 0;
}

static void mali_c720_free(struct mali_c720 *mali_c720)
{
	kvfree(mali_c720->default_regs);
	if (mali_c720->context.mali_c720)
		mutex_destroy(&mali_c720->context.regs.allocation_lock);
	mutex_destroy(&mali_c720->device_lock);
	put_device(mali_c720->dev);
	kvfree(mali_c720);
}

static void mali_c720_release(struct v4l2_device *v4l2_dev)
{
	struct mali_c720 *mali_c720 = container_of(v4l2_dev, struct mali_c720, v4l2_dev);

	mali_c720_cleanup_capture_devs(mali_c720);
	mali_c720_cleanup_fr_pipe(mali_c720);
	mali_c720_cleanup_vision_pipe(mali_c720);
	mali_c720_cleanup_isp(mali_c720);
	mali_c720_cleanup_input(mali_c720);
	media_device_cleanup(&mali_c720->media_dev);
	mali_c720_free(mali_c720);
}

/*
 * Initialize the media frameworks and register the entities and links. Publish
 * the media device once all external subdevices have bound.
 */
static int mali_c720_media_frameworks_init(struct mali_c720 *mali_c720)
{
	int ret;

	strscpy(mali_c720->media_dev.model, "ARM Mali-C720 ISP",
		sizeof(mali_c720->media_dev.model));

	media_device_init(&mali_c720->media_dev);

	mali_c720->v4l2_dev.mdev = &mali_c720->media_dev;
	ret = v4l2_device_register(mali_c720->dev, &mali_c720->v4l2_dev);
	if (ret) {
		dev_err(mali_c720->dev, "Failed to register V4L2 device\n");
		media_device_cleanup(&mali_c720->media_dev);
		return ret;
	}
	mali_c720->v4l2_dev.release = mali_c720_release;

	mali_c720->notifier.ops = &mali_c720_notifier_ops;
	v4l2_async_nf_init(&mali_c720->notifier, &mali_c720->v4l2_dev);

	ret = mali_c720_register_entities(mali_c720);
	if (ret) {
		dev_err(mali_c720->dev, "failed to register entities\n");
		goto err_cleanup_nf;
	}

	ret = mali_c720_parse_endpoint(mali_c720);
	if (ret)
		goto err_unregister_entities;

	if (mali_c720->num_async_endpoints) {
		ret = v4l2_async_nf_register(&mali_c720->notifier);
		if (ret) {
			dev_err(mali_c720->dev, "failed to register notifier\n");
			goto err_unregister_entities;
		}
	} else {
		ret = mali_c720_register_media_nodes(mali_c720);
		if (ret) {
			dev_err(mali_c720->dev, "failed to register media nodes\n");
			goto err_unregister_entities;
		}
	}

	return 0;

err_unregister_entities:
	mali_c720_unregister_video_devices(mali_c720);
	mali_c720_unregister_entities(mali_c720);
err_cleanup_nf:
	v4l2_async_nf_cleanup(&mali_c720->notifier);
	v4l2_device_unregister(&mali_c720->v4l2_dev);

	return ret;
}

static void mali_c720_media_frameworks_deinit(struct mali_c720 *mali_c720)
{
	/*
	 * We unregister video devices first to ensure streaming is stopped
	 * before subdevices are unregistered. The async notifier comes next
	 * since the .bound() callback creates media links to our subdevices.
	 */

	mali_c720_unregister_video_devices(mali_c720);
	v4l2_async_nf_unregister(&mali_c720->notifier);
	mali_c720_unregister_entities(mali_c720);
	v4l2_async_nf_cleanup(&mali_c720->notifier);
	v4l2_device_unregister(&mali_c720->v4l2_dev);
	media_device_unregister(&mali_c720->media_dev);
}

static struct media_request *mali_c720_req_alloc(struct media_device *mdev)
{
	struct mali_c720_request *mreq;

	mreq = kzalloc_obj(*mreq, GFP_KERNEL);
	if (!mreq)
		return NULL;

	return &mreq->req;
}

static void mali_c720_req_free(struct media_request *req)
{
	kfree(to_mali_c720_request(req));
}

static int mali_c720_req_validate(struct media_request *req)
{
	struct media_request_object *obj;

	/* Request-associated controls are not supported. */
	list_for_each_entry(obj, &req->objects, list) {
		if (!vb2_request_object_is_buffer(obj))
			return -EINVAL;
	}

	return vb2_request_validate(req);
}

static void mali_c720_req_queue(struct media_request *req)
{
	struct mali_c720 *mali_c720 = container_of(req->mdev, struct mali_c720, media_dev);
	struct mali_c720_context *ctx = &mali_c720->context;
	struct mali_c720_request *mreq = to_mali_c720_request(req);

	scoped_guard(spinlock, &ctx->req_lock) {
		mreq->submitted = false;
		mreq->queue_sequence = ctx->request_sequence++;
	}

	vb2_request_queue(req);

	scoped_guard(spinlock, &ctx->req_lock) {
		mreq->submitted = true;
	}

	mali_c720_try_schedule_frame(ctx);
}

static const struct media_device_ops mali_c720_media_device_ops = {
	.req_alloc = mali_c720_req_alloc,
	.req_free = mali_c720_req_free,
	.req_validate = mali_c720_req_validate,
	.req_queue = mali_c720_req_queue,
};

int mali_c720_pipeline_ready(struct mali_c720 *mali_c720)
{
	struct media_pipeline_entity_iter iter;
	struct media_entity *entity;
	int n_video_devices = 0;
	int ret;

	ret = media_pipeline_entity_iter_init(&mali_c720->pipe, &iter);
	if (ret)
		return ret;

	media_pipeline_for_each_entity(&mali_c720->pipe, &iter, entity) {
		if (entity->obj_type == MEDIA_ENTITY_TYPE_VIDEO_DEVICE)
			n_video_devices++;
	}

	media_pipeline_entity_iter_cleanup(&iter);

	return n_video_devices == mali_c720->pipe.start_count ? 0 : -EAGAIN;
}

int mali_c720_streamon(struct file *file, void *priv, enum v4l2_buf_type type)
{
	struct video_device *vdev = video_devdata(file);
	struct mali_c720 *mali_c720 = container_of(vdev->v4l2_dev, struct mali_c720, v4l2_dev);
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	int ret;

	ret = vb2_ioctl_streamon(file, priv, type);
	if (ret)
		return ret;

	guard(mutex)(&mali_c720->device_lock);

	/*
	 * The final STREAMON prepares the context and starts its inputs. Wait
	 * until VB2 marks that queue streaming before starting the context
	 * and allowing scheduling to take place.
	 */
	scoped_guard(spinlock, &ctx->state_lock) {
		if (ctx->state != MALI_C720_SLOT_STATE_PREPARED)
			return 0;

		mali_c720_ctx_mgr_start_context(ctx);
	}

	mali_c720_try_schedule_frame(ctx);

	return 0;
}

static bool mali_c720_has_ready_output(struct mali_c720 *mali_c720)
{
	unsigned int i;

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++) {
		if (!mali_c720_cap_dev_is_ready(&mali_c720->cap_devs[i]))
			return true;
	}

	return false;
}

static struct media_request *
mali_c720_oldest_request(struct media_request *oldest, struct media_request *candidate)
{
	struct mali_c720_request *mreq;

	if (!candidate)
		return oldest;

	mreq = to_mali_c720_request(candidate);

	/* Ignore partially submitted requests */
	if (!mreq->submitted)
		return oldest;

	if (!oldest ||
	    (int)(mreq->queue_sequence - to_mali_c720_request(oldest)->queue_sequence) < 0)
		return candidate;

	return oldest;
}

static struct media_request *mali_c720_find_request(struct mali_c720 *mali_c720)
{
	struct media_request *candidate, *req = NULL;
	unsigned int i;

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++) {
		candidate = mali_c720_cap_dev_get_request(&mali_c720->cap_devs[i]);
		req = mali_c720_oldest_request(req, candidate);
	}

	return req;
}

static bool mali_c720_uses_requests(struct mali_c720 *mali_c720)
{
	unsigned int i;

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++) {
		if (mali_c720_cap_dev_uses_requests(&mali_c720->cap_devs[i]))
			return true;
	}

	return false;
}

static bool mali_c720_can_schedule(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;

	/* Ensure the first free-running frame has at least one ready output. */
	if (ctx->state == MALI_C720_SLOT_STATE_STARTING &&
	    !mali_c720_has_ready_output(mali_c720))
		return false;

	return true;
}

static struct media_request *mali_c720_can_schedule_request(struct mali_c720_context *ctx)
{
	return mali_c720_find_request(ctx->mali_c720);
}

void mali_c720_try_schedule_frame(struct mali_c720_context *ctx)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	struct media_request *req = NULL;
	unsigned int i;

	guard(spinlock)(&ctx->state_lock);

	if ((ctx->state != MALI_C720_SLOT_STATE_STARTING &&
	     ctx->state != MALI_C720_SLOT_STATE_STARTED) || ctx->pipeline_busy)
		return;

	scoped_guard(spinlock, &ctx->req_lock) {
		if (mali_c720_uses_requests(mali_c720)) {
			req = mali_c720_can_schedule_request(ctx);
			if (!req)
				return;
		} else if (!mali_c720_can_schedule(ctx)) {
			return;
		}

		for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++)
			mali_c720_cap_dev_set_next_buffer(&mali_c720->cap_devs[i], req);
	}

	ctx->pipeline_busy = true;
	mali_c720_ctx_mgr_prepare_frame(ctx);

	/* Resizer configurations should be synchronised with CDMA */
	mali_c720_fr_pipe_apply_resizer_config(&mali_c720->fr_pipe);

	mali_c720_ctx_mgr_handle_sw_ready(ctx);
}

static int mali_c720_power_on(struct mali_c720 *mali_c720)
{
	int ret;

	ret = clk_prepare_enable(mali_c720->clk);
	if (ret) {
		dev_err(mali_c720->dev, "failed to enable clocks\n");
		return ret;
	}

	ret = reset_control_deassert(mali_c720->reset);
	if (ret) {
		dev_err(mali_c720->dev, "failed to deassert resets\n");
		goto err_disable_clk;
	}

	return 0;

err_disable_clk:
	clk_disable_unprepare(mali_c720->clk);
	return ret;
}

static void mali_c720_power_off(struct mali_c720 *mali_c720)
{
	reset_control_assert(mali_c720->reset);
	clk_disable_unprepare(mali_c720->clk);
}

static int __maybe_unused mali_c720_runtime_resume(struct device *dev)
{
	struct mali_c720 *mali_c720 = dev_get_drvdata(dev);

	return mali_c720_power_on(mali_c720);
}

static int __maybe_unused mali_c720_runtime_suspend(struct device *dev)
{
	struct mali_c720 *mali_c720 = dev_get_drvdata(dev);

	mali_c720_power_off(mali_c720);
	return 0;
}

static int __maybe_unused mali_c720_suspend(struct device *dev)
{
	struct mali_c720 *mali_c720 = dev_get_drvdata(dev);

	guard(mutex)(&mali_c720->device_lock);

	/* Active pipelines cannot survive the hardware reset during suspend. */
	if (media_entity_is_streaming(&mali_c720->isp.mdev.sd.entity))
		return -EBUSY;

	return pm_runtime_force_suspend(dev);
}

static const struct dev_pm_ops mali_c720_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mali_c720_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(mali_c720_runtime_suspend, mali_c720_runtime_resume,
			   NULL)
};

static int mali_c720_probe(struct platform_device *pdev)
{
	const struct mali_c720_discovery_target top = {
		.module_id = MALI_C720_DISC_MODULE_ID_PIPELINE_GLOBAL_CONTROL,
		.pipeline_id = MALI_C720_DISC_PIPE_GLOBAL,
	};
	struct device *dev = &pdev->dev;
	struct mali_c720 *mali_c720;
	struct resource *res;
	unsigned int irq;
	int ret;

	mali_c720 = kvzalloc_obj(*mali_c720, GFP_KERNEL);
	if (!mali_c720)
		return -ENOMEM;

	mali_c720->dev = get_device(dev);
	platform_set_drvdata(pdev, mali_c720);

	mutex_init(&mali_c720->device_lock);

	mali_c720->base = devm_platform_get_and_ioremap_resource(pdev, 0,
								 &res);
	ret = PTR_ERR_OR_ZERO(mali_c720->base);
	if (ret) {
		dev_err_probe(dev, ret, "failed to map IO memory\n");
		goto err_free;
	}

	mali_c720->mmio_size = resource_size(res);

	if (resource_size(res) < MALI_C720_ID_SIZE) {
		ret = dev_err_probe(dev, -EINVAL,
				    "IO memory resource %pR is too small (minimum %#x bytes)\n",
				    res, MALI_C720_ID_SIZE);
		goto err_free;
	}

	mali_c720->clk = devm_clk_get(dev, "vclk");
	ret = PTR_ERR_OR_ZERO(mali_c720->clk);
	if (ret) {
		dev_err_probe(dev, ret, "failed to acquire clock\n");
		goto err_free;
	}

	mali_c720->reset = devm_reset_control_get_optional_shared(dev, "reset");
	ret = PTR_ERR_OR_ZERO(mali_c720->reset);
	if (ret) {
		dev_err_probe(dev, ret, "failed to acquire reset\n");
		goto err_free;
	}

	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV) {
		dev_err_probe(dev, ret, "failed to initialize reserved memory\n");
		goto err_free;
	}

	ret = vb2_dma_contig_set_max_seg_size(dev, UINT_MAX);
	if (ret)
		goto err_release_mem;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48));
	if (ret) {
		dev_err_probe(dev, ret, "failed to set DMA mask\n");
		goto err_release_mem;
	}

	ret = mali_c720_power_on(mali_c720);
	if (ret) {
		dev_err(dev, "failed to power on: %d\n", ret);
		goto err_release_mem;
	}

	mali_c720->registers.id = mali_c720_get_id_regs();
	ret = mali_c720_identify_hw(mali_c720);
	if (ret)
		goto err_power_off;

	ret = mali_c720_get_registers(mali_c720->hw_version->id, &mali_c720->registers);
	if (ret) {
		dev_err_probe(dev, ret, "unsupported register layout\n");
		goto err_power_off;
	}

	if (resource_size(res) < mali_c720->registers.context_size) {
		ret = dev_err_probe(dev, -EINVAL,
				    "IO memory resource %pR is too small (minimum %#x bytes)\n",
				    res, mali_c720->registers.context_size);
		goto err_power_off;
	}

	ret = mali_c720_discovery_init(mali_c720, resource_size(res));
	if (ret) {
		dev_err(dev, "failed to parse capability discovery data\n");
		goto err_power_off;
	}

	ret = mali_c720_discovery_get_module_base(mali_c720, &top, &mali_c720->top_base);
	if (ret) {
		dev_err(dev, "failed to get top base address from discovery data\n");
		goto err_power_off;
	}

	ret = mali_c720_init_contexts(mali_c720);
	if (ret)
		goto err_power_off;

	ret = mali_c720_init_ctx_mgr(mali_c720);
	if (ret)
		goto err_power_off;

	pm_runtime_set_autosuspend_delay(&pdev->dev, 2000);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = platform_get_irq_byname(pdev, "isp");
	if (ret < 0) {
		dev_err(dev, "Failed to get interrupt\n");
		goto err_pm_runtime_disable;
	}
	irq = (unsigned int)ret;

	spin_lock_init(&mali_c720->irq_lock);
	INIT_KFIFO(mali_c720->irq_fifo);
	ret = devm_request_threaded_irq(&pdev->dev, irq,
					mali_c720_isr_top, mali_c720_isr,
					0,
					dev_driver_string(&pdev->dev),
					&pdev->dev);
	if (ret) {
		dev_err(dev, "Failed to request threaded IRQ\n");
		goto err_pm_runtime_disable;
	}
	mali_c720->irq = irq;

	mali_c720->media_dev.dev = dev;
	mali_c720->media_dev.ops = &mali_c720_media_device_ops;
	ret = mali_c720_media_frameworks_init(mali_c720);
	if (ret)
		goto err_free_irq;

	pm_runtime_idle(&pdev->dev);

	dev_info(&pdev->dev, "Media frameworks and entities registered successfully\n");

	return 0;

err_free_irq:
	devm_free_irq(dev, mali_c720->irq, dev);
err_pm_runtime_disable:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
err_power_off:
	mali_c720_power_off(mali_c720);
err_release_mem:
	of_reserved_mem_device_release(dev);
err_free:
	platform_set_drvdata(pdev, NULL);
	if (mali_c720->v4l2_dev.release)
		v4l2_device_put(&mali_c720->v4l2_dev);
	else
		mali_c720_free(mali_c720);

	return ret;
}

static void mali_c720_remove(struct platform_device *pdev)
{
	struct mali_c720 *mali_c720 = platform_get_drvdata(pdev);

	/* Keep the current power state until queues and IRQs have been drained. */
	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_barrier(&pdev->dev);

	mali_c720_media_frameworks_deinit(mali_c720);
	devm_free_irq(&pdev->dev, mali_c720->irq, &pdev->dev);

	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		mali_c720_power_off(mali_c720);
	pm_runtime_set_suspended(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	of_reserved_mem_device_release(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	v4l2_device_put(&mali_c720->v4l2_dev);
}

static const struct of_device_id mali_c720_of_match[] = {
	{ .compatible = "arm,mali-c720" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mali_c720_of_match);

static struct platform_driver mali_c720_driver = {
	.driver		= {
		.name		= "mali-c720",
		.of_match_table	= mali_c720_of_match,
		.pm = &mali_c720_pm_ops,
	},
	.probe		= mali_c720_probe,
	.remove		= mali_c720_remove,
};

module_platform_driver(mali_c720_driver);

MODULE_DESCRIPTION("Mali C720 Core driver");
MODULE_AUTHOR("Anthony McGivern <Anthony.McGivern@arm.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
