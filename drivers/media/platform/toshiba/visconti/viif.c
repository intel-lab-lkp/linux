// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-fwnode.h>

#include "viif.h"
#include "viif_capture.h"
#include "viif_csi2rx.h"
#include "viif_common.h"
#include "viif_isp.h"
#include "viif_regs.h"

/*=============================================*/
/* Register Access */
/*=============================================*/
static inline void viif_hwaif_write(struct viif_device *viif_dev, unsigned int regid, u32 val)
{
	writel(val, viif_dev->hwaif_reg + regid);
}

static inline void viif_mpu_write(struct viif_device *viif_dev, unsigned int regid, u32 val)
{
	writel(val, viif_dev->mpu_reg + regid);
}

/*=============================================*/
/* Low Layer Interrupt handler */
/*=============================================*/
static void viif_main_status_err_set_irq_mask(struct viif_device *viif_dev, u32 mask)
{
	viif_capture_write(viif_dev, REG_INT_M_MASK, mask);
}

static void viif_main_vsync_set_irq_mask(struct viif_device *viif_dev, u32 mask)
{
	viif_capture_write(viif_dev, REG_INT_M_SYNC_MASK, mask);
}

static void viif_sub_status_err_set_irq_mask(struct viif_device *viif_dev, u32 mask)
{
	viif_capture_write(viif_dev, REG_INT_S_MASK, mask);
}

static void viif_sub_vsync_set_irq_mask(struct viif_device *viif_dev, u32 mask)
{
	viif_capture_write(viif_dev, REG_INT_S_SYNC_MASK, mask);
}

/* IRQ handler: read and clear error status, report masked result */
static void viif_read_err_irq_registers(struct viif_device *viif_dev, u32 *event_main,
					u32 *event_sub)
{
	u32 val, mask;

	*event_main = 0;
	*event_sub = 0;

	val = viif_capture_read(viif_dev, REG_INT_M_STATUS);
	mask = viif_capture_read(viif_dev, REG_INT_M_MASK);
	val &= ~mask;
	if (val) {
		viif_capture_write(viif_dev, REG_INT_M_STATUS, val);
		*event_main = val;
	}

	val = viif_capture_read(viif_dev, REG_INT_S_STATUS);
	mask = viif_capture_read(viif_dev, REG_INT_S_MASK);
	val &= ~mask;
	if (val) {
		viif_capture_write(viif_dev, REG_INT_S_STATUS, val);
		*event_sub = val;
	}
}

/* IRQ handler: read and clear vsync status, report masked result */
static void viif_read_vsynq_irq_registers(struct viif_device *viif_dev, u32 *event_main,
					  u32 *event_sub)
{
	u32 val, mask;

	*event_main = 0;
	*event_sub = 0;

	val = viif_capture_read(viif_dev, REG_INT_M_SYNC);
	mask = viif_capture_read(viif_dev, REG_INT_M_SYNC_MASK);
	val &= ~mask;
	if (val) {
		viif_capture_write(viif_dev, REG_INT_M_SYNC, val);
		*event_main = val;
	}

	val = viif_capture_read(viif_dev, REG_INT_S_SYNC);
	mask = viif_capture_read(viif_dev, REG_INT_S_SYNC_MASK);
	val &= ~mask;
	if (val) {
		viif_capture_write(viif_dev, REG_INT_S_SYNC, val);
		*event_sub = val;
	}
}

/* IRQ handler: read and clear L2ISP status, report result */
static u32 viif_get_l2_transfer_status(struct viif_device *viif_dev)
{
	u32 l2_status;

	l2_status = viif_capture_read(viif_dev, REG_L2_CRGBF_ISP_INT);
	viif_capture_write(viif_dev, REG_L2_CRGBF_ISP_INT, l2_status);
	return l2_status & MASK_L2_STATUS_ERR_ALL;
}

