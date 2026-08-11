// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#include "zrdma_main.h"
#include "zrdma_verbs.h"
#include "zrdma_ctrl.h"

static const struct ib_device_ops zxdh_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_ZRDMA,
	.uverbs_abi_ver = ZXDH_ABI_VER,
};

static int zxdh_init_rdma_device(struct zxdh_device *zdev)
{
	zdev->ibdev.phys_port_cnt = 1;
	zdev->ibdev.num_comp_vectors = zdev->rf->ceqs_count;
	zdev->ibdev.dev.parent = &zdev->rf->pcidev->dev;

	ib_set_device_ops(&zdev->ibdev, &zxdh_dev_ops);

	return 0;
}

int zxdh_ib_register_device(struct zxdh_device *zdev)
{
	int ret;

	ret = zxdh_init_rdma_device(zdev);
	if (ret)
		return ret;

	ret = ib_register_device(&zdev->ibdev, "zrdma%d", zdev->rf->hw.device);
	if (ret)
		pr_err("zrdma: Register RDMA device fail\n");

	return ret;
}
