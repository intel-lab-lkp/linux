// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#include <linux/module.h>
#include <linux/printk.h>
#include <net/addrconf.h>

#include "ionic_ibdev.h"

#define DRIVER_DESCRIPTION "AMD Pensando RoCE HCA driver"
#define DEVICE_DESCRIPTION "AMD Pensando RoCE HCA"

MODULE_AUTHOR("Allen Hubbe <allen.hubbe@amd.com>");
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("NET_IONIC");

static const struct auxiliary_device_id ionic_aux_id_table[] = {
	{ .name = "ionic.rdma", },
	{},
};

MODULE_DEVICE_TABLE(auxiliary, ionic_aux_id_table);

static void ionic_destroy_ibdev(struct ionic_ibdev *dev)
{
	ib_unregister_device(&dev->ibdev);
	ib_dealloc_device(&dev->ibdev);
}

static struct ionic_ibdev *ionic_create_ibdev(void *handle,
					      struct net_device *ndev)
{
	const union ionic_lif_identity *ident;
	int rc, lif_index, version;
	struct ib_device *ibdev;
	struct ionic_ibdev *dev;

	ident = ionic_api_get_identity(handle, &lif_index);
	version = ident->rdma.version;

	if (version < IONIC_MIN_RDMA_VERSION ||
	    version > IONIC_MAX_RDMA_VERSION) {
		netdev_err(ndev, FW_INFO "ionic_rdma: incompatible version, fw ver %u\n",
			   version);
		netdev_err(ndev, FW_INFO "ionic_rdma: Driver Min Version %u\n",
			   IONIC_MIN_RDMA_VERSION);
		netdev_err(ndev, FW_INFO "ionic_rdma: Driver Max Version %u\n",
			   IONIC_MAX_RDMA_VERSION);
		rc = -EINVAL;
		goto err_dev;
	}

	dev = ib_alloc_device(ionic_ibdev, ibdev);
	if (!dev) {
		rc = -ENOMEM;
		goto err_dev;
	}

	dev->hwdev = ndev->dev.parent;
	dev->ndev = ndev;
	dev->handle = handle;
	dev->lif_index = lif_index;
	dev->ident = ident;
	dev->rdma_version = ident->rdma.version;

	ibdev = &dev->ibdev;
	ibdev->dev.parent = dev->hwdev;

	strscpy(ibdev->name, "ionic_%d", IB_DEVICE_NAME_MAX);
	strscpy(ibdev->node_desc, DEVICE_DESCRIPTION, IB_DEVICE_NODE_DESC_MAX);

	ibdev->node_type = RDMA_NODE_IB_CA;
	ibdev->phys_port_cnt = 1;

	addrconf_ifid_eui48((u8 *)&ibdev->node_guid, ndev);

	rc = ib_register_device(ibdev, "ionic_%d", ibdev->dev.parent);
	if (rc)
		goto err_register;

	return dev;

err_register:
	ib_dealloc_device(&dev->ibdev);
err_dev:
	return ERR_PTR(rc);
}

static int ionic_aux_probe(struct auxiliary_device *adev,
			   const struct auxiliary_device_id *id)
{
	struct ionic_aux_dev *ionic_adev;
	struct net_device *ndev;
	struct ionic_ibdev *dev;

	ionic_adev = container_of(adev, struct ionic_aux_dev, adev);
	ndev = ionic_api_get_netdev_from_handle(ionic_adev->handle);
	if (IS_ERR(ndev))
		return dev_err_probe(&adev->dev, PTR_ERR(ndev),
				     "Failed to get netdevice\n");

	dev_put(ndev);

	dev = ionic_create_ibdev(ionic_adev->handle, ndev);
	if (IS_ERR(dev))
		return dev_err_probe(&adev->dev, PTR_ERR(dev),
				     "Failed to register ibdev\n");

	auxiliary_set_drvdata(adev, dev);
	ibdev_dbg(&dev->ibdev, "registered\n");

	return 0;
}

static void ionic_aux_remove(struct auxiliary_device *adev)
{
	struct ionic_ibdev *dev = auxiliary_get_drvdata(adev);

	dev_dbg(&adev->dev, "unregister ibdev\n");
	ionic_destroy_ibdev(dev);
	dev_dbg(&adev->dev, "unregistered\n");
}

static struct auxiliary_driver ionic_aux_r_driver = {
	.name = "rdma",
	.probe = ionic_aux_probe,
	.remove = ionic_aux_remove,
	.id_table = ionic_aux_id_table,
};

static int __init ionic_mod_init(void)
{
	int rc;

	rc = auxiliary_driver_register(&ionic_aux_r_driver);
	if (rc)
		goto err_aux;

	return 0;

err_aux:
	return rc;
}

static void __exit ionic_mod_exit(void)
{
	auxiliary_driver_unregister(&ionic_aux_r_driver);
}

module_init(ionic_mod_init);
module_exit(ionic_mod_exit);
