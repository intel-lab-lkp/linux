// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
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

static struct mutex rdma_name_lock;
static struct list_head zxdh_rdma_list;

static void zxdh_add_handler(struct zxdh_handler *hdl)
{
	unsigned long flags;

	spin_lock_irqsave(&zxdh_handler_lock, flags);
	list_add(&hdl->list, &zxdh_handlers);
	spin_unlock_irqrestore(&zxdh_handler_lock, flags);
}

void zxdh_del_handler(struct zxdh_handler *hdl)
{
	unsigned long flags;

	spin_lock_irqsave(&zxdh_handler_lock, flags);
	list_del(&hdl->list);
	spin_unlock_irqrestore(&zxdh_handler_lock, flags);
}

int zxdh_get_del_rdma_name(const char *sbdf, char *name)
{
	struct zxdh_rdma_node *pos, *tmp;
	u8 len = 0;

	if (!sbdf || !name)
		return -EINVAL;

	if (strlen(name) > IB_DEVICE_NAME_MAX) {
		pr_err("zrdma: name len out of range\n");
		return -EINVAL;
	}

	mutex_lock(&rdma_name_lock);
	list_for_each_entry_safe(pos, tmp, &zxdh_rdma_list, list) {
		len = strlen(pos->sbdf);
		if (strlen(sbdf) == len) {
			if (strncasecmp(sbdf, pos->sbdf, len) == 0) {
				snprintf(name, IB_DEVICE_NAME_MAX, "%s",
					 pos->name);
				list_del(&pos->list);
				kfree(pos);
				mutex_unlock(&rdma_name_lock);
				return 0;
			}
		}
	}
	mutex_unlock(&rdma_name_lock);
	return -ENOENT;
}

static void zxdh_get_net_irq_cap(struct zxdh_pci_f *rf)
{
	u32 opcode = 0;

	if (rf->gen_ops.zxdh_common_func) {
		opcode = rf->gen_ops.zxdh_common_func(NULL, NULL,
						      ZXDH_FUNC_NUM_REQUIRE);
		if (opcode > ZXDH_FUNC_IRQ_FREE && opcode != 0xFF)
			rf->net_irq_cap = true;
	}
}

static void zxdh_get_common_func_num_max(struct zxdh_pci_f *rf)
{
	u32 num_max = 0;

	if (rf->gen_ops.zxdh_common_func) {
		num_max = rf->gen_ops.zxdh_common_func(NULL, NULL,
						       ZXDH_FUNC_NUM_REQUIRE);
		if (num_max != 0xFF) {
			rf->common_func_num_max = num_max;
			return;
		}
	}
	rf->common_func_num_max = -1;
}

static void zxdh_fill_device_info(struct zxdh_device *zdev,
				  struct zxdh_rdma_core_dev *zdev_info)
{
	struct zxdh_pci_f *rf = zdev->rf;

	rf->ftype = ZXDH_FUNC_TYPE(zdev_info->vport_id);
	rf->pf_id = ZXDH_PF_ID(zdev_info->vport_id);
	rf->sc_dev.ep_id = ZXDH_EP_ID(zdev_info->vport_id);
	rf->ep_id = rf->sc_dev.ep_id;
	rf->sc_dev.driver_load = true;

	rf->zdev_info = zdev_info;
	rf->pcidev = zdev_info->pdev;

	rf->msix_count = zdev_info->msix_count;
	rf->msix_entries = zdev_info->msix_entries;
	rf->sc_dev.max_ceqs = (rf->msix_count - 1);
	rf->rdma_ver = ZXDH_GEN_2;
	rf->rst_to = ZXDH_RST_TIMEOUT_HZ;
	rf->qp_index = 0;
	rf->gen_ops.zxdh_common_func = NULL;
	rf->net_irq_cap = false;
	rf->dh_dev = NULL;
	rf->drv_np_cap =
		(bool)FIELD_GET(ZXDH_RDMA_COMM_FUNC, zdev_info->ver.support);
	if (rf->drv_np_cap == ZXDH_RDMA_COMMON_FUNC_CAP) {
		if (zdev_info->ops && zdev_info->ops->zxdh_common_func)
			rf->gen_ops.zxdh_common_func =
				zdev_info->ops->zxdh_common_func;
	}

	zxdh_get_net_irq_cap(rf);
	zxdh_get_common_func_num_max(rf);

	rf->zdev = zdev;

	INIT_LIST_HEAD(&zdev->ah_list);
	mutex_init(&zdev->ah_list_lock);
	zdev->init_state = INITIAL_STATE;
	zdev->roce_mode = true;
}

static int zxdh_probe(struct auxiliary_device *aux_dev,
		      const struct auxiliary_device_id *id)
{
	struct zxdh_auxiliary_dev *zxdh_adev =
		container_of(aux_dev, struct zxdh_auxiliary_dev, adev);
	struct zxdh_handler *hdl;
	struct zxdh_device *zdev;
	struct zxdh_pci_f *rf;
	struct net_device *netdev;
	int err;

	if (!zxdh_adev->rdma_ops || !zxdh_adev->rdma_ops->get_rdma_netdev) {
		pr_err("zrdma: rdma_ops or get_rdma_netdev is NULL\n");
		return -EINVAL;
	}

	netdev = zxdh_adev->rdma_ops->get_rdma_netdev(zxdh_adev->parent);
	if (!netdev) {
		pr_err("zrdma: failed to get netdev\n");
		return -ENODEV;
	}

	zdev = ib_alloc_device(zxdh_device, ibdev);
	if (!zdev)
		return -ENOMEM;

	zdev->zxdh_adev = zxdh_adev;
	zdev->netdev = netdev;

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

	err = zxdh_ib_register_device(zdev);
	if (err)
		goto err_ib_reg;

	dev_set_drvdata(&aux_dev->dev, zdev);

	return 0;

err_ib_reg:
	zxdh_del_handler(hdl);
err_ctrl_init:
	kfree(zdev->rf);
	kfree(hdl);
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

	ib_unregister_device(&zdev->ibdev);
	zxdh_del_handler(zdev->hdl);

	kfree(zdev->rf);
	kfree(zdev->hdl);
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

static int __init zrdma_module_init(void)
{
	int ret;

	INIT_LIST_HEAD(&zxdh_rdma_list);
	mutex_init(&rdma_name_lock);

	ret = auxiliary_driver_register(&zxdh_auxiliary_drv);
	if (ret) {
		pr_err("zrdma: failed to register auxiliary driver\n");
		return ret;
	}

	return 0;
}

static void __exit zrdma_module_exit(void)
{
	auxiliary_driver_unregister(&zxdh_auxiliary_drv);
	mutex_destroy(&rdma_name_lock);
}

module_init(zrdma_module_init);
module_exit(zrdma_module_exit);
