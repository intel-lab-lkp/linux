// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cadence Display Port Interface (DP) driver
 *
 * Copyright (C) 2023-2026 NXP Semiconductor, Inc.
 *
 */
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/irq.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>

#include "cdns-mhdp8501-core.h"

static int firmware_version_show(struct seq_file *s, void *data)
{
	struct cdns_mhdp8501_device *mhdp = s->private;

	u32 version = readl(mhdp->base.regs + VER_L) | readl(mhdp->base.regs + VER_H) << 8;
	u32 lib_version = readl(mhdp->base.regs + VER_LIB_L_ADDR) |
			  readl(mhdp->base.regs + VER_LIB_H_ADDR) << 8;

	seq_printf(s, "FW version %d, Lib version %d\n", version, lib_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static void cdns_mhdp8501_debugfs_init(struct cdns_mhdp8501_device *mhdp)
{
	mhdp->debugfs = debugfs_create_dir(dev_name(mhdp->dev), NULL);

	debugfs_create_file("firmware_version", 0444, mhdp->debugfs,
			    mhdp, &firmware_version_fops);
}

static void cdns_mhdp8501_debugfs_cleanup(struct cdns_mhdp8501_device *mhdp)
{
	debugfs_remove_recursive(mhdp->debugfs);
}

static int cdns_mhdp8501_read_hpd(struct cdns_mhdp8501_device *mhdp)
{
	u8 status = 0xff;
	int ret;

	ret = cdns_mhdp_mailbox_send_recv(&mhdp->base, MB_MODULE_ID_GENERAL,
					  GENERAL_GET_HPD_STATE,
					  0, NULL, sizeof(status), &status);
	if (ret) {
		dev_err(mhdp->dev, "read hpd failed: %d\n", ret);
		return ret;
	}

	return status;
}

enum drm_connector_status cdns_mhdp8501_detect(struct drm_bridge *bridge,
					       struct drm_connector *connector)
{
	struct cdns_mhdp8501_device *mhdp = bridge_to_mhdp(bridge);
	int hpd;

	hpd = cdns_mhdp8501_read_hpd(mhdp);

	if (hpd == 1)
		return connector_status_connected;
	else if (hpd == 0)
		return connector_status_disconnected;

	return connector_status_unknown;
}

enum drm_mode_status
cdns_mhdp8501_mode_valid(struct drm_bridge *bridge,
			 const struct drm_display_info *info,
			 const struct drm_display_mode *mode)
{
	/* We don't support double-clocked */
	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		return MODE_BAD;

	/* MAX support pixel clock rate 594MHz */
	if (mode->clock > 594000)
		return MODE_CLOCK_HIGH;

	if (mode->hdisplay > 3840)
		return MODE_BAD_HVALUE;

	if (mode->vdisplay > 2160)
		return MODE_BAD_VVALUE;

	return MODE_OK;
}

static void hotplug_work_func(struct work_struct *work)
{
	struct cdns_mhdp8501_device *mhdp = container_of(work,
						     struct cdns_mhdp8501_device,
						     hotplug_work.work);
	enum drm_connector_status status = cdns_mhdp8501_detect(&mhdp->bridge,
								NULL);

	/*
	 * iMX8MQ has two HPD interrupts: one for plugout and one for plugin.
	 * These interrupts cannot be masked and cleaned, so we must enable one
	 * and disable the other to avoid continuous interrupt generation.
	 */
	if (status == connector_status_connected) {
		/* Cable connected  */
		dev_dbg(mhdp->dev, "HDMI/DP Cable Plug In\n");
		drm_bridge_hpd_notify(&mhdp->bridge, status);
		if (!READ_ONCE(mhdp->removing))
			enable_irq(mhdp->irq[IRQ_OUT]);

		/* Reset HDMI/DP link with sink */
		if (mhdp->bridge_type == DRM_MODE_CONNECTOR_HDMIA)
			cdns_hdmi_handle_hotplug(mhdp);
		else
			cdns_dp_check_link_state(mhdp);
	} else if (status == connector_status_disconnected) {
		/* Cable Disconnected  */
		dev_dbg(mhdp->dev, "HDMI/DP Cable Plug Out\n");
		drm_bridge_hpd_notify(&mhdp->bridge, status);
		if (!READ_ONCE(mhdp->removing))
			enable_irq(mhdp->irq[IRQ_IN]);
	} else {
		/* HPD state read failed, retry to avoid losing HPD */
		dev_warn(mhdp->dev, "failed to read HPD state, retrying\n");
		mod_delayed_work(system_wq, &mhdp->hotplug_work,
				 msecs_to_jiffies(HOTPLUG_DEBOUNCE_MS));
	}
}

static irqreturn_t cdns_mhdp8501_irq_thread(int irq, void *data)
{
	struct cdns_mhdp8501_device *mhdp = data;

	disable_irq_nosync(irq);

	mod_delayed_work(system_wq, &mhdp->hotplug_work,
			 msecs_to_jiffies(HOTPLUG_DEBOUNCE_MS));

	return IRQ_HANDLED;
}

#define DATA_LANES_COUNT	4
static int cdns_mhdp8501_dt_parse(struct platform_device *pdev,
				  u32 *lane_mapping)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	u32 data_lanes[DATA_LANES_COUNT];
	struct device_node *endpoint;
	int ret, i;

	endpoint = of_graph_get_endpoint_by_regs(np, 1, -1);
	if (!endpoint) {
		dev_err(dev, "missing port@1 endpoint\n");
		return -ENODEV;
	}

	ret = drm_of_get_data_lanes_count(endpoint, DATA_LANES_COUNT,
					  DATA_LANES_COUNT);
	if (ret < 0) {
		dev_err(dev, "expected 4 data lanes\n");
		of_node_put(endpoint);
		return ret;
	}

	ret = of_property_read_u32_array(endpoint, "data-lanes",
					 data_lanes, DATA_LANES_COUNT);
	of_node_put(endpoint);
	if (ret)
		return ret;

	*lane_mapping = 0;
	for (i = 0; i < DATA_LANES_COUNT; i++) {
		if (data_lanes[i] > 3) {
			dev_err(dev, "invalid lane index %u at position %d\n",
				data_lanes[i], i);
			return -EINVAL;
		}
		*lane_mapping |= data_lanes[i] << (i * 2);
	}

	return 0;
}

static int cdns_mhdp8501_add_bridge(struct cdns_mhdp8501_device *mhdp)
{
	mhdp->bridge.type = mhdp->bridge_type;
	mhdp->bridge.of_node = mhdp->dev->of_node;
	mhdp->bridge.vendor = "NXP";
	mhdp->bridge.product = "i.MX8";
	mhdp->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID |
			   DRM_BRIDGE_OP_HPD;

	if (mhdp->bridge_type == DRM_MODE_CONNECTOR_HDMIA) {
		mhdp->bridge.ops |= DRM_BRIDGE_OP_HDMI;
		mhdp->bridge.ddc = cdns_hdmi_i2c_adapter(mhdp);
		if (IS_ERR(mhdp->bridge.ddc))
			return PTR_ERR(mhdp->bridge.ddc);
	}

	drm_bridge_add(&mhdp->bridge);

	return 0;
}

static int cdns_mhdp8501_probe(struct platform_device *pdev)
{
	const struct drm_bridge_funcs *bridge_funcs;
	struct cdns_mhdp8501_device *mhdp;
	struct device *dev = &pdev->dev;
	struct device_node *remote;
	enum phy_mode phy_mode;
	struct resource *res;
	u32 lane_mapping;
	int bridge_type;
	u32 reg;
	int ret;

	bridge_type = (int)(uintptr_t)of_device_get_match_data(dev);

	ret = cdns_mhdp8501_dt_parse(pdev, &lane_mapping);
	if (ret < 0)
		return ret;

	ret = devm_of_platform_populate(dev);
	if (ret)
		return ret;

	bridge_funcs = (bridge_type == DRM_MODE_CONNECTOR_HDMIA) ?
			&cdns_hdmi_bridge_funcs : &cdns_dp_bridge_funcs;

	mhdp = devm_drm_bridge_alloc(dev, struct cdns_mhdp8501_device,
				     bridge, bridge_funcs);
	if (!mhdp)
		return -ENOMEM;

	mhdp->dev = dev;
	mhdp->bridge_type = bridge_type;
	mhdp->lane_mapping = lane_mapping;

	remote = of_graph_get_remote_node(dev->of_node, 1, 0);
	if (!remote)
		return dev_err_probe(dev, -ENODEV,
				     "failed to find remote bridge node\n");

	mhdp->bridge.next_bridge = of_drm_find_and_get_bridge(remote);
	of_node_put(remote);
	if (!mhdp->bridge.next_bridge)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "failed to get next bridge\n");

	INIT_DELAYED_WORK(&mhdp->hotplug_work, hotplug_work_func);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	mhdp->regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!mhdp->regs)
		return -ENOMEM;

	/* init base struct for access mhdp mailbox */
	mhdp->base.dev = mhdp->dev;
	mhdp->base.regs = mhdp->regs;
	mutex_init(&mhdp->base.mailbox_mutex);
	mutex_init(&mhdp->link_mutex);

	/*
	 * Store &mhdp->base as drvdata so child devices (e.g. the PHY) can
	 * retrieve the shared cdns_mhdp_base, including the mailbox_mutex.
	 */
	dev_set_drvdata(dev, &mhdp->base);

	mhdp->phy = devm_of_phy_get_by_index(dev, pdev->dev.of_node, 0);
	if (IS_ERR(mhdp->phy)) {
		ret = dev_err_probe(dev, PTR_ERR(mhdp->phy), "no PHY configured\n");
		goto err_mutex;
	}

	mhdp->irq[IRQ_IN] = platform_get_irq_byname(pdev, "plug_in");
	if (mhdp->irq[IRQ_IN] < 0) {
		ret = dev_err_probe(dev, mhdp->irq[IRQ_IN], "No plug_in irq number\n");
		goto err_mutex;
	}

	mhdp->irq[IRQ_OUT] = platform_get_irq_byname(pdev, "plug_out");
	if (mhdp->irq[IRQ_OUT] < 0) {
		ret = dev_err_probe(dev, mhdp->irq[IRQ_OUT], "No plug_out irq number\n");
		goto err_mutex;
	}

	irq_set_status_flags(mhdp->irq[IRQ_IN], IRQ_NOAUTOEN);
	ret = devm_request_threaded_irq(dev, mhdp->irq[IRQ_IN],
					NULL, cdns_mhdp8501_irq_thread,
					IRQF_ONESHOT, dev_name(dev), mhdp);
	if (ret < 0) {
		dev_err(dev, "can't claim irq %d\n", mhdp->irq[IRQ_IN]);
		ret = -EINVAL;
		goto err_mutex;
	}

	irq_set_status_flags(mhdp->irq[IRQ_OUT], IRQ_NOAUTOEN);
	ret = devm_request_threaded_irq(dev, mhdp->irq[IRQ_OUT],
					NULL, cdns_mhdp8501_irq_thread,
					IRQF_ONESHOT, dev_name(dev), mhdp);
	if (ret < 0) {
		dev_err(dev, "can't claim irq %d\n", mhdp->irq[IRQ_OUT]);
		ret = -EINVAL;
		goto err_mutex;
	}

	if (mhdp->bridge_type == DRM_MODE_CONNECTOR_DisplayPort)
		phy_mode = PHY_MODE_DP;
	else if (mhdp->bridge_type == DRM_MODE_CONNECTOR_HDMIA)
		phy_mode = PHY_MODE_HDMI;

	if (mhdp->bridge_type == DRM_MODE_CONNECTOR_DisplayPort) {
		drm_dp_aux_init(&mhdp->dp.aux);
		mhdp->dp.aux.name = "mhdp8501_dp_aux";
		mhdp->dp.aux.dev = dev;
		mhdp->dp.aux.transfer = cdns_dp_aux_transfer;
	}

	/* Enable APB clock */
	mhdp->apb_clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(mhdp->apb_clk)) {
		ret = dev_err_probe(dev, PTR_ERR(mhdp->apb_clk),
				    "couldn't get apb clk\n");
		goto err_mutex;
	}

	/*
	 * Wait for the KEEP_ALIVE "message" on the first 8 bits.
	 * Updated each sched "tick" (~2ms)
	 */
	ret = readl_poll_timeout(mhdp->regs + KEEP_ALIVE, reg,
				 reg & CDNS_KEEP_ALIVE_MASK, 500,
				 CDNS_KEEP_ALIVE_TIMEOUT);
	if (ret) {
		dev_err(dev, "device didn't give any life sign: reg %d\n", reg);
		goto err_mutex;
	}

	/*
	 * Create debugfs only after the APB clock is on and the firmware is
	 * confirmed running.  A concurrent read of firmware_version before
	 * this point would access clock-gated registers and cause a bus fault.
	 */
	cdns_mhdp8501_debugfs_init(mhdp);

	ret = phy_init(mhdp->phy);
	if (ret) {
		dev_err(dev, "Failed to initialize PHY: %d\n", ret);
		goto err_debugfs;
	}

	ret = phy_set_mode(mhdp->phy, phy_mode);
	if (ret) {
		dev_err(dev, "Failed to configure PHY: %d\n", ret);
		goto err_phy;
	}

	ret = cdns_mhdp8501_add_bridge(mhdp);
	if (ret)
		goto err_phy;

	/* Enable cable hotplug detect */
	ret = cdns_mhdp8501_read_hpd(mhdp);
	if (ret < 0)
		goto err_bridge;

	if (ret == 1)
		enable_irq(mhdp->irq[IRQ_OUT]);
	else
		enable_irq(mhdp->irq[IRQ_IN]);

	return 0;