/*=============================================*/
/* Low Layer hardware setup */
/*=============================================*/
static void viif_mpu_disable(struct viif_device *viif_dev)
{
	viif_mpu_write(viif_dev, REG_MPU_MP_EN, 0);
	viif_mpu_write(viif_dev, REG_MPU_MF_EN, 1);
}

static void viif_hwaif_enable(struct viif_device *viif_dev)
{
	/* pass through; disable all entries */
	viif_hwaif_write(viif_dev, REG_HWAIF_REGION_ENTRY_EN, 0);

	/* no limit for outstanding requests */
	viif_hwaif_write(viif_dev, REG_HWAIF_OSTD_RLEN, 0);
	viif_hwaif_write(viif_dev, REG_HWAIF_OSTD_WREQ, 0);

	/* no data-pack/outstanding */
	viif_hwaif_write(viif_dev, REG_HWAIF_HWAIF_CONF, 0);

	/* enable bus access */
	viif_hwaif_write(viif_dev, REG_HWAIF_HWAIF_EN, 1);
}

/*=============================================*/
/* handling V4L2 framework */
/*=============================================*/
static void visconti_viif_hw_on(struct viif_device *viif_dev)
{
	/* Disable MPU */
	viif_mpu_disable(viif_dev);
	/* Enable HWAIF */
	viif_hwaif_enable(viif_dev);
}

static void visconti_viif_hw_off(struct viif_device *viif_dev)
{
	/* Uninitialize HWD driver */
}

static inline struct viif_device *v4l2_to_viif(struct v4l2_device *v4l2_dev)
{
	return container_of(v4l2_dev, struct viif_device, v4l2_dev);
}

/* This function runs in work queue context */
/* Reading ISP registers takes 30us. */
/* Please note that this function should be finished */
/* before a userland capture application is trigered by vb2_buffer_done() */
static void visconti_viif_wthread_l1info(struct work_struct *work)
{
	/* called function is implemented by the next patch */
/*
 *	struct viif_device *viif_dev = container_of(work, struct viif_device, work);
 *
 *	visconti_viif_save_l1_info(viif_dev);
 */
}

static void viif_vsync_irq_handler_w_isp(struct viif_device *viif_dev)
{
	u32 event_main, event_sub, status_err, l2_transfer_status;
	u64 ts;

	ts = ktime_get_ns();
	viif_read_vsynq_irq_registers(viif_dev, &event_main, &event_sub);

	/* Delayed Vsync of MAIN unit */
	if (event_main & MASK_INT_M_SYNC_LINES_DELAY_INT2) {
		/* unmask timeout error of gamma table */
		viif_main_status_err_set_irq_mask(viif_dev, MASK_INT_M_DELAY_INT_ERROR);
		viif_dev->masked_gamma_path = 0;

		/* Get abort status of L2ISP */
		spin_lock(&viif_dev->regbuf_lock);
		hwd_viif_isp_guard_start(viif_dev);
		l2_transfer_status = viif_get_l2_transfer_status(viif_dev);
		hwd_viif_isp_guard_end(viif_dev);
		spin_unlock(&viif_dev->regbuf_lock);

		status_err = viif_dev->status_err;
		viif_dev->status_err = 0;

		visconti_viif_capture_switch_buffer(&viif_dev->cap_dev0, status_err,
						    l2_transfer_status, ts);
		visconti_viif_capture_switch_buffer(&viif_dev->cap_dev1, status_err,
						    l2_transfer_status, ts);
		queue_work(viif_dev->wq, &viif_dev->work);
	}

	/* Delayed Vsync of SUB unit */
	if (event_sub & MASK_INT_S_SYNC_LINES_DELAY_INT1)
		visconti_viif_capture_switch_buffer(&viif_dev->cap_dev2, 0, 0, ts);
}

