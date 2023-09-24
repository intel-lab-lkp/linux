// SPDX-License-Identifier: GPL-2.0
/*
 *  Shared Memory Communications Direct over loopback device.
 *
 *  Provide a SMC-D loopback dummy device.
 *
 *  Copyright (c) 2022, Alibaba Inc.
 *
 *  Author: Wen Gu <guwen@linux.alibaba.com>
 *          Tony Lu <tonylu@linux.alibaba.com>
 *
 */

#include <linux/device.h>
#include <linux/types.h>
#include <net/smc.h>

#include "smc_ism.h"
#include "smc_loopback.h"

#if IS_ENABLED(CONFIG_SMC_LO)
static const char smc_lo_dev_name[] = "smc_lo";
static struct smc_lo_dev *lo_dev;

static const struct smcd_ops lo_ops = {
	.query_remote_gid	= NULL,
	.register_dmb		= NULL,
	.unregister_dmb		= NULL,
	.add_vlan_id		= NULL,
	.del_vlan_id		= NULL,
	.set_vlan_required	= NULL,
	.reset_vlan_required	= NULL,
	.signal_event		= NULL,
	.move_data		= NULL,
	.supports_v2		= NULL,
	.get_system_eid		= NULL,
	.get_local_gid		= NULL,
	.get_chid		= NULL,
	.get_dev		= NULL,
};

static struct smcd_dev *smcd_lo_alloc_dev(const struct smcd_ops *ops,
					  int max_dmbs)
{
	struct smcd_dev *smcd;

	smcd = kzalloc(sizeof(*smcd), GFP_KERNEL);
	if (!smcd)
		return NULL;

	smcd->conn = kcalloc(max_dmbs, sizeof(struct smc_connection *),
			     GFP_KERNEL);
	if (!smcd->conn)
		goto out_smcd;

	smcd->ops = ops;

	spin_lock_init(&smcd->lock);
	spin_lock_init(&smcd->lgr_lock);
	INIT_LIST_HEAD(&smcd->vlan);
	INIT_LIST_HEAD(&smcd->lgr_list);
	init_waitqueue_head(&smcd->lgrs_deleted);
	return smcd;

out_smcd:
	kfree(smcd);
	return NULL;
}

static int smcd_lo_register_dev(struct smc_lo_dev *ldev)
{
	struct smcd_dev *smcd;

	smcd = smcd_lo_alloc_dev(&lo_ops, SMC_LODEV_MAX_DMBS);
	if (!smcd)
		return -ENOMEM;

	ldev->smcd = smcd;
	smcd->priv = ldev;

	/* TODO:
	 * register smc_lo to smcd_dev list.
	 */
	return 0;
}

static void smcd_lo_unregister_dev(struct smc_lo_dev *ldev)
{
	/* TODO:
	 * unregister smc_lo from smcd_dev list.
	 */
}

static void smc_lo_dev_release(struct device *dev)
{
	struct smc_lo_dev *ldev =
		container_of(dev, struct smc_lo_dev, dev);
	struct smcd_dev *smcd = ldev->smcd;

	kfree(smcd->conn);
	kfree(smcd);
	kfree(ldev);
}

static int smc_lo_dev_init(struct smc_lo_dev *ldev)
{
	return smcd_lo_register_dev(ldev);
}

static int smc_lo_dev_probe(void)
{
	struct smc_lo_dev *ldev;
	int ret;

	ldev = kzalloc(sizeof(*ldev), GFP_KERNEL);
	if (!ldev)
		return -ENOMEM;

	ldev->dev.parent = NULL;
	ldev->dev.release = smc_lo_dev_release;
	device_initialize(&ldev->dev);
	dev_set_name(&ldev->dev, smc_lo_dev_name);

	ret = smc_lo_dev_init(ldev);
	if (ret)
		goto free_dev;

	lo_dev = ldev; /* global loopback device */
	return 0;

free_dev:
	kfree(ldev);
	return ret;
}

static void smc_lo_dev_exit(struct smc_lo_dev *ldev)
{
	smcd_lo_unregister_dev(ldev);
}

static void smc_lo_dev_remove(void)
{
	if (!lo_dev)
		return;

	smc_lo_dev_exit(lo_dev);
	put_device(&lo_dev->dev); /* device_initialize in smc_lo_dev_probe */
}
#endif

int smc_loopback_init(void)
{
#if IS_ENABLED(CONFIG_SMC_LO)
	return smc_lo_dev_probe();
#else
	return 0;
#endif
}

void smc_loopback_exit(void)
{
#if IS_ENABLED(CONFIG_SMC_LO)
	smc_lo_dev_remove();
#endif
}
