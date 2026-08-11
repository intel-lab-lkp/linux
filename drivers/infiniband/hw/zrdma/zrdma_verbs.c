// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#include "zrdma_main.h"
#include "zrdma_verbs.h"
#include "zrdma_ctrl.h"
#include <rdma/ib_mad.h>
#include <rdma/ib_cache.h>
#include <rdma/ib_user_verbs.h>
#include <linux/etherdevice.h>

static void zxdh_sc_pd_init(struct zxdh_sc_dev *dev, struct zxdh_sc_pd *pd,
			    u32 pd_id, int abi_ver)
{
	pd->pd_id = pd_id;
	pd->abi_ver = abi_ver;
	pd->dev = dev;
}

static void zxdh_ib_dealloc_device(struct ib_device *ibdev)
{
	struct zxdh_device *zdev = to_zdev(ibdev);

	kfree(zdev->rf);
	zdev->rf = NULL;
}

static int zxdh_alloc_pd(struct ib_pd *pd, struct ib_udata *udata)
{
	struct zxdh_device *zdev = to_zdev(pd->device);
	struct zxdh_sc_dev *dev = &zdev->rf->sc_dev;
	struct zxdh_alloc_pd_resp uresp = {};
	struct zxdh_pd *zpd = to_zpd(pd);
	struct zxdh_pci_f *rf = zdev->rf;
	struct zxdh_sc_pd *sc_pd;
	u32 pd_id;
	int err;

	err = zxdh_alloc_rsrc(rf, rf->allocated_pds, rf->max_pd, &pd_id,
			      &rf->next_pd);
	if (err) {
		pr_warn("zrdma: zxdh_alloc_rsrc failed err=%d\n", err);
		return err;
	}

	sc_pd = &zpd->sc_pd;
	if (udata) {
		struct zxdh_ucontext *ucontext;

		ucontext = rdma_udata_to_drv_context(udata,
						     struct zxdh_ucontext,
						     ibucontext);

		zxdh_sc_pd_init(dev, sc_pd, pd_id, ucontext->abi_ver);
		uresp.pd_id = pd_id;
		if (ib_copy_to_udata(udata, &uresp,
				     min(sizeof(uresp), udata->outlen))) {
			err = -EFAULT;
			goto error;
		}
	} else {
		zxdh_sc_pd_init(dev, sc_pd, pd_id, ZXDH_ABI_VER);
	}

	return 0;

error:
	zxdh_free_rsrc(rf, rf->allocated_pds, pd_id);

	return err;
}

static struct rdma_user_mmap_entry *
zxdh_user_mmap_entry_insert(struct zxdh_ucontext *ucontext, u64 bar_offset,
			    enum zxdh_mmap_flag mmap_flag, u64 *mmap_offset)
{
	struct zxdh_user_mmap_entry *entry =
		kzalloc(sizeof(*entry), GFP_KERNEL);
	int ret;

	if (!entry)
		return NULL;

	entry->bar_offset = bar_offset;
	entry->mmap_flag = mmap_flag;

	ret = rdma_user_mmap_entry_insert(&ucontext->ibucontext,
					  &entry->rdma_entry, PAGE_SIZE);
	if (ret) {
		kfree(entry);
		return NULL;
	}
	*mmap_offset = rdma_user_mmap_get_offset(&entry->rdma_entry);

	return &entry->rdma_entry;
}

static struct rdma_user_mmap_entry *
zxdh_mp_mmap_entry_insert(struct zxdh_ucontext *ucontext, u64 phy_addr,
			  size_t length, enum zxdh_mmap_flag mmap_flag,
			  u64 *mmap_offset)
{
	struct zxdh_user_mmap_entry *entry =
		kzalloc(sizeof(*entry), GFP_KERNEL);
	int ret;

	if (!entry)
		return NULL;

	entry->bar_offset = phy_addr;
	entry->mmap_flag = mmap_flag;

	ret = rdma_user_mmap_entry_insert(&ucontext->ibucontext,
					  &entry->rdma_entry, length);
	if (ret) {
		kfree(entry);
		return NULL;
	}
	*mmap_offset = rdma_user_mmap_get_offset(&entry->rdma_entry);

	return &entry->rdma_entry;
}

