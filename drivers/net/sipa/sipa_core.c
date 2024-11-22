/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Spreadtrum pin controller driver
 * Copyright (C) 2017 Spreadtrum  - http://www.spreadtrum.com
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_wakeup.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/sprd/sipa.h>
#include <linux/tick.h>
#include <uapi/linux/sched/types.h>
#include "sipa_priv.h"

#define DRV_NAME "sipa"

/**
 * SPRD IPA contains a number of common fifo
 * in the current Unisoc, mainly includes USB, WIFI, PCIE, AP etc.
 */
static struct sipa_cmn_fifo_info sipa_cmn_fifo_statics[SIPA_CFIFO_MAX] = {
	{
		.cfifo_name = "sprd,usb-ul",
		.tx_fifo = "sprd,usb-ul-tx",
		.rx_fifo = "sprd,usb-ul-rx",
		.relate_ep = SIPA_EP_USB,
		.src_id = SIPA_TERM_USB,
		.dst_id = SIPA_TERM_AP,
		.is_to_ipa = 1,
		.is_pam = 1,
	},
	{
		.cfifo_name = "sprd,wifi-ul",
		.tx_fifo = "sprd,wifi-ul-tx",
		.rx_fifo = "sprd,wifi-ul-rx",
		.relate_ep = SIPA_EP_WIFI,
		.src_id = SIPA_TERM_WIFI1,
		.dst_id = SIPA_TERM_AP,
		.is_to_ipa = 1,
		.is_pam = 1,
	},
	{
		.cfifo_name = "sprd,pcie-ul",
		.tx_fifo = "sprd,pcie-ul-tx",
		.rx_fifo = "sprd,pcie-ul-rx",
		.relate_ep = SIPA_EP_PCIE,
		.src_id = SIPA_TERM_PCIE0,
		.dst_id = SIPA_TERM_AP,
		.is_to_ipa = 1,
		.is_pam = 1,
	},
	{
		.cfifo_name = "sprd,wiap-dl",
		.tx_fifo = "sprd,wiap-dl-tx",
		.rx_fifo = "sprd,wiap-dl-rx",
		.relate_ep = SIPA_EP_WIAP,
		.src_id = SIPA_TERM_VAP0,
		.dst_id = SIPA_TERM_AP,
		.is_to_ipa = 1,
		.is_pam = 1,
	},
	{
		.cfifo_name = "sprd,map-in",
		.tx_fifo = "sprd,map-in-tx",
		.rx_fifo = "sprd,map-in-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_VCP,
		.is_to_ipa = 1,
		.is_pam = 0,
	},
	{
		.cfifo_name = "sprd,usb-dl",
		.tx_fifo = "sprd,usb-dl-tx",
		.rx_fifo = "sprd,usb-dl-rx",
		.relate_ep = SIPA_EP_USB,
		.src_id = SIPA_TERM_USB,
		.dst_id = SIPA_TERM_AP,
		.is_to_ipa = 0,
		.is_pam = 1,
	},
	{
		.cfifo_name = "sprd,wifi-dl",
		.tx_fifo = "sprd,wifi-dl-tx",
		.rx_fifo = "sprd,wifi-dl-rx",
		.relate_ep = SIPA_EP_WIFI,
		.src_id = SIPA_TERM_WIFI1,
		.dst_id = SIPA_TERM_AP,
		.is_to_ipa = 0,
		.is_pam = 1,
	},
	{
		.cfifo_name = "sprd,pcie-dl",
		.tx_fifo = "sprd,pcie-dl-tx",
		.rx_fifo = "sprd,pcie-dl-rx",
		.relate_ep = SIPA_EP_PCIE,
		.src_id = SIPA_TERM_PCIE0,
		.dst_id = SIPA_TERM_AP,
		.is_to_ipa = 0,
		.is_pam = 1,
	},
	{
		.cfifo_name = "sprd,wiap-ul",
		.tx_fifo = "sprd,wiap-ul-tx",
		.rx_fifo = "sprd,wiap-ul-rx",
		.relate_ep = SIPA_EP_WIAP,
		.src_id = SIPA_TERM_VAP0,
		.dst_id = SIPA_TERM_AP,
		.is_to_ipa = 0,
		.is_pam = 1,
	},
	{
		.cfifo_name = "sprd,map0-out",
		.tx_fifo = "sprd,map0-out-tx",
		.rx_fifo = "sprd,map0-out-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_USB,
		.is_to_ipa = 0,
		.is_pam = 0,
	},
	{
		.cfifo_name = "sprd,map1-out",
		.tx_fifo = "sprd,map1-out-tx",
		.rx_fifo = "sprd,map1-out-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_USB,
		.is_to_ipa = 0,
		.is_pam = 0,
	},
	{
		.cfifo_name = "sprd,map2-out",
		.tx_fifo = "sprd,map2-out-tx",
		.rx_fifo = "sprd,map2-out-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_USB,
		.is_to_ipa = 0,
		.is_pam = 0,
	},
	{
		.cfifo_name = "sprd,map3-out",
		.tx_fifo = "sprd,map3-out-tx",
		.rx_fifo = "sprd,map3-out-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_USB,
		.is_to_ipa = 0,
		.is_pam = 0,
	},
	{
		.cfifo_name = "sprd,map4-out",
		.tx_fifo = "sprd,map4-out-tx",
		.rx_fifo = "sprd,map4-out-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_USB,
		.is_to_ipa = 0,
		.is_pam = 0,
	},
	{
		.cfifo_name = "sprd,map5-out",
		.tx_fifo = "sprd,map5-out-tx",
		.rx_fifo = "sprd,map5-out-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_USB,
		.is_to_ipa = 0,
		.is_pam = 0,
	},
	{
		.cfifo_name = "sprd,map6-out",
		.tx_fifo = "sprd,map6-out-tx",
		.rx_fifo = "sprd,map6-out-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_USB,
		.is_to_ipa = 0,
		.is_pam = 0,
	},
	{
		.cfifo_name = "sprd,map7-out",
		.tx_fifo = "sprd,map7-out-tx",
		.rx_fifo = "sprd,map7-out-rx",
		.relate_ep = SIPA_EP_AP,
		.src_id = SIPA_TERM_AP,
		.dst_id = SIPA_TERM_USB,
		.is_to_ipa = 0,
		.is_pam = 0,
	},
};