static void viif_status_err_irq_handler(struct viif_device *viif_dev)
{
	u32 event_main, event_sub, val, mask;

	viif_read_err_irq_registers(viif_dev, &event_main, &event_sub);

	if (event_main) {
		/* mask for gamma table time out error which will be unmasked in the next Vsync */
		val = FIELD_GET(MASK_INT_M_L2ISP_GAMMA_TABLE_TIMEOUT, event_main);
		if (val) {
			viif_dev->masked_gamma_path |= val;
			mask = MASK_INT_M_DELAY_INT_ERROR |
			       FIELD_PREP(MASK_INT_M_L2ISP_GAMMA_TABLE_TIMEOUT,
					  viif_dev->masked_gamma_path);
			viif_main_status_err_set_irq_mask(viif_dev, mask);
		}

		viif_dev->status_err = event_main;
	}
	viif_dev->reported_err_main |= event_main;
	viif_dev->reported_err_sub |= event_sub;
	dev_err(viif_dev->dev, "MAIN/SUB error 0x%x 0x%x.\n", event_main, event_sub);
}

static void viif_csi2rx_err_irq_handler(struct viif_device *viif_dev)
{
	u32 event;

	event = visconti_viif_csi2rx_err_irq_handler(viif_dev);
	viif_dev->reported_err_csi2rx |= event;
	dev_err(viif_dev->dev, "CSI2RX error 0x%x.\n", event);
}

static irqreturn_t visconti_viif_irq(int irq, void *dev_id)
{
	struct viif_device *viif_dev = dev_id;
	int irq_type = irq - viif_dev->irq[0];

	switch (irq_type) {
	case 0:
		viif_vsync_irq_handler_w_isp(viif_dev);
		break;
	case 1:
		viif_status_err_irq_handler(viif_dev);
		break;
	case 2:
		viif_csi2rx_err_irq_handler(viif_dev);
		break;
	}

	return IRQ_HANDLED;
}

/* ----- Async Notifier Operations----- */
static int visconti_viif_create_sensor_link(struct viif_device *viif_dev)
{
	struct v4l2_subdev *sensor_sd = viif_dev->sensor_sd;
	int source_pad;
	int ret;

	if (!sensor_sd)
		return -EINVAL;

	/* camera subdev pad0 -> isp suddev pad0 */
	source_pad = media_entity_get_fwnode_pad(&sensor_sd->entity, sensor_sd->fwnode,
						 MEDIA_PAD_FL_SOURCE);
	if (source_pad < 0) {
		dev_err(viif_dev->dev, "failed to find source pad\n");
		return source_pad;
	}

	ret = media_create_pad_link(&sensor_sd->entity, source_pad,
				    &viif_dev->csi2rx_subdev.sd.entity, VIIF_CSI2RX_PAD_SINK,
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		dev_err(viif_dev->dev, "failed create_pad_link (camera:src -> csi2rx:sink)\n");

	return ret;
}

static int visconti_viif_notify_bound(struct v4l2_async_notifier *notifier,
				      struct v4l2_subdev *v4l2_sd,
				      struct v4l2_async_connection *asc)
{
	struct viif_device *viif_dev = container_of(notifier, struct viif_device, notifier);
	struct viif_sensor_async *s_as = container_of(asc, struct viif_sensor_async, asc);

	s_as->v4l2_sd = v4l2_sd;
	if (!s_as->index) {
		viif_dev->sensor_sd = v4l2_sd;
		viif_dev->sensor_num_lane = s_as->num_lane;
		return visconti_viif_create_sensor_link(viif_dev);
	}

	return 0;
}

static void visconti_viif_notify_unbind(struct v4l2_async_notifier *notifier,
					struct v4l2_subdev *subdev,
					struct v4l2_async_connection *asc)
{
	struct viif_device *viif_dev = container_of(notifier, struct viif_device, notifier);

	if (viif_dev->sensor_sd == subdev)
		viif_dev->sensor_sd = NULL;
}

static int visconti_viif_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct v4l2_device *v4l2_dev = notifier->v4l2_dev;
	struct viif_device *viif_dev = v4l2_to_viif(v4l2_dev);
	int ret;

	ret = v4l2_device_register_subdev_nodes(v4l2_dev);
	if (ret)
		return ret;

	return visconti_viif_capture_register_ctrl_handlers(viif_dev);
}