static int zxdh_alloc_ucontext(struct ib_ucontext *uctx, struct ib_udata *udata)
{
	struct zxdh_ucontext *ucontext = to_ucontext(uctx);
	struct zxdh_alloc_ucontext_resp uresp = {};
	struct ib_device *ibdev = uctx->device;
	struct zxdh_device *zdev = to_zdev(ibdev);
	struct zxdh_alloc_ucontext_req req = {};

	if (ib_copy_from_udata(&req, udata, min(sizeof(req), udata->inlen)))
		return -EINVAL;

	if (req.userspace_ver != ZXDH_CONTEXT_VER_V1 &&
	    req.userspace_ver != ZXDH_CONTEXT_VER_V2 &&
	    req.userspace_ver != ZXDH_CONTEXT_VER_V3) {
		pr_err("zrdma: Invalid userspace_ver%d\n", req.userspace_ver);
		return -EINVAL;
	}
	ucontext->zdev = zdev;
	ucontext->abi_ver = req.userspace_ver;

	uresp.srq_db_bar_off = SRQ_DB_OFFSET;

	ucontext->sq_db_mmap_entry =
		zxdh_user_mmap_entry_insert(ucontext, SQ_DB_BAR_OFF,
					    ZXDH_MMAP_IO_NC,
					    &uresp.sq_db_mmap_key);
	if (!ucontext->sq_db_mmap_entry)
		return -ENOMEM;

	ucontext->cq_db_mmap_entry =
		zxdh_user_mmap_entry_insert(ucontext, CQ_DB_BAR_OFF,
					    ZXDH_MMAP_IO_NC,
					    &uresp.cq_db_mmap_key);
	if (!ucontext->cq_db_mmap_entry) {
		rdma_user_mmap_entry_remove(ucontext->sq_db_mmap_entry);
		return -ENOMEM;
	}

	if (ucontext->abi_ver == ZXDH_CONTEXT_VER_V1 ||
	    ucontext->abi_ver == ZXDH_CONTEXT_VER_V2) {
		ucontext->srq_db_mmap_entry =
			zxdh_mp_mmap_entry_insert(ucontext, SRQ_DB_BAR_OFF,
						  PAGE_SIZE,
						  ZXDH_MMAP_IO_NC_BY_SIZE,
						  &uresp.srq_db_mmap_key);
	} else {
		ucontext->srq_db_mmap_entry =
			zxdh_mp_mmap_entry_insert(ucontext, SRQ_DB_BAR_OFF,
						  SRQ_DB_MMAP_SIZE,
						  ZXDH_MMAP_IO_NC_BY_SIZE,
						  &uresp.srq_db_mmap_key);
	}
	if (!ucontext->srq_db_mmap_entry) {
		rdma_user_mmap_entry_remove(ucontext->sq_db_mmap_entry);
		rdma_user_mmap_entry_remove(ucontext->cq_db_mmap_entry);
		pr_err("zrdma: srq_db_mmap_entry is NULL!\n");
		return -ENOMEM;
	}

	uresp.srq_db_mmap_size = SRQ_DB_MMAP_SIZE;
	uresp.kernel_ver = ZXDH_CONTEXT_VER_V3;
	uresp.hw_rev = ZXDH_GEN_2;
	uresp.chip_rev = CHIP_VERSION;
	uresp.rdma_tool_flags =
		ZXDH_QP_EXTEND_OP | ZXDH_CAPTURE | ZXDH_GET_HW_DATA |
		ZXDH_GET_HW_OBJECT_DATA | ZXDH_CHECK_HW_HEALTH |
		ZXDH_RDMA_TOOL_CFG_DEV_PARAM | ZXDH_RDMA_TOOL_READ_RAM |
		ZXDH_RDMA_TOOL_DEVX_MODIFY_CQ | ZXDH_RDMA_SET_CREDIT_CAP |
		ZXDH_RDMA_SWITCH | ZXDH_RDMA_TOOL_DEVX_EXT_MEM |
		ZXDH_RDMA_TOOL_PRIV_EXT;

	uresp.feature_flags = ZXDH_FEATURE_RTS_AE | ZXDH_FEATURE_CQ_RESIZE |
			      ZXDH_FEATURE_64_BYTE_CQE;
	uresp.max_hw_wq_frags = MAX_HW_WQ_FRAGS;
	uresp.max_hw_read_sges = MAX_HW_READ_SGES;
	uresp.max_hw_inline = MAX_HW_INLINE;
	uresp.max_hw_srq_wr = MAX_HW_SRQ_WR;
	uresp.max_hw_rq_quanta = MAX_HW_RQ_QUANTA;
	uresp.max_hw_srq_quanta = MAX_HW_SRQ_QUANTA;
	uresp.max_hw_wq_quanta = MAX_HW_WQ_QUANTA;
	uresp.max_hw_sq_chunk = MAX_HW_SQ_CHUNK;
	uresp.max_hw_cq_size = MAX_HW_CQ_SIZE;
	uresp.min_hw_cq_size = MIN_HW_CQ_SIZE;
	uresp.db_addr_type = ZXDH_DB_ADDR_BAR;
	uresp.sq_db_bar_off = SQ_DB_BAR_OFF & ZXDH_PAGE_OFFSET_NUM;
	uresp.cq_db_bar_off = CQ_DB_BAR_OFF & ZXDH_PAGE_OFFSET_NUM;

	if (ib_copy_to_udata(udata, &uresp,
			     min(sizeof(uresp), udata->outlen))) {
		rdma_user_mmap_entry_remove(ucontext->sq_db_mmap_entry);
		rdma_user_mmap_entry_remove(ucontext->cq_db_mmap_entry);
		rdma_user_mmap_entry_remove(ucontext->srq_db_mmap_entry);
		return -EFAULT;
	}

	INIT_LIST_HEAD(&ucontext->cq_reg_mem_list);
	spin_lock_init(&ucontext->cq_reg_mem_list_lock);
	INIT_LIST_HEAD(&ucontext->qp_reg_mem_list);
	spin_lock_init(&ucontext->qp_reg_mem_list_lock);
	INIT_LIST_HEAD(&ucontext->srq_reg_mem_list);
	spin_lock_init(&ucontext->srq_reg_mem_list_lock);

	return 0;
}

