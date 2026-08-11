// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#include <linux/module.h>
#include <linux/auxiliary_bus.h>

#define ZXDH_PF_NAME "dinghai10e"
#define ZXDH_RDMA_DEV_NAME "rdma_aux"

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Yanze Zhang <zhang.yanze@zte.com.cn>");
MODULE_DESCRIPTION("ZTE Ethernet Protocol Driver for RDMA");

static const struct auxiliary_device_id zxdh_auxiliary_id_table[] = {
	{
		.name = ZXDH_PF_NAME "." ZXDH_RDMA_DEV_NAME,
	},
	{},
};

MODULE_DEVICE_TABLE(auxiliary, zxdh_auxiliary_id_table);

static int zxdh_probe(struct auxiliary_device *aux_dev,
		      const struct auxiliary_device_id *id)
{
	/* Placeholder: Real probe logic will be added in subsequent patches */
	dev_info(&aux_dev->dev, "ZTE DingHai RDMA device detected (stub)\n");
	return 0;
}

static void zxdh_remove(struct auxiliary_device *aux_dev)
{
	dev_info(&aux_dev->dev, "ZTE DingHai RDMA device removed (stub)\n");
}

static struct auxiliary_driver zxdh_auxiliary_drv = {
	.driver = {
		.name = ZXDH_RDMA_DEV_NAME,
	},
	.id_table = zxdh_auxiliary_id_table,
	.probe = zxdh_probe,
	.remove = zxdh_remove,
};

static int __init zrdma_module_init(void)
{
	return auxiliary_driver_register(&zxdh_auxiliary_drv);
}

static void __exit zrdma_module_exit(void)
{
	auxiliary_driver_unregister(&zxdh_auxiliary_drv);
}

module_init(zrdma_module_init);
module_exit(zrdma_module_exit);
