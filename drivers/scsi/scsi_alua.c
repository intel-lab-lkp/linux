// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic SCSI-3 ALUA SCSI driver
 *
 * Copyright (C) 2007-2010 Hannes Reinecke, SUSE Linux Products GmbH.
 * All rights reserved.
 */

#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_alua.h>

#define DRV_NAME "alua"

static struct workqueue_struct *kalua_wq;

int scsi_alua_sdev_init(struct scsi_device *sdev)
{
	int rel_port, ret, tpgs;

	tpgs = scsi_device_tpgs(sdev);
	if (!tpgs)
		return 0;

	sdev->alua = kzalloc(sizeof(*sdev->alua), GFP_KERNEL);
	if (!sdev->alua)
		return -ENOMEM;

	sdev->alua->group_id = scsi_vpd_tpg_id(sdev, &rel_port);
	sdev_printk(KERN_INFO, sdev,
			    "%s: group_id=%d\n",
			    DRV_NAME, sdev->alua->group_id);
	if (sdev->alua->group_id < 0) {
		/*
		 * Internal error; TPGS supported but required
		 * VPD identification descriptors not present.
		 * Disable ALUA support.
		 */
		sdev_printk(KERN_INFO, sdev,
			    "%s: No target port descriptors found\n",
			    __func__);
		ret = -EIO;
		goto out_free_data;
	}

	sdev->alua->sdev = sdev;
	sdev->alua->tpgs = tpgs;

	return 0;
out_free_data:
	kfree(sdev->alua);
	sdev->alua = NULL;
	return ret;
}

void scsi_alua_sdev_exit(struct scsi_device *sdev)
{
	kfree(sdev->alua);
	sdev->alua = NULL;
}

int scsi_alua_init(void)
{
	kalua_wq = alloc_workqueue("kalua", WQ_MEM_RECLAIM | WQ_PERCPU, 0);
	if (!kalua_wq)
		return -ENOMEM;
	return 0;
}

void scsi_exit_alua(void)
{
	destroy_workqueue(kalua_wq);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_alua");