static int zxdh_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	struct zxdh_pd *zpd = to_zpd(ibpd);
	struct zxdh_device *zdev = to_zdev(ibpd->device);

	zxdh_free_rsrc(zdev->rf, zdev->rf->allocated_pds, zpd->sc_pd.pd_id);
	return 0;
}

static void zxdh_dealloc_ucontext(struct ib_ucontext *context)
{
	struct zxdh_ucontext *ucontext = to_ucontext(context);

	if (!ucontext)
		return;

	if (ucontext->sq_db_mmap_entry)
		rdma_user_mmap_entry_remove(ucontext->sq_db_mmap_entry);
	if (ucontext->cq_db_mmap_entry)
		rdma_user_mmap_entry_remove(ucontext->cq_db_mmap_entry);
	if (ucontext->srq_db_mmap_entry)
		rdma_user_mmap_entry_remove(ucontext->srq_db_mmap_entry);
}

static void zxdh_mmap_free(struct rdma_user_mmap_entry *entry)
{
	struct zxdh_user_mmap_entry *zentry = to_zxdh_mmap_entry(entry);

	kfree(zentry);
}

static int zxdh_mmap(struct ib_ucontext *context, struct vm_area_struct *vma)
{
	struct rdma_user_mmap_entry *rdma_entry;
	struct zxdh_user_mmap_entry *entry;
	struct zxdh_ucontext *ucontext;
	u64 pfn;
	int ret;

	ucontext = to_ucontext(context);

	rdma_entry = rdma_user_mmap_entry_get(&ucontext->ibucontext, vma);
	if (!rdma_entry) {
		pr_err("zrdma: pgoff[0x%lx] does not have valid entry\n",
		       vma->vm_pgoff);
		return -EINVAL;
	}

	entry = to_zxdh_mmap_entry(rdma_entry);

	pfn = (entry->bar_offset +
	       pci_resource_start(ucontext->zdev->rf->pcidev, 0)) >>
	      PAGE_SHIFT;

	switch (entry->mmap_flag) {
	case ZXDH_MMAP_IO_NC:
		ret = rdma_user_mmap_io(context, vma, pfn, PAGE_SIZE,
					pgprot_noncached(vma->vm_page_prot),
					rdma_entry);
		break;
	case ZXDH_MMAP_IO_NC_BY_SIZE:
		ret = rdma_user_mmap_io(context, vma, pfn,
					vma->vm_end - vma->vm_start,
					pgprot_noncached(vma->vm_page_prot),
					rdma_entry);
		break;
	case ZXDH_MMAP_IO_WC:
		ret = rdma_user_mmap_io(context, vma, pfn, PAGE_SIZE,
					pgprot_writecombine(vma->vm_page_prot),
					rdma_entry);
		break;
	default:
		pr_err("[zxdh_rdma] VERBS: unsupported mmap_flag[%d]\n",
		       entry->mmap_flag);
		ret = -EINVAL;
	}

	if (ret)
		pr_err("[zxdh_rdma] VERBS: bar_offset [0x%llx] mmap_flag[%d] err[%d]\n",
		       entry->bar_offset, entry->mmap_flag, ret);

	rdma_user_mmap_entry_put(rdma_entry);

	return ret;
}