static const struct v4l2_async_notifier_operations viif_notify_ops = {
	.bound = visconti_viif_notify_bound,
	.unbind = visconti_viif_notify_unbind,
	.complete = visconti_viif_notify_complete,
};

/* ----- Probe and Remove ----- */
static int visconti_viif_subdev_notifier_register(struct viif_device *viif_dev)
{
	struct fwnode_handle *fwnode = dev_fwnode(viif_dev->dev);
	struct v4l2_async_notifier *ntf = &viif_dev->notifier;
	struct fwnode_handle *ep;
	unsigned int index = 0;
	int ret = 0;

	v4l2_async_nf_init(ntf, &viif_dev->v4l2_dev);
	ntf->ops = &viif_notify_ops;

	fwnode_graph_for_each_endpoint(fwnode, ep) {
		struct v4l2_fwnode_endpoint vep = {};
		struct viif_sensor_async *viif_asd;

		ret = v4l2_fwnode_endpoint_parse(ep, &vep);
		if (ret) {
			dev_err(viif_dev->dev, "failed to parse endpoint %pfw\n", ep);
			break;
		}

		if (vep.bus_type != V4L2_MBUS_CSI2_DPHY || vep.bus.mipi_csi2.num_data_lanes == 0) {
			dev_warn(viif_dev->dev, "missing CSI-2 properties in endpoint %pfw\n", ep);
			break;
		}

		viif_asd = v4l2_async_nf_add_fwnode_remote(ntf, ep, struct viif_sensor_async);
		viif_asd->index = index++;
		viif_asd->num_lane = vep.bus.mipi_csi2.num_data_lanes;
	}

	if (ret) {
		fwnode_handle_put(ep);
		v4l2_async_nf_cleanup(ntf);
		return ret;
	}

	if (!index)
		dev_dbg(viif_dev->dev, "No remote subdevice found\n");

	ret = v4l2_async_nf_register(ntf);
	if (ret) {
		v4l2_async_nf_cleanup(ntf);
		return ret;
	}

	return 0;
}