err_bridge:
	drm_bridge_remove(&mhdp->bridge);
err_phy:
	phy_exit(mhdp->phy);
err_debugfs:
	cdns_mhdp8501_debugfs_cleanup(mhdp);
err_mutex:
	mutex_destroy(&mhdp->link_mutex);
	mutex_destroy(&mhdp->base.mailbox_mutex);
	return ret;
}

static void cdns_mhdp8501_remove(struct platform_device *pdev)
{
	struct cdns_mhdp_base *base = platform_get_drvdata(pdev);
	struct cdns_mhdp8501_device *mhdp =
		container_of(base, struct cdns_mhdp8501_device, base);

	WRITE_ONCE(mhdp->removing, true);

	disable_irq(mhdp->irq[IRQ_IN]);
	disable_irq(mhdp->irq[IRQ_OUT]);

	cancel_delayed_work_sync(&mhdp->hotplug_work);

	drm_bridge_remove(&mhdp->bridge);

	phy_exit(mhdp->phy);

	mutex_destroy(&mhdp->link_mutex);
	mutex_destroy(&mhdp->base.mailbox_mutex);

	cdns_mhdp8501_debugfs_cleanup(mhdp);
}

static const struct of_device_id cdns_mhdp8501_dt_ids[] = {
	{ .compatible = "fsl,imx8mq-mhdp8501-hdmi",
	  .data = (void *)DRM_MODE_CONNECTOR_HDMIA },
	{ .compatible = "fsl,imx8mq-mhdp8501-dp",
	  .data = (void *)DRM_MODE_CONNECTOR_DisplayPort },
	{ },
};
MODULE_DEVICE_TABLE(of, cdns_mhdp8501_dt_ids);

static struct platform_driver cdns_mhdp8501_driver = {
	.probe = cdns_mhdp8501_probe,
	.remove = cdns_mhdp8501_remove,
	.driver = {
		.name = "cdns-mhdp8501",
		.of_match_table = cdns_mhdp8501_dt_ids,
	},
};

module_platform_driver(cdns_mhdp8501_driver);

MODULE_AUTHOR("Sandor Yu <sandor.yu@nxp.com>");
MODULE_DESCRIPTION("Cadence MHDP8501 bridge driver");
MODULE_LICENSE("GPL");