static void extract_version(const char *input, char *output)
{
	const char *last_dash_pos = strrchr(input, '-');

	if (last_dash_pos) {
		const char *v_pos = strstr(last_dash_pos, "V");

		if (v_pos)
			strscpy(output, v_pos + 1, 12);
		else
			output[0] = '\0';
	} else {
		output[0] = '\0';
	}
}

static int zxdh_query_device(struct ib_device *ibdev,
			     struct ib_device_attr *props,
			     struct ib_udata *udata)
{
	struct zxdh_device *zdev = to_zdev(ibdev);
	struct pci_dev *pcidev = zdev->rf->pcidev;
	int major, sub_major, minor, sub_minor;
	struct net_device *slave = NULL;
	struct ethtool_drvinfo info;
	char extracted_version[16];
	struct list_head *iter;

	memset(&info, 0, sizeof(info));
	if (zdev->netdev->priv_flags & IFF_BONDING) {
		rcu_read_lock();
		netdev_for_each_lower_dev(zdev->netdev, slave, iter) {
			slave->ethtool_ops->get_drvinfo(slave, &info);
			break;
		}
		rcu_read_unlock();
		if (!slave) {
			zdev->netdev->ethtool_ops->get_drvinfo(zdev->netdev,
							       &info);
		}
	} else {
		zdev->netdev->ethtool_ops->get_drvinfo(zdev->netdev, &info);
	}
	extract_version(info.fw_version, extracted_version);
	if (sscanf(extracted_version, "%d.%d.%d.%d", &major, &sub_major, &minor,
		   &sub_minor) != 4) {
		major = 0;
		sub_major = 0;
		minor = 0;
		sub_minor = 0;
	}

	if (udata->inlen || udata->outlen)
		return -EINVAL;

