// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/pci.h>
#include <linux/irq.h>
#include <net/addrconf.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_mad.h>

#include "ionic_ibdev.h"

#define DRIVER_DESCRIPTION "AMD Pensando RoCE HCA driver"
#define DEVICE_DESCRIPTION "AMD Pensando RoCE HCA"

MODULE_AUTHOR("Allen Hubbe <allen.hubbe@amd.com>");
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("NET_IONIC");

#define IONIC_VERSION(a, b) (((a) << 16) + ((b) << 8))

static const struct auxiliary_device_id ionic_aux_id_table[] = {
	{ .name = "ionic.rdma", },
	{},
};

MODULE_DEVICE_TABLE(auxiliary, ionic_aux_id_table);

void ionic_port_event(struct ionic_ibdev *dev, enum ib_event_type event)
{
	struct ib_event ev;

	ev.device = &dev->ibdev;
	ev.element.port_num = 1;
	ev.event = event;

	ib_dispatch_event(&ev);
}

static int ionic_query_device(struct ib_device *ibdev,
			      struct ib_device_attr *attr,
			      struct ib_udata *udata)
{
	struct ionic_ibdev *dev = to_ionic_ibdev(ibdev);

	addrconf_ifid_eui48((u8 *)&attr->sys_image_guid, dev->ndev);
	attr->max_mr_size =
		le32_to_cpu(dev->ident->rdma.npts_per_lif) * PAGE_SIZE / 2;
	attr->page_size_cap = dev->page_size_supported;

	attr->vendor_id = to_pci_dev(dev->hwdev)->vendor;
	attr->vendor_part_id = to_pci_dev(dev->hwdev)->device;

	attr->hw_ver = dev->info->asic_rev;
	attr->fw_ver = 0;
	attr->max_qp = dev->size_qpid;
	attr->max_qp_wr = IONIC_MAX_DEPTH;
	attr->device_cap_flags =
		IB_DEVICE_MEM_WINDOW |
		IB_DEVICE_MEM_MGT_EXTENSIONS |
		IB_DEVICE_MEM_WINDOW_TYPE_2B |
		0;
	attr->kernel_cap_flags = IBK_LOCAL_DMA_LKEY;
	attr->max_send_sge =
		min(ionic_v1_send_wqe_max_sge(dev->max_stride, 0, false),
		    IONIC_SPEC_HIGH);
	attr->max_recv_sge =
		min(ionic_v1_recv_wqe_max_sge(dev->max_stride, 0, false),
		    IONIC_SPEC_HIGH);
	attr->max_sge_rd = attr->max_send_sge;
	attr->max_cq = dev->inuse_cqid.inuse_size / dev->udma_count;
	attr->max_cqe = IONIC_MAX_CQ_DEPTH - IONIC_CQ_GRACE;
	attr->max_mr = dev->inuse_mrid.inuse_size;
	attr->max_pd = IONIC_MAX_PD;
	attr->max_qp_rd_atom = IONIC_MAX_RD_ATOM;
	attr->max_ee_rd_atom = 0;
	attr->max_res_rd_atom = IONIC_MAX_RD_ATOM;
	attr->max_qp_init_rd_atom = IONIC_MAX_RD_ATOM;
	attr->max_ee_init_rd_atom = 0;
	attr->atomic_cap = IB_ATOMIC_GLOB;
	attr->masked_atomic_cap = IB_ATOMIC_GLOB;
	attr->max_mw = dev->inuse_mrid.inuse_size;
	attr->max_mcast_grp = 0;
	attr->max_mcast_qp_attach = 0;
	attr->max_ah = dev->inuse_ahid.inuse_size;
	attr->max_fast_reg_page_list_len =
		le32_to_cpu(dev->ident->rdma.npts_per_lif) / 2;
	attr->max_pkeys = IONIC_PKEY_TBL_LEN;

	return 0;
}

static int ionic_query_port(struct ib_device *ibdev, u32 port,
			    struct ib_port_attr *attr)
{
	struct ionic_ibdev *dev = to_ionic_ibdev(ibdev);
	struct net_device *ndev = dev->ndev;

	if (port != 1)
		return -EINVAL;

	if (netif_running(ndev) && netif_carrier_ok(ndev)) {
		attr->state = IB_PORT_ACTIVE;
		attr->phys_state = IB_PORT_PHYS_STATE_LINK_UP;
	} else if (netif_running(ndev)) {
		attr->state = IB_PORT_DOWN;
		attr->phys_state = IB_PORT_PHYS_STATE_POLLING;
	} else {
		attr->state = IB_PORT_DOWN;
		attr->phys_state = IB_PORT_PHYS_STATE_DISABLED;
	}

