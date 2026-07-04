// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/workqueue.h>
#include <rdma/ib_verbs.h>

#include "zrdma_main.h"

MODULE_ALIAS("zrdma");
MODULE_AUTHOR("Yanze Zhang <zhang.yanze@zte.com.cn>");
MODULE_DESCRIPTION("ZTE Ethernet Protocol Driver for RDMA");
MODULE_LICENSE("Dual BSD/GPL");

LIST_HEAD(zxdh_handlers);
DEFINE_SPINLOCK(zxdh_handler_lock);

static void zxdh_add_handler(struct zxdh_handler *hdl)
{
	unsigned long flags;

	spin_lock_irqsave(&zxdh_handler_lock, flags);
	list_add(&hdl->list, &zxdh_handlers);
	spin_unlock_irqrestore(&zxdh_handler_lock, flags);
}

static void zxdh_del_handler(struct zxdh_handler *hdl)
{
	unsigned long flags;

	spin_lock_irqsave(&zxdh_handler_lock, flags);
	list_del(&hdl->list);
	spin_unlock_irqrestore(&zxdh_handler_lock, flags);
}

static void zxdh_fill_device_info(struct zxdh_device *zdev,
				  struct zxdh_core_dev_info *zdev_info)
{
	struct zxdh_pci_f *rf = zdev->rf;

	rf->ftype = ZXDH_FUNC_TYPE(zdev_info->vport_id);
	rf->pf_id = ZXDH_PF_ID(zdev_info->vport_id);
	rf->sc_dev.ep_id = ZXDH_EP_ID(zdev_info->vport_id);
	rf->ep_id = rf->sc_dev.ep_id;
	rf->sc_dev.driver_load = true;
	rf->zdev_info = zdev_info;
	rf->pcidev = zdev_info->pdev;
	rf->hw.pci_hw_addr = zdev_info->hw_addr;
	rf->zdev = zdev;

	INIT_LIST_HEAD(&zdev->ah_list);
	mutex_init(&zdev->ah_list_lock);
	zdev->netdev = zdev_info->netdev;
	zdev->source_netdev = zdev_info->netdev;
	zdev->init_state = INITIAL_STATE;
}

static int zxdh_probe(struct auxiliary_device *aux_dev,
		      const struct auxiliary_device_id *id)
{
	struct zxdh_auxiliary_dev *zxdh_adev =
		container_of(aux_dev, struct zxdh_auxiliary_dev, adev);
	struct zxdh_handler *hdl;
	struct zxdh_device *zdev;
	struct zxdh_pci_f *rf;
	int err;

	zdev = ib_alloc_device(zxdh_device, ibdev);
	if (!zdev)
		return -ENOMEM;

	zdev->zxdh_adev = zxdh_adev;

	zdev->rf = kzalloc_obj(*zdev->rf);
	if (!zdev->rf) {
		ib_dealloc_device(&zdev->ibdev);
		return -ENOMEM;
	}

	zxdh_fill_device_info(zdev, zxdh_adev->zxdh_info);

	hdl = kzalloc_obj(*hdl);
	if (!hdl) {
		kfree(zdev->rf);
		ib_dealloc_device(&zdev->ibdev);
		return -ENOMEM;
	}

	hdl->zdev = zdev;
	zdev->hdl = hdl;
	zdev->netdev_speed = SPEED_UNKNOWN;

	rf = zdev->rf;
	err = zxdh_ctrl_init_hw(rf);
	if (err)
		goto err_ctrl_init;

	zxdh_add_handler(hdl);

	dev_set_drvdata(&aux_dev->dev, zdev);

	return 0;

err_ctrl_init:
	kfree(hdl);
	kfree(zdev->rf);
	ib_dealloc_device(&zdev->ibdev);

	return err;
}

static void zxdh_remove(struct auxiliary_device *aux_dev)
{
	struct zxdh_device *zdev = dev_get_drvdata(&aux_dev->dev);

	if (!zdev) {
		dev_err(&aux_dev->dev, "%s: zdev is NULL\n", __func__);
		return;
	}

	zxdh_del_handler(zdev->hdl);
	kfree(zdev->hdl);
	kfree(zdev->rf);
	ib_dealloc_device(&zdev->ibdev);
}

static void zxdh_shutdown(struct auxiliary_device *aux_dev)
{
	zxdh_remove(aux_dev);
}

static const struct auxiliary_device_id zxdh_auxiliary_id_table[] = {
	{
		.name = ZXDH_PF_NAME "." ZXDH_RDMA_DEV_NAME,
	},
	{},
};

MODULE_DEVICE_TABLE(auxiliary, zxdh_auxiliary_id_table);

static struct auxiliary_driver zxdh_auxiliary_drv = {
	.driver = {
		.name = ZXDH_RDMA_DEV_NAME,
	},
	.id_table = zxdh_auxiliary_id_table,
	.probe = zxdh_probe,
	.remove = zxdh_remove,
	.shutdown = zxdh_shutdown,
};

module_auxiliary_driver(zxdh_auxiliary_drv);