	memset(props, 0, sizeof(*props));
	ether_addr_copy((u8 *)&props->sys_image_guid, zdev->netdev->dev_addr);
	props->fw_ver = ((u64)major << 48 | (u64)sub_major << 32 |
			 (u64)minor << 16 | sub_minor);
	props->device_cap_flags =
		IB_DEVICE_MEM_WINDOW | IB_DEVICE_MEM_MGT_EXTENSIONS |
		IB_DEVICE_BAD_QKEY_CNTR | IB_DEVICE_SYS_IMAGE_GUID |
		IB_DEVICE_RC_RNR_NAK_GEN | IB_DEVICE_N_NOTIFY_CQ;
	props->vendor_id = pcidev->vendor;
	props->vendor_part_id = pcidev->device;
	props->hw_ver = pcidev->revision;
	props->page_size_cap = SZ_4K | SZ_2M | SZ_1G;
	props->max_mr_size = MAX_MR_SIZE;
	props->max_qp = MAX_QP;
	props->max_qp_wr = MAX_QP_WR;
	props->max_cq = MAX_CQ;
	props->max_cqe = MAX_CQE;
	props->max_mr = MAX_MR;
	props->max_mw = MAX_MW;
	props->max_pd = MAX_PD;
	props->max_sge_rd = MAX_SGE_RD;
	props->max_qp_rd_atom = MAX_QP_RD_ATOM;
	props->max_res_rd_atom = MAX_RES_RD_ATOM;
	props->max_qp_init_rd_atom = MAX_QP_INIT_RD_ATOM;
	props->max_srq = MAX_SRQ;
	props->max_srq_wr = MAX_SRQ_WR;
	props->max_srq_sge = MAX_SRQ_SGE;
	props->local_ca_ack_delay = LOCAL_CA_ACK_DELAY;
	props->hca_core_clock = HCA_CORE_CLOCK;
	props->max_wq_type_rq = MAX_WQ_TYPE_RQ;
	if (rdma_protocol_roce(ibdev, 1)) {
		props->max_pkeys = ZXDH_PKEY_TBL_SZ;
		props->max_ah = MAX_AH;
		props->max_mcast_grp = 0;
		props->max_mcast_qp_attach = 0;
		props->max_total_mcast_qp_attach = 0;
	}
	props->max_fast_reg_page_list_len = MAX_FAST_REG_PAGE_LIST_LEN;
	props->cq_caps.max_cq_moderation_count = MAX_CQ_MODERATION_COUNT;
	props->cq_caps.max_cq_moderation_period = MAX_CQ_MODERATION_PERIOD;
	props->timestamp_mask = TIMESTAMP_MASK;

	return 0;
}

static u32 zxdh_get_eth_netdev_speed(struct zxdh_device *zdev, u32 port_num)
{
	struct ethtool_link_ksettings lksettings;
	u32 netdev_speed = (u32)SPEED_UNKNOWN;
	struct zxdh_pci_f *rf = zdev->rf;
	int rc;

	if (rdma_port_get_link_layer(&zdev->ibdev, port_num) !=
	    IB_LINK_LAYER_ETHERNET)
		return -EINVAL;

	if (zdev->netdev_speed != (u32)SPEED_UNKNOWN)
		return zdev->netdev_speed;

	if (rf->drv_np_cap == ZXDH_RDMA_COMMON_FUNC_CAP &&
	    rf->common_func_num_max > ZXDH_FUNC_NETDEV_SPEED_GET) {
		rc = rf->gen_ops.zxdh_common_func(zdev->zxdh_adev->parent,
						  &netdev_speed,
						  ZXDH_FUNC_NETDEV_SPEED_GET);

		if (rc || netdev_speed == (u32)SPEED_UNKNOWN)
			netdev_speed = SPEED_1000;
		else
			zdev->netdev_speed = netdev_speed;
	} else {
		if (zdev->netdev_speed == (u32)SPEED_UNKNOWN) {
			rtnl_lock();
			rc = __ethtool_get_link_ksettings(zdev->netdev,
							  &lksettings);
			rtnl_unlock();

			if (!rc &&
			    lksettings.base.speed != (u32)SPEED_UNKNOWN) {
				netdev_speed = lksettings.base.speed;
				zdev->netdev_speed = netdev_speed;
			} else {
				netdev_speed = SPEED_1000;
			}
		} else {
			netdev_speed = zdev->netdev_speed;
		}
	}

	return netdev_speed;
}

static int zxdh_get_eth_speed(struct ib_device *dev, struct net_device *netdev,
			      u32 port_num, u16 *speed, u8 *width)
{
	struct zxdh_device *zdev = to_zdev(dev);
	u32 netdev_speed;

	if (zdev)
		netdev_speed = zxdh_get_eth_netdev_speed(zdev, port_num);
	else
		netdev_speed = SPEED_1000;

	if (netdev_speed <= SPEED_1000) {
		*width = IB_WIDTH_1X;
		*speed = IB_SPEED_SDR;
	} else if (netdev_speed <= SPEED_10000) {
		*width = IB_WIDTH_1X;
		*speed = IB_SPEED_FDR10;
	} else if (netdev_speed <= SPEED_20000) {
		*width = IB_WIDTH_4X;
		*speed = IB_SPEED_DDR;
	} else if (netdev_speed <= SPEED_25000) {
		*width = IB_WIDTH_1X;
		*speed = IB_SPEED_EDR;
	} else if (netdev_speed <= SPEED_40000) {
		*width = IB_WIDTH_4X;
		*speed = IB_SPEED_FDR10;
	} else if (netdev_speed <= SPEED_100000) {
		*width = IB_WIDTH_4X;
		*speed = IB_SPEED_EDR;
	} else {
		*width = IB_WIDTH_4X;
		*speed = IB_SPEED_HDR;
	}

	return 0;
}