static int visconti_viif_create_links(struct viif_device *viif_dev)
{
	int ret;

	ret = media_create_pad_link(&viif_dev->csi2rx_subdev.sd.entity, VIIF_CSI2RX_PAD_SRC,
				    &viif_dev->isp_subdev.sd.entity, VIIF_ISP_PAD_SINK,
				    MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(viif_dev->dev, "failed create_pad_link (csi2rx:src -> isp:sink)\n");
		return ret;
	}

	ret = media_create_pad_link(&viif_dev->isp_subdev.sd.entity, VIIF_ISP_PAD_SRC_PATH0,
				    &viif_dev->cap_dev0.vdev.entity, VIIF_CAPTURE_PAD_SINK,
				    MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(viif_dev->dev, "failed create_pad_link (isp:src -> capture0:sink)\n");
		return ret;
	}

	ret = media_create_pad_link(&viif_dev->isp_subdev.sd.entity, VIIF_ISP_PAD_SRC_PATH1,
				    &viif_dev->cap_dev1.vdev.entity, VIIF_CAPTURE_PAD_SINK,
				    MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(viif_dev->dev, "failed create_pad_link (isp:src -> capture1:sink)\n");
		return ret;
	}

	ret = media_create_pad_link(&viif_dev->isp_subdev.sd.entity, VIIF_ISP_PAD_SRC_PATH2,
				    &viif_dev->cap_dev2.vdev.entity, VIIF_CAPTURE_PAD_SINK,
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		dev_err(viif_dev->dev, "failed create_pad_link (isp:src -> capture2:sink)\n");

	return ret;
}

static const struct of_device_id visconti_viif_of_table[] = {
	{
		.compatible = "toshiba,visconti5-viif",
	},
	{},
};
MODULE_DEVICE_TABLE(of, visconti_viif_of_table);

#define NUM_IRQS   3
#define IRQ_ID_STR "viif"

static int visconti_viif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct viif_device *viif_dev;
	dma_addr_t tables_dma;
	int ret, i;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
	if (ret)
		return ret;

	viif_dev = devm_kzalloc(dev, sizeof(*viif_dev), GFP_KERNEL);
	if (!viif_dev)
		return -ENOMEM;

	platform_set_drvdata(pdev, viif_dev);
	viif_dev->dev = dev;

	spin_lock_init(&viif_dev->regbuf_lock);
	mutex_init(&viif_dev->stream_lock);

	viif_dev->capture_reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(viif_dev->capture_reg))
		return PTR_ERR(viif_dev->capture_reg);

	viif_dev->csi2host_reg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(viif_dev->csi2host_reg))
		return PTR_ERR(viif_dev->csi2host_reg);

	viif_dev->hwaif_reg = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(viif_dev->hwaif_reg))
		return PTR_ERR(viif_dev->hwaif_reg);

	viif_dev->mpu_reg = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(viif_dev->mpu_reg))
		return PTR_ERR(viif_dev->mpu_reg);

	viif_dev->run_flag_main = false;

	for (i = 0; i < NUM_IRQS; i++) {
		ret = platform_get_irq(pdev, i);
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to acquire irq resource\n");
		viif_dev->irq[i] = ret;
		ret = devm_request_irq(dev, viif_dev->irq[i], visconti_viif_irq, 0, IRQ_ID_STR,
				       viif_dev);
		if (ret)
			return dev_err_probe(dev, ret, "irq request failed\n");
	}

	viif_dev->tables =
		dma_alloc_wc(dev, sizeof(struct viif_table_area), &tables_dma, GFP_KERNEL);
	if (!viif_dev->tables)
		return -ENOMEM;
	viif_dev->tables_dma = (struct viif_table_area *)tables_dma;

	pm_runtime_enable(dev);

	/* build media_dev */
	viif_dev->media_dev.hw_revision = 0;
	strscpy(viif_dev->media_dev.model, VIIF_DRIVER_NAME, sizeof(viif_dev->media_dev.model));
	viif_dev->media_dev.dev = dev;
	/* TODO: platform:visconti-viif-0,1,2,3 for each VIIF driver instance */
	snprintf(viif_dev->media_dev.bus_info, sizeof(viif_dev->media_dev.bus_info), "%s-0",
		 VIIF_BUS_INFO_BASE);
	media_device_init(&viif_dev->media_dev);

	/* build v4l2_dev */
	viif_dev->v4l2_dev.mdev = &viif_dev->media_dev;
	ret = v4l2_device_register(dev, &viif_dev->v4l2_dev);
	if (ret)
		goto error_dma_free;

	ret = media_device_register(&viif_dev->media_dev);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to register media device\n");
		goto error_v4l2_unregister;
	}

	ret = visconti_viif_csi2rx_register(viif_dev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register csi2rx sub node\n");
		goto error_media_unregister;
	}

	ret = visconti_viif_isp_register(viif_dev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register isp sub node\n");
		goto error_csi2rx_unregister;
	}

	ret = visconti_viif_capture_register(viif_dev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register capture node\n");
		goto error_isp_unregister;
	}

	ret = visconti_viif_create_links(viif_dev);
	if (ret)
		goto error_capture_unregister;

	visconti_viif_subdev_notifier_register(viif_dev);
	if (ret)
		goto error_capture_unregister;

	viif_dev->wq = create_workqueue("visconti-viif");
	if (!viif_dev->wq) {
		ret = dev_err_probe(dev, -ENOMEM, "failed to create workqueue\n");
		goto error_notifier_unregister;
	}
	INIT_WORK(&viif_dev->work, visconti_viif_wthread_l1info);

	return 0;

