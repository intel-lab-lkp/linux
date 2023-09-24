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
#include <linux/smc.h>
#include <net/smc.h>

#include "smc_ism.h"
#include "smc_loopback.h"

#if IS_ENABLED(CONFIG_SMC_LO)
#define SMC_LO_SUPPORTS_V2	0x1 /* SMC-D loopback supports SMC-Dv2 */

static const char smc_lo_dev_name[] = "smc_lo";
static struct smcd_seid SMC_LO_SEID = {
	.seid_string = "IBM-SYSZ-ISMSEID00000000",
	.serial_number = "0000",
	.type = "0000",
};

static struct smc_lo_dev *lo_dev;

static void smc_lo_create_seid(struct smcd_seid *seid)
{
#if IS_ENABLED(CONFIG_S390)
	struct cpuid id;
	u16 ident_tail;
	char tmp[5];

	get_cpu_id(&id);
	ident_tail = (u16)(id.ident & S390_ISM_IDENT_MASK);
	snprintf(tmp, 5, "%04X", ident_tail);
	memcpy(&seid->serial_number, tmp, 4);
	snprintf(tmp, 5, "%04X", id.machine);
	memcpy(&seid->type, tmp, 4);
#else
	memset(seid, 0, SMC_MAX_EID_LEN);
#endif
}

static void smc_lo_generate_id(struct smc_lo_dev *ldev)
{
	struct smcd_gid *lgid = &ldev->local_gid;
	uuid_t uuid;

	uuid_gen(&uuid);
	memcpy(&lgid->gid, &uuid, sizeof(lgid->gid));
	memcpy(&lgid->gid_ext, (u8 *)&uuid + sizeof(lgid->gid),
	       sizeof(lgid->gid_ext));

	ldev->chid = SMC_LO_CHID;
	smc_lo_create_seid(&SMC_LO_SEID);
}

static int smc_lo_query_rgid(struct smcd_dev *smcd, struct smcd_gid *rgid,
			     u32 vid_valid, u32 vid)
{
	struct smc_lo_dev *ldev = smcd->priv;

	/* rgid should equal to lgid in loopback */
	if (!ldev || rgid->gid != ldev->local_gid.gid ||
	    rgid->gid_ext != ldev->local_gid.gid_ext)
		return -ENETUNREACH;
	return 0;
}

static int smc_lo_add_vlan_id(struct smcd_dev *smcd, u64 vlan_id)
{
	return -EOPNOTSUPP;
}

static int smc_lo_del_vlan_id(struct smcd_dev *smcd, u64 vlan_id)
{
	return -EOPNOTSUPP;
}

static int smc_lo_set_vlan_required(struct smcd_dev *smcd)
{
	return -EOPNOTSUPP;
}

static int smc_lo_reset_vlan_required(struct smcd_dev *smcd)
{
	return -EOPNOTSUPP;
}

static int smc_lo_signal_event(struct smcd_dev *dev, struct smcd_gid *rgid,
			       u32 trigger_irq, u32 event_code, u64 info)
{
	return 0;
}

static int smc_lo_supports_v2(void)
{
	return SMC_LO_SUPPORTS_V2;
}

static u8 *smc_lo_get_system_eid(void)
{
	return SMC_LO_SEID.seid_string;
}

static void smc_lo_get_local_gid(struct smcd_dev *smcd,
				 struct smcd_gid *smcd_gid)
{
	struct smc_lo_dev *ldev = smcd->priv;

	smcd_gid->gid = ldev->local_gid.gid;
	smcd_gid->gid_ext = ldev->local_gid.gid_ext;
}

static u16 smc_lo_get_chid(struct smcd_dev *smcd)
{
	return ((struct smc_lo_dev *)smcd->priv)->chid;
}

static struct device *smc_lo_get_dev(struct smcd_dev *smcd)
{
	return &((struct smc_lo_dev *)smcd->priv)->dev;
}

static const struct smcd_ops lo_ops = {
	.query_remote_gid = smc_lo_query_rgid,
	.register_dmb		= NULL,
	.unregister_dmb		= NULL,
	.add_vlan_id = smc_lo_add_vlan_id,
	.del_vlan_id = smc_lo_del_vlan_id,
	.set_vlan_required = smc_lo_set_vlan_required,
	.reset_vlan_required = smc_lo_reset_vlan_required,
	.signal_event = smc_lo_signal_event,
	.move_data		= NULL,
	.supports_v2 = smc_lo_supports_v2,
	.get_system_eid = smc_lo_get_system_eid,
	.get_local_gid = smc_lo_get_local_gid,
	.get_chid = smc_lo_get_chid,
	.get_dev = smc_lo_get_dev,
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
	smc_lo_generate_id(ldev);

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