static struct sipa_plat_drv_cfg *s_sipa_core;

/**
 * sipa_get_ctrl_pointer() - get the main structure of th sipa driver.
 */
struct sipa_plat_drv_cfg *sipa_get_ctrl_pointer(void)
{
	return s_sipa_core;
}
EXPORT_SYMBOL(sipa_get_ctrl_pointer);

static int sipa_create_ep_from_fifo_idx(struct device *dev,
					enum sipa_cmn_fifo_index fifo_idx)
{
	enum sipa_ep_id ep_id;
	struct sipa_common_fifo *fifo;
	struct sipa_endpoint *ep = NULL;
	struct sipa_cmn_fifo_info *fifo_info;
	struct sipa_plat_drv_cfg *ipa = dev_get_drvdata(dev);

	fifo_info = (struct sipa_cmn_fifo_info *)sipa_cmn_fifo_statics;
	ep_id = (fifo_info + fifo_idx)->relate_ep;

	ep = ipa->eps[ep_id];
	if (!ep) {
		ep = kzalloc(sizeof(*ep), GFP_KERNEL);
		if (!ep)
			return -ENOMEM;

		ipa->eps[ep_id] = ep;
	} else if (fifo_idx > SIPA_CFIFO_MAP0_OUT) {
		dev_info(dev, "ep %d has already create\n", ep->id);
		return 0;
	}

	ep->dev = dev;
	ep->id = (fifo_info + fifo_idx)->relate_ep;
	dev_info(dev, "idx = %d ep = %d ep_id = %d is_to_ipa = %d\n",
		 fifo_idx, ep->id, ep_id,
		 (fifo_info + fifo_idx)->is_to_ipa);

	ep->connected = false;
	ep->suspended = true;

	if (!(fifo_info + fifo_idx)->is_to_ipa) {
		fifo = &ep->recv_fifo;
		fifo->is_receiver = true;
	} else {
		fifo = &ep->send_fifo;
		fifo->is_receiver = false;
	}

	fifo->rx_fifo.fifo_depth = ipa->cmn_fifo_cfg[fifo_idx].rx_fifo.depth;
	fifo->tx_fifo.fifo_depth = ipa->cmn_fifo_cfg[fifo_idx].tx_fifo.depth;
	fifo->dst_id = (fifo_info + fifo_idx)->dst_id;
	fifo->src_id = (fifo_info + fifo_idx)->src_id;

	fifo->idx = fifo_idx;

	return 0;
}

static int sipa_create_eps(struct device *dev)
{
	int i, ret = 0;
	struct sipa_plat_drv_cfg *ipa = dev_get_drvdata(dev);

	dev_info(dev, "create eps start\n");
	for (i = 0; i < SIPA_CFIFO_MAX; i++) {
		if (ipa->cmn_fifo_cfg[i].tx_fifo.depth > 0) {
			ret = sipa_create_ep_from_fifo_idx(dev, i);
			if (ret) {
				dev_err(dev, "create eps fifo %d fail\n", i);
				return ret;
			}
		}
	}

	return 0;
}

static int sipa_init(struct device *dev)
{
	int ret;

	/* init sipa eps */
	ret = sipa_create_eps(dev);

	return ret;
}

static int sipa_plat_drv_probe(struct platform_device *pdev_p)
{
	int ret;
	struct device *dev = &pdev_p->dev;
	struct sipa_plat_drv_cfg *ipa;

	ipa = devm_kzalloc(dev, sizeof(*ipa), GFP_KERNEL);
	if (!ipa)
		return -ENOMEM;

	s_sipa_core = ipa;
	dev_set_drvdata(dev, ipa);

	ipa->dev = dev;
	if (dma_set_mask_and_coherent(dev, ipa->hw_data->dma_mask))
		dev_warn(dev, "no suitable DMA availabld\n");

	ret = sipa_init(dev);
	if (ret) {
		dev_err(dev, "init failed %d\n", ret);
		return ret;
	}

	ipa->udp_frag = false;
	ipa->udp_port = false;
	atomic_set(&ipa->udp_port_num, 0);

	return ret;
}

static const struct of_device_id sipa_plat_drv_match[] = {
	{ .compatible = "sprd,ipa-v3", .data = NULL},
	{}
};

static struct platform_driver sipa_plat_drv = {
	.probe = sipa_plat_drv_probe,
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = sipa_plat_drv_match,
	},
};
module_platform_driver(sipa_plat_drv);

MODULE_DESCRIPTION("Unisoc IPA HW device driver");
MODULE_AUTHOR("Catdeo Zhang<catdeo.zhang@unisoc.com>");
MODULE_LICENSE("GPL");