	attr->max_mtu = iboe_get_mtu(ndev->max_mtu);
	attr->active_mtu = min(attr->max_mtu, iboe_get_mtu(ndev->mtu));
	attr->gid_tbl_len = IONIC_GID_TBL_LEN;
	attr->ip_gids = true;
	attr->port_cap_flags = 0;
	attr->max_msg_sz = 0x80000000;
	attr->pkey_tbl_len = IONIC_PKEY_TBL_LEN;
	attr->max_vl_num = 1;
	attr->subnet_prefix = 0xfe80000000000000ull;

	return ib_get_eth_speed(ibdev, port,
				&attr->active_speed,
				&attr->active_width);
}

static enum rdma_link_layer ionic_get_link_layer(struct ib_device *ibdev,
						 u32 port)
{
	return IB_LINK_LAYER_ETHERNET;
}

static struct net_device *ionic_get_netdev(struct ib_device *ibdev, u32 port)
{
	struct ionic_ibdev *dev = to_ionic_ibdev(ibdev);

	if (port != 1)
		return ERR_PTR(-EINVAL);

	dev_hold(dev->ndev);

	return dev->ndev;
}

static int ionic_query_pkey(struct ib_device *ibdev, u32 port, u16 index,
			    u16 *pkey)
{
	if (port != 1)
		return -EINVAL;

	if (index != 0)
		return -EINVAL;

	*pkey = IB_DEFAULT_PKEY_FULL;

	return 0;
}

static int ionic_modify_device(struct ib_device *ibdev, int mask,
			       struct ib_device_modify *attr)
{
	struct ionic_ibdev *dev = to_ionic_ibdev(ibdev);

	if (mask & ~IB_DEVICE_MODIFY_NODE_DESC)
		return -EOPNOTSUPP;

	if (mask & IB_DEVICE_MODIFY_NODE_DESC)
		memcpy(dev->ibdev.node_desc, attr->node_desc,
		       IB_DEVICE_NODE_DESC_MAX);

	return 0;
}

static int ionic_get_port_immutable(struct ib_device *ibdev, u32 port,
				    struct ib_port_immutable *attr)
{
	if (port != 1)
		return -EINVAL;

	attr->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;

	attr->pkey_tbl_len = IONIC_PKEY_TBL_LEN;
	attr->gid_tbl_len = IONIC_GID_TBL_LEN;
	attr->max_mad_size = IB_MGMT_MAD_SIZE;

	return 0;
}

static void ionic_get_dev_fw_str(struct ib_device *ibdev, char *str)
{
	struct ionic_ibdev *dev = to_ionic_ibdev(ibdev);

	strscpy(str, dev->info->fw_version, IB_FW_VERSION_NAME_MAX);
}

static const struct cpumask *ionic_get_vector_affinity(struct ib_device *ibdev,
						       int comp_vector)
{
	struct ionic_ibdev *dev = to_ionic_ibdev(ibdev);

	if (comp_vector < 0 || comp_vector >= dev->eq_count)
		return NULL;

	return irq_get_affinity_mask(dev->eq_vec[comp_vector]->irq);
}

static ssize_t hw_rev_show(struct device *device, struct device_attribute *attr,
			   char *buf)
{
	struct ionic_ibdev *dev =
		rdma_device_to_drv_device(device, struct ionic_ibdev, ibdev);

	return sysfs_emit(buf, "0x%x\n", dev->info->asic_rev);
}
static DEVICE_ATTR_RO(hw_rev);

static ssize_t hca_type_show(struct device *device,
			     struct device_attribute *attr, char *buf)
{
	struct ionic_ibdev *dev =
		rdma_device_to_drv_device(device, struct ionic_ibdev, ibdev);

	return sysfs_emit(buf, "%s\n", dev->ibdev.node_desc);
}
static DEVICE_ATTR_RO(hca_type);

static struct attribute *ionic_rdma_attributes[] = {
	&dev_attr_hw_rev.attr,
	&dev_attr_hca_type.attr,
	NULL
};

static const struct attribute_group ionic_rdma_attr_group = {
	.attrs = ionic_rdma_attributes,
};