static int zxdh_query_port(struct ib_device *ibdev, u32 port,
			   struct ib_port_attr *props)
{
	struct zxdh_device *zdev = to_zdev(ibdev);
	struct net_device *netdev = zdev->netdev;

	props->max_mtu = IB_MTU_4096;
	props->active_mtu = zxdh_mtu_int_to_enum(netdev->mtu);
	props->lid = 0;
	props->lmc = 0;
	props->sm_lid = 0;
	props->sm_sl = 0;
	if (netif_carrier_ok(netdev) && netif_running(netdev)) {
		props->state = IB_PORT_ACTIVE;
		props->phys_state = IB_PORT_PHYS_STATE_LINK_UP;
	} else {
		props->state = IB_PORT_DOWN;
		props->phys_state = IB_PORT_PHYS_STATE_DISABLED;
	}
	zxdh_get_eth_speed(ibdev, netdev, port, &props->active_speed,
			   &props->active_width);
	if (rdma_protocol_roce(ibdev, 1)) {
		props->gid_tbl_len = 255;
		props->ip_gids = true;
		props->pkey_tbl_len = ZXDH_PKEY_TBL_SZ;
	} else {
		props->gid_tbl_len = 1;
	}
	props->port_cap_flags |= IB_PORT_CM_SUP;
	props->max_msg_sz = MAX_MSG_SZ;
	props->qkey_viol_cntr = 0;

	return 0;
}

static enum rdma_link_layer zxdh_get_link_layer(struct ib_device *ibdev,
						u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

static int zxdh_query_pkey(struct ib_device *ibdev, u32 port, u16 index,
			   u16 *pkey)
{
	if (index >= ZXDH_PKEY_TBL_SZ)
		return -EINVAL;

	*pkey = ZXDH_DEFAULT_PKEY;
	return 0;
}

static int zxdh_query_gid_roce(struct ib_device *ibdev, u32 port, int index,
			       union ib_gid *gid)
{
	int ret;

	ret = rdma_query_gid(ibdev, port, index, gid);
	if (ret == -EAGAIN) {
		memcpy(gid, &zgid, sizeof(*gid));
		return 0;
	}

	return ret;
}

static int zxdh_roce_port_immutable(struct ib_device *ibdev, u32 port_num,
				    struct ib_port_immutable *immutable)
{
	struct ib_port_attr attr;
	int err;

	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP |
				    RDMA_CORE_CAP_PROT_ROCE;
	err = ib_query_port(ibdev, port_num, &attr);
	if (err)
		return err;

	immutable->max_mad_size = IB_MGMT_MAD_SIZE;
	immutable->pkey_tbl_len = attr.pkey_tbl_len;
	immutable->gid_tbl_len = attr.gid_tbl_len;

	return 0;
}

static void zxdh_get_dev_fw_str(struct ib_device *dev, char *str)
{
	struct zxdh_device *zdev = to_zdev(dev);
	struct net_device *slave = NULL;
	struct ethtool_drvinfo info;
	char extracted_version[16];
	struct list_head *iter;

	memset(&info, 0, sizeof(info));

	if (zdev->netdev->priv_flags & IFF_BONDING) {
		rcu_read_lock();
		netdev_for_each_lower_dev(zdev->netdev, slave, iter) {
			slave->ethtool_ops->get_drvinfo(slave, &info);
			break;
		}
		rcu_read_unlock();
		if (!slave) {
			zdev->netdev->ethtool_ops->get_drvinfo(zdev->netdev,
							       &info);
		}
		extract_version(info.fw_version, extracted_version);
		snprintf(str, IB_FW_VERSION_NAME_MAX, "%s", extracted_version);
		return;
	}
	zdev->netdev->ethtool_ops->get_drvinfo(zdev->netdev, &info);
	extract_version(info.fw_version, extracted_version);
	snprintf(str, IB_FW_VERSION_NAME_MAX, "%s", extracted_version);
}

static const struct ib_device_ops zxdh_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_ZRDMA,
	.uverbs_abi_ver = ZXDH_ABI_VER,
	.alloc_pd = zxdh_alloc_pd,
	.alloc_ucontext = zxdh_alloc_ucontext,
	.dealloc_driver = zxdh_ib_dealloc_device,
	.dealloc_pd = zxdh_dealloc_pd,
	.dealloc_ucontext = zxdh_dealloc_ucontext,
	.query_device = zxdh_query_device,
	.query_port = zxdh_query_port,
	.query_gid = zxdh_query_gid_roce,
	.query_pkey = zxdh_query_pkey,
	.get_link_layer = zxdh_get_link_layer,
	.get_port_immutable = zxdh_roce_port_immutable,
	.get_dev_fw_str = zxdh_get_dev_fw_str,
	.mmap = zxdh_mmap,
	.mmap_free = zxdh_mmap_free,

	INIT_RDMA_OBJ_SIZE(ib_pd, zxdh_pd, ibpd),
	INIT_RDMA_OBJ_SIZE(ib_ucontext, zxdh_ucontext, ibucontext),
};