error_notifier_unregister:
	v4l2_async_nf_unregister(&viif_dev->notifier);
	v4l2_async_nf_cleanup(&viif_dev->notifier);
error_capture_unregister:
	visconti_viif_capture_unregister(viif_dev);
error_isp_unregister:
	visconti_viif_isp_unregister(viif_dev);
error_csi2rx_unregister:
	visconti_viif_csi2rx_unregister(viif_dev);
error_media_unregister:
	media_device_unregister(&viif_dev->media_dev);
error_v4l2_unregister:
	v4l2_device_unregister(&viif_dev->v4l2_dev);
error_dma_free:
	pm_runtime_disable(dev);
	dma_free_wc(&pdev->dev, sizeof(struct viif_table_area), viif_dev->tables,
		    (dma_addr_t)viif_dev->tables_dma);
	return ret;
}

static int visconti_viif_remove(struct platform_device *pdev)
{
	struct viif_device *viif_dev = platform_get_drvdata(pdev);

	destroy_workqueue(viif_dev->wq);
	v4l2_async_nf_unregister(&viif_dev->notifier);
	v4l2_async_nf_cleanup(&viif_dev->notifier);
	visconti_viif_capture_unregister(viif_dev);
	visconti_viif_isp_unregister(viif_dev);
	visconti_viif_csi2rx_unregister(viif_dev);
	media_device_unregister(&viif_dev->media_dev);
	v4l2_device_unregister(&viif_dev->v4l2_dev);

	pm_runtime_disable(&pdev->dev);
	dma_free_wc(&pdev->dev, sizeof(struct viif_table_area), viif_dev->tables,
		    (dma_addr_t)viif_dev->tables_dma);

	return 0;
}

static int __maybe_unused visconti_viif_runtime_suspend(struct device *dev)
{
	/* This callback is kicked when the last device-file is closed */
	struct viif_device *viif_dev = dev_get_drvdata(dev);

	visconti_viif_hw_off(viif_dev);

	return 0;
}

static int __maybe_unused visconti_viif_runtime_resume(struct device *dev)
{
	/* This callback is kicked when the first device-file is opened */
	struct viif_device *viif_dev = dev_get_drvdata(dev);

	viif_dev->rawpack_mode = (u32)VIIF_RAWPACK_DISABLE;

	/* Initialize HWD driver */
	visconti_viif_hw_on(viif_dev);

	/* VSYNC mask setting of MAIN unit */
	viif_main_vsync_set_irq_mask(viif_dev, MASK_INT_M_SYNC_MASK_SET);

	/* STATUS error mask setting of MAIN unit */
	viif_main_status_err_set_irq_mask(viif_dev, MASK_INT_M_DELAY_INT_ERROR);

	/* VSYNC mask settings of SUB unit */
	viif_sub_vsync_set_irq_mask(viif_dev, MASK_INT_S_SYNC_MASK_SET);

	/* STATUS error mask setting(unmask) of SUB unit */
	viif_sub_status_err_set_irq_mask(viif_dev,
					 MASK_INT_S_RESERVED_SET | MASK_INT_S_DELAY_INT_ERROR);

	return 0;
}

static const struct dev_pm_ops visconti_viif_pm_ops = {
	SET_RUNTIME_PM_OPS(visconti_viif_runtime_suspend, visconti_viif_runtime_resume, NULL)
};

static struct platform_driver visconti_viif_driver = {
	.probe = visconti_viif_probe,
	.remove = visconti_viif_remove,
	.driver = {
			.name = "visconti_viif",
			.of_match_table = visconti_viif_of_table,
			.pm = &visconti_viif_pm_ops,
		},
};

module_platform_driver(visconti_viif_driver);

MODULE_AUTHOR("Yuji Ishikawa <yuji2.ishikawa@toshiba.co.jp>");
MODULE_DESCRIPTION("Toshiba Visconti Video Input driver");
MODULE_LICENSE("Dual BSD/GPL");