static void ionic_disassociate_ucontext(struct ib_ucontext *ibcontext)
{
	/*
	 * Dummy define disassociate_ucontext so that it does not
	 * wait for user context before cleaning up hw resources.
	 */
}

static const struct ib_device_ops ionic_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_IONIC,
	.uverbs_abi_ver = IONIC_ABI_VERSION,
	.query_device = ionic_query_device,
	.query_port = ionic_query_port,
	.get_link_layer = ionic_get_link_layer,
	.get_netdev = ionic_get_netdev,
	.query_pkey = ionic_query_pkey,
	.modify_device = ionic_modify_device,

	.get_port_immutable = ionic_get_port_immutable,
	.get_dev_fw_str = ionic_get_dev_fw_str,
	.get_vector_affinity = ionic_get_vector_affinity,
	.device_group = &ionic_rdma_attr_group,
	.disassociate_ucontext = ionic_disassociate_ucontext,
};

static void ionic_destroy_ibdev(struct ionic_ibdev *dev)
{
	ionic_kill_rdma_admin(dev, false);
	ib_unregister_device(&dev->ibdev);
	ionic_stats_cleanup(dev);
	ionic_destroy_rdma_admin(dev);
	ionic_resid_destroy(&dev->inuse_qpid);
	ionic_resid_destroy(&dev->inuse_cqid);
	ionic_resid_destroy(&dev->inuse_mrid);
	ionic_resid_destroy(&dev->inuse_ahid);
	ionic_resid_destroy(&dev->inuse_pdid);
	xa_destroy(&dev->qp_tbl);
	xa_destroy(&dev->cq_tbl);
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
	dev->info = ionic_api_get_devinfo(handle);
	ionic_api_kernel_dbpage(handle, &dev->intr_ctrl, &dev->dbid,
				&dev->dbpage);

	dev->rdma_version = ident->rdma.version;
	dev->qp_opcodes = ident->rdma.qp_opcodes;
	dev->admin_opcodes = ident->rdma.admin_opcodes;

	if (IONIC_VERSION(ident->rdma.version, ident->rdma.minor_version) >=
		IONIC_VERSION(2, 1))
		dev->page_size_supported =
			cpu_to_le64(ident->rdma.page_size_cap);
	else
		dev->page_size_supported = IONIC_PAGE_SIZE_SUPPORTED;

	dev->aq_base = le32_to_cpu(ident->rdma.aq_qtype.qid_base);
	dev->cq_base = le32_to_cpu(ident->rdma.cq_qtype.qid_base);
	dev->eq_base = le32_to_cpu(ident->rdma.eq_qtype.qid_base);

	/*
	 * ionic_create_rdma_admin() may reduce aq_count or eq_count if
	 * it is unable to allocate all that were requested.
	 * aq_count is tunable; see ionic_aq_count
	 * eq_count is tunable; see ionic_eq_count
	 */
	dev->aq_count = le32_to_cpu(ident->rdma.aq_qtype.qid_count);
	dev->eq_count = le32_to_cpu(ident->rdma.eq_qtype.qid_count);

	dev->aq_qtype = ident->rdma.aq_qtype.qtype;
	dev->sq_qtype = ident->rdma.sq_qtype.qtype;
	dev->rq_qtype = ident->rdma.rq_qtype.qtype;
	dev->cq_qtype = ident->rdma.cq_qtype.qtype;
	dev->eq_qtype = ident->rdma.eq_qtype.qtype;

	dev->max_stride = ident->rdma.max_stride;
	dev->expdb_mask = ionic_api_get_expdb(dev->handle);
	if (dev->expdb_mask) {
		struct ionic_qtype_info *qti;

		qti = ionic_api_get_queue_identity(dev->handle,
						   IONIC_QTYPE_TXQ);
		dev->sq_expdb = !!(qti->features & IONIC_QIDENT_F_EXPDB);

		qti = ionic_api_get_queue_identity(dev->handle,
						   IONIC_QTYPE_RXQ);
		dev->rq_expdb = !!(qti->features & IONIC_QIDENT_F_EXPDB);
	}

	dev->udma_qgrp_shift = ident->rdma.udma_shift;
	dev->udma_count = 2;

	xa_init_flags(&dev->qp_tbl, GFP_ATOMIC);
	rwlock_init(&dev->qp_tbl_rw);
	xa_init_flags(&dev->cq_tbl, GFP_ATOMIC);
	rwlock_init(&dev->cq_tbl_rw);

	mutex_init(&dev->inuse_lock);
	spin_lock_init(&dev->inuse_splock);

	rc = ionic_resid_init(&dev->inuse_pdid, IONIC_MAX_PD);
	if (rc)
		goto err_pdid;

	rc = ionic_resid_init(&dev->inuse_ahid,
			      le32_to_cpu(ident->rdma.nahs_per_lif));
	if (rc)
		goto err_ahid;

	rc = ionic_resid_init(&dev->inuse_mrid,
			      le32_to_cpu(ident->rdma.nmrs_per_lif));
	if (rc)
		goto err_mrid;

	/* skip reserved lkey */
	dev->inuse_mrid.next_id = 1;
	dev->next_mrkey = 1;

	rc = ionic_resid_init(&dev->inuse_cqid,
			      le32_to_cpu(ident->rdma.cq_qtype.qid_count));
	if (rc)
		goto err_cqid;

	dev->next_cqid[0] = 0;
	dev->next_cqid[1] = dev->inuse_cqid.inuse_size / dev->udma_count;
	dev->half_cqid_udma_shift =
		order_base_2(dev->inuse_cqid.inuse_size / dev->udma_count);

	dev->size_qpid = le32_to_cpu(ident->rdma.sq_qtype.qid_count);
	rc = ionic_resid_init(&dev->inuse_qpid, dev->size_qpid);
	if (rc)
		goto err_qpid;

	/* skip reserved SMI and GSI qpids */
	dev->next_qpid[0] = 2;
	dev->next_qpid[1] = dev->size_qpid / dev->udma_count;
	dev->half_qpid_udma_shift =
		order_base_2(dev->size_qpid / dev->udma_count);

	rc = ionic_rdma_reset_devcmd(dev);
	if (rc)
		goto err_reset;

	rc = ionic_create_rdma_admin(dev);
	if (rc)
		goto err_register;

	ibdev = &dev->ibdev;
	ibdev->dev.parent = dev->hwdev;

	strscpy(ibdev->name, "ionic_%d", IB_DEVICE_NAME_MAX);
	strscpy(ibdev->node_desc, DEVICE_DESCRIPTION, IB_DEVICE_NODE_DESC_MAX);

	ibdev->node_type = RDMA_NODE_IB_CA;
	ibdev->phys_port_cnt = 1;

	/* the first two eq are reserved for async events */
	ibdev->num_comp_vectors = dev->eq_count - 2;

	addrconf_ifid_eui48((u8 *)&ibdev->node_guid, ndev);

	ibdev->uverbs_cmd_mask =
		BIT_ULL(IB_USER_VERBS_CMD_GET_CONTEXT)		|
		BIT_ULL(IB_USER_VERBS_CMD_QUERY_DEVICE)		|
		BIT_ULL(IB_USER_VERBS_CMD_QUERY_PORT)		|
		0;

	ib_set_device_ops(&dev->ibdev, &ionic_dev_ops);
	ionic_datapath_setops(dev);
	ionic_controlpath_setops(dev);

	ionic_stats_init(dev);

	rc = ib_register_device(ibdev, "ionic_%d", ibdev->dev.parent);
	if (rc)
		goto err_stats;

	return dev;

err_stats:
	ionic_stats_cleanup(dev);
err_register:
	ionic_kill_rdma_admin(dev, false);
	ionic_destroy_rdma_admin(dev);
err_reset:
	ionic_resid_destroy(&dev->inuse_qpid);
err_qpid:
	ionic_resid_destroy(&dev->inuse_cqid);
err_cqid:
	ionic_resid_destroy(&dev->inuse_mrid);
err_mrid:
	ionic_resid_destroy(&dev->inuse_ahid);
err_ahid:
	ionic_resid_destroy(&dev->inuse_pdid);
err_pdid:
	xa_destroy(&dev->qp_tbl);
	xa_destroy(&dev->cq_tbl);
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

	ionic_evt_workq = create_workqueue(DRIVER_NAME "-evt");
	if (!ionic_evt_workq)
		return -ENOMEM;

	rc = auxiliary_driver_register(&ionic_aux_r_driver);
	if (rc)
		goto err_aux;

	return 0;

err_aux:
	destroy_workqueue(ionic_evt_workq);

	return rc;
}

static void __exit ionic_mod_exit(void)
{
	auxiliary_driver_unregister(&ionic_aux_r_driver);
	destroy_workqueue(ionic_evt_workq);
}

module_init(ionic_mod_init);
module_exit(ionic_mod_exit);