static void zxdh_init_roce_device(struct zxdh_device *zdev)
{
	zdev->ibdev.node_type = RDMA_NODE_IB_CA;
	addrconf_addr_eui48((u8 *)&zdev->ibdev.node_guid,
			    zdev->netdev->dev_addr);
}

static int zxdh_init_rdma_device(struct zxdh_device *zdev)
{
	struct pci_dev *pcidev = zdev->rf->pcidev;

	if (zdev->roce_mode)
		zxdh_init_roce_device(zdev);
	else
		return -EPFNOSUPPORT;

	zdev->ibdev.phys_port_cnt = 1;
	zdev->ibdev.num_comp_vectors = zdev->rf->ceqs_count;
	zdev->ibdev.dev.parent = &pcidev->dev;

	ib_set_device_ops(&zdev->ibdev, &zxdh_dev_ops);

	return 0;
}

static void zxdh_port_ibevent(struct zxdh_device *zdev)
{
	struct ib_event event;

	event.device = &zdev->ibdev;
	event.element.port_num = 1;
	event.event = zdev->ibdev_status ? IB_EVENT_PORT_ACTIVE :
						 IB_EVENT_PORT_ERR;
	ib_dispatch_event(&event);
}

int zxdh_ib_register_device(struct zxdh_device *zdev)
{
	char name[IB_DEVICE_NAME_MAX] = {};
	int ret;
	int err;

	ret = zxdh_init_rdma_device(zdev);
	if (ret)
		return ret;

	ret = zxdh_get_del_rdma_name(pci_name(zdev->rf->pcidev), name);
	if (ret)
		snprintf(name, IB_DEVICE_NAME_MAX, "%s", "zrdma%d");

	ret = ib_device_set_netdev(&zdev->ibdev, zdev->netdev, 1);
	if (ret)
		goto error;

	zdev->rf->hw.device = &zdev->rf->pcidev->dev;
	dma_set_max_seg_size(zdev->rf->hw.device, SZ_1G);

	ret = ib_register_device(&zdev->ibdev, name, zdev->rf->hw.device);
	if (ret) {
		snprintf(name, IB_DEVICE_NAME_MAX, "%s", "zrdma%d");
		err = ib_register_device(&zdev->ibdev, name,
					 zdev->rf->hw.device);
		if (err)
			goto error;
	}

	zdev->ibdev_status = 1;
	zxdh_port_ibevent(zdev);

	return 0;

error:
	if (ret)
		pr_err("zrdma: Register RDMA device fail\n");

	return ret;
}
