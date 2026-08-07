// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2015-2018 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <linux/net.h>
#include <rdma/opa_addr.h>
#define OPA_NUM_PKEY_BLOCKS_PER_SMP \
	(OPA_SMP_DR_DATA_SIZE / (OPA_PARTITION_TABLE_BLK_SIZE * sizeof(u16)))

#include "hfi2.h"
#include "file_ops.h"
#include "chip_gen.h"
#include "mad.h"
#include "trace.h"
#include "qp.h"
#include "rdma/ib_sa.h"
#include "vf2pf.h"

#undef CPORT_MAD_TRACE /* lots of messages for every local MAD */
#undef CPORT_UMAD_TRACE /* lots of messages for every user MAD */
/* GET PORT_INFO is constant while the FM is running, so control those separately. */
#undef GET_PORT_INFO_DEBUG

#undef DEBUG_MAD_MCTXT

static int cport_mad_to = 1;

/* offset from start of packet for the MAD header */
#define MAD_9B_OFFSET (8 + 12 + 8) /* 9B LRH, BTH, DETH */
#define MAD_16B_FM_OFFSET (16 + 8) /* 16B LRH, Mgmt L4 */
#define MAD_16B_IB_OFFSET (16 + 12 + 8) /* 16B LRH, BTH, DETH */

/* the reset value from the FM is supposed to be 0xffff, handle both */
#define OPA_LINK_WIDTH_RESET_OLD 0x0fff
#define OPA_LINK_WIDTH_RESET 0xffff

#define OPA_WC_MCTXT_UMAD 1 /* MAD arrive via MCTXT UMAD channel */

struct trap_node {
	struct list_head list;
	struct opa_mad_notice_attr data;
	__be64 tid;
	int len;
	u32 retry;
	u8 in_use;
	u8 repress;
};

static int smp_length_check(u32 data_size, u32 request_len)
{
	if (unlikely(request_len < data_size))
		return -EINVAL;

	return 0;
}

static int reply(struct ib_mad_hdr *smp)
{
	/*
	 * The verbs framework will handle the directed/LID route
	 * packet changes.
	 */
	smp->method = IB_MGMT_METHOD_GET_RESP;
	if (smp->mgmt_class == IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE)
		smp->status |= IB_SMP_DIRECTION;
	return IB_MAD_RESULT_SUCCESS | IB_MAD_RESULT_REPLY;
}

static inline void clear_opa_smp_data(struct opa_smp *smp)
{
	void *data = opa_get_smp_data(smp);
	size_t size = opa_get_smp_data_size(smp);

	memset(data, 0, size);
}

static u16 hfi2_lookup_pkey_value(struct hfi2_ibport *ibp, int pkey_idx)
{
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);

	if (pkey_idx < ppd->dd->params->pkey_table_size)
		return ppd->pkeys[pkey_idx];

	return 0;
}

void hfi2_event_pkey_change(struct hfi2_devdata *dd, u32 port)
{
	struct ib_event event;

	event.event = IB_EVENT_PKEY_CHANGE;
	event.device = &dd->verbs_dev.rdi.ibdev;
	event.element.port_num = port;
	ib_dispatch_event(&event);
}

/*
 * If the port is down, clean up all pending traps.  We need to be careful
 * with the given trap, because it may be queued.
 */
static void cleanup_traps(struct hfi2_ibport *ibp, struct trap_node *trap)
{
	struct trap_node *node, *q;
	unsigned long flags;
	struct list_head trap_list;
	int i;

	for (i = 0; i < RVT_MAX_TRAP_LISTS; i++) {
		spin_lock_irqsave(&ibp->rvp.lock, flags);
		list_replace_init(&ibp->rvp.trap_lists[i].list, &trap_list);
		ibp->rvp.trap_lists[i].list_len = 0;
		spin_unlock_irqrestore(&ibp->rvp.lock, flags);

		/*
		 * Remove all items from the list, freeing all the non-given
		 * traps.
		 */
		list_for_each_entry_safe(node, q, &trap_list, list) {
			list_del(&node->list);
			if (node != trap)
				kfree(node);
		}
	}

	/*
	 * If this wasn't on one of the lists it would not be freed.  If it
	 * was on the list, it is now safe to free.
	 */
	kfree(trap);
}

static struct trap_node *check_and_add_trap(struct hfi2_ibport *ibp,
					    struct trap_node *trap)
{
	struct trap_node *node;
	struct trap_list *trap_list;
	unsigned long flags;
	unsigned long timeout;
	int found = 0;
	unsigned int queue_id;
	static int trap_count;

	queue_id = trap->data.generic_type & 0x0F;
	if (queue_id >= RVT_MAX_TRAP_LISTS) {
		trap_count++;
		pr_err_ratelimited(
			"hfi2: Invalid trap 0x%0x dropped. Total dropped: %d\n",
			trap->data.generic_type, trap_count);
		kfree(trap);
		return NULL;
	}

	/*
	 * Since the retry (handle timeout) does not remove a trap request
	 * from the list, all we have to do is compare the node.
	 */
	spin_lock_irqsave(&ibp->rvp.lock, flags);
	trap_list = &ibp->rvp.trap_lists[queue_id];

	list_for_each_entry(node, &trap_list->list, list) {
		if (node == trap) {
			node->retry++;
			found = 1;
			break;
		}
	}

	/* If it is not on the list, add it, limited to RVT-MAX_TRAP_LEN. */
	if (!found) {
		if (trap_list->list_len < RVT_MAX_TRAP_LEN) {
			trap_list->list_len++;
			list_add_tail(&trap->list, &trap_list->list);
		} else {
			pr_warn_ratelimited(
				"hfi2: Maximum trap limit reached for 0x%0x traps\n",
				trap->data.generic_type);
			kfree(trap);
		}
	}

	/*
	 * Next check to see if there is a timer pending.  If not, set it up
	 * and get the first trap from the list.
	 */
	node = NULL;
	if (!timer_pending(&ibp->rvp.trap_timer)) {
		/*
		 * o14-2
		 * If the time out is set we have to wait until it expires
		 * before the trap can be sent.
		 * This should be > RVT_TRAP_TIMEOUT
		 */
		timeout =
			(RVT_TRAP_TIMEOUT * (1UL << ibp->rvp.subnet_timeout)) /
			1000;
		mod_timer(&ibp->rvp.trap_timer,
			  jiffies + usecs_to_jiffies(timeout));
		node = list_first_entry(&trap_list->list, struct trap_node,
					list);
		node->in_use = 1;
	}
	spin_unlock_irqrestore(&ibp->rvp.lock, flags);

	return node;
}

static void subn_handle_opa_trap_repress(struct hfi2_ibport *ibp,
					 struct opa_smp *smp)
{
	struct trap_list *trap_list;
	struct trap_node *trap;
	unsigned long flags;
	int i;

	if (smp->attr_id != IB_SMP_ATTR_NOTICE)
		return;

	spin_lock_irqsave(&ibp->rvp.lock, flags);
	for (i = 0; i < RVT_MAX_TRAP_LISTS; i++) {
		trap_list = &ibp->rvp.trap_lists[i];
		trap = list_first_entry_or_null(&trap_list->list,
						struct trap_node, list);
		if (trap && trap->tid == smp->tid) {
			if (trap->in_use) {
				trap->repress = 1;
			} else {
				trap_list->list_len--;
				list_del(&trap->list);
				kfree(trap);
			}
			break;
		}
	}
	spin_unlock_irqrestore(&ibp->rvp.lock, flags);
}

static void hfi2_update_sm_ah_attr(struct hfi2_ibport *ibp,
				   struct rdma_ah_attr *attr, u32 dlid)
{
	rdma_ah_set_dlid(attr, dlid);
	rdma_ah_set_port_num(attr, ppd_from_ibp(ibp)->port);
	if (dlid >= be16_to_cpu(IB_MULTICAST_LID_BASE)) {
		struct ib_global_route *grh = rdma_ah_retrieve_grh(attr);

		rdma_ah_set_ah_flags(attr, IB_AH_GRH);
		grh->sgid_index = 0;
		grh->hop_limit = 1;
		grh->dgid.global.subnet_prefix = ibp->rvp.gid_prefix;
		grh->dgid.global.interface_id = OPA_MAKE_ID(dlid);
	}
}

static int hfi2_modify_qp0_ah(struct hfi2_ibport *ibp, struct rvt_ah *ah,
			      u32 dlid)
{
	struct rdma_ah_attr attr;
	struct rvt_qp *qp0;
	int ret = -EINVAL;

	memset(&attr, 0, sizeof(attr));
	attr.type = ah->ibah.type;
	hfi2_update_sm_ah_attr(ibp, &attr, dlid);
	rcu_read_lock();
	qp0 = rcu_dereference(ibp->rvp.qp[0]);
	if (qp0)
		ret = rdma_modify_ah(&ah->ibah, &attr);
	rcu_read_unlock();
	return ret;
}

static struct ib_ah *hfi2_create_qp0_ah(struct hfi2_ibport *ibp, u32 dlid)
{
	struct rdma_ah_attr attr;
	struct ib_ah *ah = ERR_PTR(-EINVAL);
	struct rvt_qp *qp0;
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	struct hfi2_devdata *dd = dd_from_ppd(ppd);
	u32 port_num = ppd->port;

	memset(&attr, 0, sizeof(attr));
	attr.type = rdma_ah_find_type(&dd->verbs_dev.rdi.ibdev, port_num);
	hfi2_update_sm_ah_attr(ibp, &attr, dlid);
	rcu_read_lock();
	qp0 = rcu_dereference(ibp->rvp.qp[0]);
	if (qp0)
		ah = rdma_create_ah(qp0->ibqp.pd, &attr, 0);
	rcu_read_unlock();
	return ah;
}

static void send_trap(struct hfi2_ibport *ibp, struct trap_node *trap)
{
	struct ib_mad_send_buf *send_buf;
	struct ib_mad_agent *agent;
	struct opa_smp *smp;
	unsigned long flags;
	int pkey_idx;
	u32 qpn = ppd_from_ibp(ibp)->sm_trap_qp;

	agent = ibp->rvp.send_agent;
	if (!agent) {
		cleanup_traps(ibp, trap);
		return;
	}

	/* o14-3.2.1 */
	if (hfi2_driver_lstate(ppd_from_ibp(ibp)) != IB_PORT_ACTIVE) {
		cleanup_traps(ibp, trap);
		return;
	}

	/* Add the trap to the list if necessary and see if we can send it */
	trap = check_and_add_trap(ibp, trap);
	if (!trap)
		return;

	pkey_idx = hfi2_lookup_pkey_idx(ibp, LIM_MGMT_P_KEY);
	if (pkey_idx < 0) {
		pr_warn("%s: failed to find limited mgmt pkey, defaulting 0x%x\n",
			__func__, hfi2_get_pkey(ibp, 1));
		pkey_idx = 1;
	}

	send_buf = ib_create_send_mad(agent, qpn, pkey_idx, 0, IB_MGMT_MAD_HDR,
				      IB_MGMT_MAD_DATA, GFP_ATOMIC,
				      IB_MGMT_BASE_VERSION);
	if (IS_ERR(send_buf))
		return;

	smp = send_buf->mad;
	smp->base_version = OPA_MGMT_BASE_VERSION;
	smp->mgmt_class = IB_MGMT_CLASS_SUBN_LID_ROUTED;
	smp->class_version = OPA_SM_CLASS_VERSION;
	smp->method = IB_MGMT_METHOD_TRAP;

	/* Only update the transaction ID for new traps (o13-5). */
	if (trap->tid == 0) {
		ibp->rvp.tid++;
		/* make sure that tid != 0 */
		if (ibp->rvp.tid == 0)
			ibp->rvp.tid++;
		trap->tid = cpu_to_be64(ibp->rvp.tid);
	}
	smp->tid = trap->tid;

	smp->attr_id = IB_SMP_ATTR_NOTICE;
	/* o14-1: smp->mkey = 0; */

	memcpy(smp->route.lid.data, &trap->data, trap->len);

	spin_lock_irqsave(&ibp->rvp.lock, flags);
	if (!ibp->rvp.sm_ah) {
		if (ibp->rvp.sm_lid != be16_to_cpu(IB_LID_PERMISSIVE)) {
			struct ib_ah *ah;

			ah = hfi2_create_qp0_ah(ibp, ibp->rvp.sm_lid);
			if (IS_ERR(ah)) {
				spin_unlock_irqrestore(&ibp->rvp.lock, flags);
				return;
			}
			send_buf->ah = ah;
			ibp->rvp.sm_ah = ibah_to_rvtah(ah);
		} else {
			spin_unlock_irqrestore(&ibp->rvp.lock, flags);
			return;
		}
	} else {
		send_buf->ah = &ibp->rvp.sm_ah->ibah;
	}

	/*
	 * If the trap was repressed while things were getting set up, don't
	 * bother sending it. This could happen for a retry.
	 */
	if (trap->repress) {
		list_del(&trap->list);
		spin_unlock_irqrestore(&ibp->rvp.lock, flags);
		kfree(trap);
		ib_free_send_mad(send_buf);
		return;
	}

	trap->in_use = 0;
	spin_unlock_irqrestore(&ibp->rvp.lock, flags);

	if (ib_post_send_mad(send_buf, NULL))
		ib_free_send_mad(send_buf);
}

void hfi2_handle_trap_timer(struct timer_list *t)
{
	struct hfi2_ibport *ibp = timer_container_of(ibp, t, rvp.trap_timer);
	struct trap_node *trap = NULL;
	unsigned long flags;
	int i;

	/* Find the trap with the highest priority */
	spin_lock_irqsave(&ibp->rvp.lock, flags);
	for (i = 0; !trap && i < RVT_MAX_TRAP_LISTS; i++) {
		trap = list_first_entry_or_null(&ibp->rvp.trap_lists[i].list,
						struct trap_node, list);
	}
	spin_unlock_irqrestore(&ibp->rvp.lock, flags);

	if (trap)
		send_trap(ibp, trap);
}

static struct trap_node *create_trap_node(u8 type, __be16 trap_num, u32 lid)
{
	struct trap_node *trap;

	trap = kzalloc_obj(*trap, GFP_ATOMIC);
	if (!trap)
		return NULL;

	INIT_LIST_HEAD(&trap->list);
	trap->data.generic_type = type;
	trap->data.prod_type_lsb = IB_NOTICE_PROD_CA;
	trap->data.trap_num = trap_num;
	trap->data.issuer_lid = cpu_to_be32(lid);

	return trap;
}

/*
 * Send a bad P_Key trap (ch. 14.3.8).
 */
void hfi2_bad_pkey(struct hfi2_ibport *ibp, u32 key, u32 sl, u32 qp1, u32 qp2,
		   u32 lid1, u32 lid2)
{
	struct trap_node *trap;
	u32 lid = ppd_from_ibp(ibp)->lid;

	ibp->rvp.n_pkt_drops++;
	ibp->rvp.pkey_violations++;

	trap = create_trap_node(IB_NOTICE_TYPE_SECURITY, OPA_TRAP_BAD_P_KEY,
				lid);
	if (!trap)
		return;

	/* Send violation trap */
	trap->data.ntc_257_258.lid1 = cpu_to_be32(lid1);
	trap->data.ntc_257_258.lid2 = cpu_to_be32(lid2);
	trap->data.ntc_257_258.key = cpu_to_be32(key);
	trap->data.ntc_257_258.sl = sl << 3;
	trap->data.ntc_257_258.qp1 = cpu_to_be32(qp1);
	trap->data.ntc_257_258.qp2 = cpu_to_be32(qp2);

	trap->len = sizeof(trap->data);
	send_trap(ibp, trap);
}

/*
 * Send a bad M_Key trap (ch. 14.3.9).
 */
static void bad_mkey(struct hfi2_ibport *ibp, struct ib_mad_hdr *mad,
		     __be64 mkey, __be32 dr_slid, u8 return_path[], u8 hop_cnt)
{
	struct trap_node *trap;
	u32 lid = ppd_from_ibp(ibp)->lid;

	trap = create_trap_node(IB_NOTICE_TYPE_SECURITY, OPA_TRAP_BAD_M_KEY,
				lid);
	if (!trap)
		return;

	/* Send violation trap */
	trap->data.ntc_256.lid = trap->data.issuer_lid;
	trap->data.ntc_256.method = mad->method;
	trap->data.ntc_256.attr_id = mad->attr_id;
	trap->data.ntc_256.attr_mod = mad->attr_mod;
	trap->data.ntc_256.mkey = mkey;
	if (mad->mgmt_class == IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE) {
		trap->data.ntc_256.dr_slid = dr_slid;
		trap->data.ntc_256.dr_trunc_hop = IB_NOTICE_TRAP_DR_NOTICE;
		if (hop_cnt > ARRAY_SIZE(trap->data.ntc_256.dr_rtn_path)) {
			trap->data.ntc_256.dr_trunc_hop |=
				IB_NOTICE_TRAP_DR_TRUNC;
			hop_cnt = ARRAY_SIZE(trap->data.ntc_256.dr_rtn_path);
		}
		trap->data.ntc_256.dr_trunc_hop |= hop_cnt;
		memcpy(trap->data.ntc_256.dr_rtn_path, return_path, hop_cnt);
	}

	trap->len = sizeof(trap->data);

	send_trap(ibp, trap);
}

/*
 * Send a Port Capability Mask Changed trap (ch. 14.3.11).
 */
void hfi2_cap_mask_chg(struct rvt_dev_info *rdi, u32 port_num)
{
	struct trap_node *trap;
	struct hfi2_ibdev *verbs_dev = dev_from_rdi(rdi);
	struct hfi2_devdata *dd = dd_from_dev(verbs_dev);
	struct hfi2_ibport *ibp = &dd->pport[port_num - 1].ibport_data;
	u32 lid = ppd_from_ibp(ibp)->lid;

	trap = create_trap_node(IB_NOTICE_TYPE_INFO, OPA_TRAP_CHANGE_CAPABILITY,
				lid);
	if (!trap)
		return;

	trap->data.ntc_144.lid = trap->data.issuer_lid;
	trap->data.ntc_144.new_cap_mask = cpu_to_be32(ibp->rvp.port_cap_flags);
	trap->data.ntc_144.cap_mask3 = cpu_to_be16(ibp->rvp.port_cap3_flags);

	trap->len = sizeof(trap->data);
	send_trap(ibp, trap);
}

/*
 * Send a System Image GUID Changed trap (ch. 14.3.12).
 */
void hfi2_sys_guid_chg(struct hfi2_ibport *ibp)
{
	struct trap_node *trap;
	u32 lid = ppd_from_ibp(ibp)->lid;

	trap = create_trap_node(IB_NOTICE_TYPE_INFO, OPA_TRAP_CHANGE_SYSGUID,
				lid);
	if (!trap)
		return;

	trap->data.ntc_145.new_sys_guid = hfi2_ib_hfi2_sys_image_guid;
	trap->data.ntc_145.lid = trap->data.issuer_lid;

	trap->len = sizeof(trap->data);
	send_trap(ibp, trap);
}

/*
 * Send a Node Description Changed trap (ch. 14.3.13).
 */
void hfi2_node_desc_chg(struct hfi2_ibport *ibp)
{
	struct trap_node *trap;
	u32 lid = ppd_from_ibp(ibp)->lid;

	trap = create_trap_node(IB_NOTICE_TYPE_INFO, OPA_TRAP_CHANGE_CAPABILITY,
				lid);
	if (!trap)
		return;

	trap->data.ntc_144.lid = trap->data.issuer_lid;
	trap->data.ntc_144.change_flags =
		cpu_to_be16(OPA_NOTICE_TRAP_NODE_DESC_CHG);

	trap->len = sizeof(trap->data);
	send_trap(ibp, trap);
}

static int __subn_get_opa_nodedesc(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct opa_node_description *nd;

	if (am || smp_length_check(sizeof(*nd), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	nd = (struct opa_node_description *)data;

	memcpy(nd->data, ibdev->node_desc, sizeof(nd->data));

	if (resp_len)
		*resp_len += sizeof(*nd);

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_get_opa_nodeinfo(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct opa_node_info *ni;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	u32 pidx = port - 1; /* IB number port from 1, hw from 0 */

	ni = (struct opa_node_info *)data;

	/* GUID 0 is illegal */
	if (am || pidx >= dd->num_pports || ibdev->node_guid == 0 ||
	    smp_length_check(sizeof(*ni), max_len) ||
	    get_sguid(to_iport(ibdev, port), HFI2_PORT_GUID_INDEX) == 0) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	ni->port_guid = get_sguid(to_iport(ibdev, port), HFI2_PORT_GUID_INDEX);
	ni->base_version = OPA_MGMT_BASE_VERSION;
	ni->class_version = OPA_SM_CLASS_VERSION;
	ni->node_type = 1; /* channel adapter */
	ni->num_ports = ibdev->phys_port_cnt;
	/* This is already in network order */
	ni->system_image_guid = hfi2_ib_hfi2_sys_image_guid;
	ni->node_guid = ibdev->node_guid;
	ni->partition_cap = cpu_to_be16(hfi2_get_npkeys(dd));
	ni->device_id = cpu_to_be16(dd->pcidev->device);
	ni->revision = cpu_to_be32(dd->minrev);
	ni->local_port_num = port;
	ni->vendor_id[0] = dd->oui1;
	ni->vendor_id[1] = dd->oui2;
	ni->vendor_id[2] = dd->oui3;

	if (resp_len)
		*resp_len += sizeof(*ni);

	return reply((struct ib_mad_hdr *)smp);
}

static int subn_get_nodeinfo(struct ib_smp *smp, struct ib_device *ibdev,
			     u32 port)
{
	struct ib_node_info *nip = (struct ib_node_info *)&smp->data;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	u32 pidx = port - 1; /* IB number port from 1, hw from 0 */

	/* GUID 0 is illegal */
	if (smp->attr_mod || pidx >= dd->num_pports || ibdev->node_guid == 0 ||
	    get_sguid(to_iport(ibdev, port), HFI2_PORT_GUID_INDEX) == 0) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	nip->port_guid = get_sguid(to_iport(ibdev, port), HFI2_PORT_GUID_INDEX);
	nip->base_version = OPA_MGMT_BASE_VERSION;
	nip->class_version = OPA_SM_CLASS_VERSION;
	nip->node_type = 1; /* channel adapter */
	nip->num_ports = ibdev->phys_port_cnt;
	/* This is already in network order */
	nip->sys_guid = hfi2_ib_hfi2_sys_image_guid;
	nip->node_guid = ibdev->node_guid;
	nip->partition_cap = cpu_to_be16(hfi2_get_npkeys(dd));
	nip->device_id = cpu_to_be16(dd->pcidev->device);
	nip->revision = cpu_to_be32(dd->minrev);
	nip->local_port_num = port;
	nip->vendor_id[0] = dd->oui1;
	nip->vendor_id[1] = dd->oui2;
	nip->vendor_id[2] = dd->oui3;

	return reply((struct ib_mad_hdr *)smp);
}

static void set_link_width_enabled(struct hfi2_pportdata *ppd, u32 w)
{
	(void)hfi2_set_ib_cfg(ppd, HFI2_IB_CFG_LWID_ENB, w);
}

static void set_link_width_downgrade_enabled(struct hfi2_pportdata *ppd, u32 w)
{
	(void)hfi2_set_ib_cfg(ppd, HFI2_IB_CFG_LWID_DG_ENB, w);
}

static void set_link_speed_enabled(struct hfi2_pportdata *ppd, u32 s)
{
	(void)hfi2_set_ib_cfg(ppd, HFI2_IB_CFG_SPD_ENB, s);
}

static int check_mkey(struct hfi2_ibport *ibp, struct ib_mad_hdr *mad,
		      int mad_flags, __be64 mkey, __be32 dr_slid,
		      u8 return_path[], u8 hop_cnt)
{
	int valid_mkey = 0;
	int ret = 0;

	/* Is the mkey in the process of expiring? */
	if (ibp->rvp.mkey_lease_timeout &&
	    time_after_eq(jiffies, ibp->rvp.mkey_lease_timeout)) {
		/* Clear timeout and mkey protection field. */
		ibp->rvp.mkey_lease_timeout = 0;
		ibp->rvp.mkeyprot = 0;
	}

	if ((mad_flags & IB_MAD_IGNORE_MKEY) || ibp->rvp.mkey == 0 ||
	    ibp->rvp.mkey == mkey)
		valid_mkey = 1;

	/* Unset lease timeout on any valid Get/Set/TrapRepress */
	if (valid_mkey && ibp->rvp.mkey_lease_timeout &&
	    (mad->method == IB_MGMT_METHOD_GET ||
	     mad->method == IB_MGMT_METHOD_SET ||
	     mad->method == IB_MGMT_METHOD_TRAP_REPRESS))
		ibp->rvp.mkey_lease_timeout = 0;

	if (!valid_mkey) {
		switch (mad->method) {
		case IB_MGMT_METHOD_GET:
			/* Bad mkey not a violation below level 2 */
			if (ibp->rvp.mkeyprot < 2)
				break;
			fallthrough;
		case IB_MGMT_METHOD_SET:
		case IB_MGMT_METHOD_TRAP_REPRESS:
			if (ibp->rvp.mkey_violations != 0xFFFF)
				++ibp->rvp.mkey_violations;
			if (!ibp->rvp.mkey_lease_timeout &&
			    ibp->rvp.mkey_lease_period)
				ibp->rvp.mkey_lease_timeout =
					jiffies +
					ibp->rvp.mkey_lease_period * HZ;
			/* Generate a trap notice. */
			bad_mkey(ibp, mad, mkey, dr_slid, return_path, hop_cnt);
			ret = 1;
		}
	}

	return ret;
}

static int __subn_get_opa_portinfo(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	int i;
	struct hfi2_devdata *dd;
	struct hfi2_pportdata *ppd;
	struct hfi2_ibport *ibp;
	struct opa_port_info *pi = (struct opa_port_info *)data;
	u8 mtu;
	u8 credit_rate;
	u8 is_beaconing_active;
	u32 state;
	u32 num_ports = OPA_AM_NPORT(am);
	u32 start_of_sm_config = OPA_AM_START_SM_CFG(am);
	u32 buffer_units;
	u64 tmp = 0;

	if (num_ports != 1 || smp_length_check(sizeof(*pi), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	dd = dd_from_ibdev(ibdev);
	/* IB numbers ports from 1, hw from 0 */
	ppd = dd->pport + (port - 1);
	ibp = &ppd->ibport_data;

	if (ppd->vls_supported / 2 > ARRAY_SIZE(pi->neigh_mtu.pvlx_to_mtu) ||
	    ppd->vls_supported > ARRAY_SIZE(ppd->vld)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	pi->lid = cpu_to_be32(ppd->lid);

	/* Only return the mkey if the protection field allows it. */
	if (!(smp->method == IB_MGMT_METHOD_GET && ibp->rvp.mkey != smp->mkey &&
	      ibp->rvp.mkeyprot == 1))
		pi->mkey = ibp->rvp.mkey;

	pi->subnet_prefix = ibp->rvp.gid_prefix;
	pi->sm_lid = cpu_to_be32(ibp->rvp.sm_lid);
	pi->ib_cap_mask = cpu_to_be32(ibp->rvp.port_cap_flags);
	pi->mkey_lease_period = cpu_to_be16(ibp->rvp.mkey_lease_period);
	pi->sm_trap_qp = cpu_to_be32(ppd->sm_trap_qp);
	pi->sa_qp = cpu_to_be32(ppd->sa_qp);

	pi->link_width.enabled = cpu_to_be16(ppd->link_width_enabled);
	pi->link_width.supported = cpu_to_be16(ppd->link_width_supported);
	pi->link_width.active = cpu_to_be16(ppd->link_width_active);

	pi->link_width_downgrade.supported =
		cpu_to_be16(ppd->link_width_downgrade_supported);
	pi->link_width_downgrade.enabled =
		cpu_to_be16(ppd->link_width_downgrade_enabled);
	pi->link_width_downgrade.tx_active =
		cpu_to_be16(ppd->link_width_downgrade_tx_active);
	pi->link_width_downgrade.rx_active =
		cpu_to_be16(ppd->link_width_downgrade_rx_active);

	pi->link_speed.supported = cpu_to_be16(ppd->link_speed_supported);
	pi->link_speed.active = cpu_to_be16(ppd->link_speed_active);
	pi->link_speed.enabled = cpu_to_be16(ppd->link_speed_enabled);

	state = hfi2_driver_lstate(ppd);

	if (start_of_sm_config && (state == IB_PORT_INIT))
		ppd->is_sm_config_started = 1;

	pi->port_phys_conf = (ppd->port_type & 0xf);

	pi->port_states.ledenable_offlinereason = ppd->neighbor_normal << 4;
	pi->port_states.ledenable_offlinereason |= ppd->is_sm_config_started
						   << 5;
	/*
	 * This pairs with the memory barrier in hfi2_start_led_override to
	 * ensure that we read the correct state of LED beaconing represented
	 * by led_override_timer_active
	 */
	smp_rmb();
	is_beaconing_active = !!atomic_read(&ppd->led_override_timer_active);
	pi->port_states.ledenable_offlinereason |= is_beaconing_active << 6;
	pi->port_states.ledenable_offlinereason |= ppd->offline_disabled_reason;

	pi->port_states.portphysstate_portstate =
		(hfi2_driver_pstate(ppd) << 4) | state;

	pi->mkeyprotect_lmc = (ibp->rvp.mkeyprot << 6) | ppd->lmc;

	memset(pi->neigh_mtu.pvlx_to_mtu, 0, sizeof(pi->neigh_mtu.pvlx_to_mtu));
	for (i = 0; i < ppd->vls_supported; i++) {
		mtu = hfi2_mtu_to_enum(ppd->vld[i].mtu,
				       HFI2_DEFAULT_ACTIVE_MTU);
		if ((i % 2) == 0)
			pi->neigh_mtu.pvlx_to_mtu[i / 2] |= (mtu << 4);
		else
			pi->neigh_mtu.pvlx_to_mtu[i / 2] |= mtu;
	}
	/* don't forget VL 15 */
	mtu = hfi2_mtu_to_enum(ppd->vld[15].mtu, 2048);
	pi->neigh_mtu.pvlx_to_mtu[15 / 2] |= mtu;
	pi->smsl = ibp->rvp.sm_sl & OPA_PI_MASK_SMSL;
	pi->operational_vls = hfi2_get_ib_cfg(ppd, HFI2_IB_CFG_OP_VLS);
	pi->partenforce_filterraw |=
		(ppd->linkinit_reason & OPA_PI_MASK_LINKINIT_REASON);
	if (ppd->part_enforce & HFI2_PART_ENFORCE_IN)
		pi->partenforce_filterraw |= OPA_PI_MASK_PARTITION_ENFORCE_IN;
	if (ppd->part_enforce & HFI2_PART_ENFORCE_OUT)
		pi->partenforce_filterraw |= OPA_PI_MASK_PARTITION_ENFORCE_OUT;
	pi->mkey_violations = cpu_to_be16(ibp->rvp.mkey_violations);
	/* P_KeyViolations are counted by hardware. */
	pi->pkey_violations = cpu_to_be16(ibp->rvp.pkey_violations);
	pi->qkey_violations = cpu_to_be16(ibp->rvp.qkey_violations);

	pi->vl.cap = ppd->vls_supported;
	pi->vl.high_limit = cpu_to_be16(ibp->rvp.vl_high_limit);
	pi->vl.arb_high_cap = (u8)hfi2_get_ib_cfg(ppd, HFI2_IB_CFG_VL_HIGH_CAP);
	pi->vl.arb_low_cap = (u8)hfi2_get_ib_cfg(ppd, HFI2_IB_CFG_VL_LOW_CAP);

	pi->clientrereg_subnettimeout = ibp->rvp.subnet_timeout;

	pi->port_link_mode = cpu_to_be16(OPA_PORT_LINK_MODE_OPA << 10 |
					 OPA_PORT_LINK_MODE_OPA << 5 |
					 OPA_PORT_LINK_MODE_OPA);

	pi->port_ltp_crc_mode = cpu_to_be16(ppd->port_ltp_crc_mode);

	pi->port_mode = cpu_to_be16(ppd->is_active_optimize_enabled ?
					    OPA_PI_MASK_PORT_ACTIVE_OPTOMIZE :
					    0);

	pi->port_packet_format.supported = cpu_to_be16(
		OPA_PORT_PACKET_FORMAT_9B | OPA_PORT_PACKET_FORMAT_16B);
	pi->port_packet_format.enabled = cpu_to_be16(
		OPA_PORT_PACKET_FORMAT_9B | OPA_PORT_PACKET_FORMAT_16B);

	/* flit_control.interleave is (OPA V1, version .76):
	 * bits		use
	 * ----		---
	 * 2		res
	 * 2		DistanceSupported
	 * 2		DistanceEnabled
	 * 5		MaxNextLevelTxEnabled
	 * 5		MaxNestLevelRxSupported
	 *
	 * HFI supports only "distance mode 1" (see OPA V1, version .76,
	 * section 9.6.2), so set DistanceSupported, DistanceEnabled
	 * to 0x1.
	 */
	pi->flit_control.interleave = cpu_to_be16(0x1400);

	pi->link_down_reason = ppd->local_link_down_reason.sma;
	pi->neigh_link_down_reason = ppd->neigh_link_down_reason.sma;
	pi->port_error_action = cpu_to_be32(ppd->port_error_action);
	pi->mtucap = hfi2_mtu_to_enum(hfi2_max_mtu, IB_MTU_4096);

	/* 32.768 usec. response time (guessing) */
	pi->resptimevalue = 3;

	pi->local_port_num = port;

	/* buffer info for FM */
	pi->overall_buffer_space = cpu_to_be16(dd->link_credits);

	pi->neigh_node_guid = cpu_to_be64(ppd->neighbor_guid);
	pi->neigh_port_num = ppd->neighbor_port_number;
	pi->port_neigh_mode =
		(ppd->neighbor_type & OPA_PI_MASK_NEIGH_NODE_TYPE) |
		(ppd->mgmt_allowed ? OPA_PI_MASK_NEIGH_MGMT_ALLOWED : 0) |
		(ppd->neighbor_fm_security ? OPA_PI_MASK_NEIGH_FW_AUTH_BYPASS :
					     0);

	/* HFIs shall always return VL15 credits to their
	 * neighbor in a timely manner, without any credit return pacing.
	 */
	credit_rate = 0;
	buffer_units = (dd->vau) & OPA_PI_MASK_BUF_UNIT_BUF_ALLOC;
	buffer_units |= (dd->vcu << 3) & OPA_PI_MASK_BUF_UNIT_CREDIT_ACK;
	buffer_units |= (credit_rate << 6) &
			OPA_PI_MASK_BUF_UNIT_VL15_CREDIT_RATE;
	buffer_units |= (dd->vl15_init << 11) & OPA_PI_MASK_BUF_UNIT_VL15_INIT;
	pi->buffer_units = cpu_to_be32(buffer_units);

	pi->opa_cap_mask = cpu_to_be16(ibp->rvp.port_cap3_flags);
	pi->collectivemask_multicastmask =
		((OPA_COLLECTIVE_NR & 0x7) << 3 | (OPA_MCAST_NR & 0x7));

	/* HFI supports a replay buffer 128 LTPs in size */
	pi->replay_depth.buffer = 0x80;
	/* use the cached round trip count */
	tmp = ppd->link_ltp_rtt;

	/*
	 * this counter is 16 bits wide, but the replay_depth.wire
	 * variable is only 8 bits
	 */
	if (tmp > 0xff)
		tmp = 0xff;
	pi->replay_depth.wire = tmp;

	if (resp_len)
		*resp_len += sizeof(struct opa_port_info);

	return reply((struct ib_mad_hdr *)smp);
}

/**
 * get_pkeys - return the PKEY table for any block
 * @dd: the hfi2_ib device
 * @port: the IB port number
 * @start_block: starting block to read
 * @num_blocks: number of blocks to read
 * @pkeys: the pkey table is placed here
 *
 * Copy out the pkeys - the keys are endianized for the host.
 */
static int get_pkeys(struct hfi2_devdata *dd, u32 port, u32 start_block,
		     u32 num_blocks, u16 *pkeys)
{
	struct hfi2_pportdata *ppd = dd->pport + (port - 1);
	u32 start_index;
	u32 end_index;

	/* calculate the indices affected */
	start_index = start_block * OPA_PARTITION_TABLE_BLK_SIZE;
	end_index = min_t(u32, dd->params->pkey_table_size,
			  start_index +
				  (num_blocks * OPA_PARTITION_TABLE_BLK_SIZE));

	memcpy(pkeys, &ppd->pkeys[start_index],
	       (end_index - start_index) * sizeof(ppd->pkeys[0]));

	return 0;
}

static int __subn_get_opa_pkeytable(struct opa_smp *smp, u32 am, u8 *data,
				    struct ib_device *ibdev, u32 port,
				    u32 *resp_len, u32 max_len)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	u32 n_blocks_req = OPA_AM_NBLK(am);
	u32 start_block = am & 0x7ff;
	__be16 *p;
	u16 *q;
	int i;
	u32 n_blocks_avail;
	u32 npkeys = hfi2_get_npkeys(dd);
	u32 num_req_keys;
	size_t size;

	if (n_blocks_req == 0) {
		pr_warn("OPA Get PKey AM Invalid : P = %d; B = 0x%x; N = 0x%x\n",
			port, start_block, n_blocks_req);
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	n_blocks_avail = DIV_ROUND_UP(npkeys, OPA_PARTITION_TABLE_BLK_SIZE);

	num_req_keys = n_blocks_req * OPA_PARTITION_TABLE_BLK_SIZE;
	size = num_req_keys * sizeof(u16);

	if (smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	if (start_block + n_blocks_req > n_blocks_avail ||
	    n_blocks_req > OPA_NUM_PKEY_BLOCKS_PER_SMP) {
		pr_warn("OPA Get PKey AM Invalid : s 0x%x; req 0x%x; "
			"avail 0x%x; blk/smp 0x%lx\n",
			start_block, n_blocks_req, n_blocks_avail,
			OPA_NUM_PKEY_BLOCKS_PER_SMP);
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	p = (__be16 *)data;
	q = (u16 *)data;
	/* get the current pkeys */
	get_pkeys(dd, port, start_block, n_blocks_req, q);
	/* make keys big endian */
	for (i = 0; i < num_req_keys; i++)
		p[i] = cpu_to_be16(q[i]);
	if (resp_len)
		*resp_len += size;

	return reply((struct ib_mad_hdr *)smp);
}

enum {
	HFI_TRANSITION_DISALLOWED,
	HFI_TRANSITION_IGNORED,
	HFI_TRANSITION_ALLOWED,
	HFI_TRANSITION_UNDEFINED,
};

/*
 * Use shortened names to improve readability of
 * {logical,physical}_state_transitions
 */
enum {
	__D = HFI_TRANSITION_DISALLOWED,
	__I = HFI_TRANSITION_IGNORED,
	__A = HFI_TRANSITION_ALLOWED,
	__U = HFI_TRANSITION_UNDEFINED,
};

/*
 * IB_PORTPHYSSTATE_POLLING (2) through OPA_PORTPHYSSTATE_MAX (11) are
 * represented in physical_state_transitions.
 */
#define __N_PHYSTATES (OPA_PORTPHYSSTATE_MAX - IB_PORTPHYSSTATE_POLLING + 1)

/*
 * Within physical_state_transitions, rows represent "old" states,
 * columns "new" states, and physical_state_transitions.allowed[old][new]
 * indicates if the transition from old state to new state is legal (see
 * OPAg1v1, Table 6-4).
 */
static const struct {
	u8 allowed[__N_PHYSTATES][__N_PHYSTATES];
} physical_state_transitions = { {
	/* 2    3    4    5    6    7    8    9   10   11 */
	/* 2 */ { __A, __A, __D, __D, __D, __D, __D, __D, __D, __D },
	/* 3 */ { __A, __I, __D, __D, __D, __D, __D, __D, __D, __A },
	/* 4 */ { __U, __U, __U, __U, __U, __U, __U, __U, __U, __U },
	/* 5 */ { __A, __A, __D, __I, __D, __D, __D, __D, __D, __D },
	/* 6 */ { __U, __U, __U, __U, __U, __U, __U, __U, __U, __U },
	/* 7 */ { __D, __A, __D, __D, __D, __I, __D, __D, __D, __D },
	/* 8 */ { __U, __U, __U, __U, __U, __U, __U, __U, __U, __U },
	/* 9 */ { __I, __A, __D, __D, __D, __D, __D, __I, __D, __D },
	/*10 */ { __U, __U, __U, __U, __U, __U, __U, __U, __U, __U },
	/*11 */ { __D, __A, __D, __D, __D, __D, __D, __D, __D, __I },
} };

/*
 * IB_PORT_DOWN (1) through IB_PORT_ACTIVE_DEFER (5) are represented
 * logical_state_transitions
 */

#define __N_LOGICAL_STATES (IB_PORT_ACTIVE_DEFER - IB_PORT_DOWN + 1)

/*
 * Within logical_state_transitions rows represent "old" states,
 * columns "new" states, and logical_state_transitions.allowed[old][new]
 * indicates if the transition from old state to new state is legal (see
 * OPAg1v1, Table 9-12).
 */
static const struct {
	u8 allowed[__N_LOGICAL_STATES][__N_LOGICAL_STATES];
} logical_state_transitions = { {
	/* 1    2    3    4    5 */
	/* 1 */ { __I, __D, __D, __D, __U },
	/* 2 */ { __D, __I, __A, __D, __U },
	/* 3 */ { __D, __D, __I, __A, __U },
	/* 4 */ { __D, __D, __I, __I, __U },
	/* 5 */ { __U, __U, __U, __U, __U },
} };

static int logical_transition_allowed(int old, int new)
{
	if (old < IB_PORT_NOP || old > IB_PORT_ACTIVE_DEFER ||
	    new < IB_PORT_NOP || new > IB_PORT_ACTIVE_DEFER) {
		pr_warn("invalid logical state(s) (old %d new %d)\n", old, new);
		return HFI_TRANSITION_UNDEFINED;
	}

	if (new == IB_PORT_NOP)
		return HFI_TRANSITION_ALLOWED; /* always allowed */

	/* adjust states for indexing into logical_state_transitions */
	old -= IB_PORT_DOWN;
	new -= IB_PORT_DOWN;

	if (old < 0 || new < 0)
		return HFI_TRANSITION_UNDEFINED;
	return logical_state_transitions.allowed[old][new];
}

static int physical_transition_allowed(int old, int new)
{
	if (old < IB_PORTPHYSSTATE_NOP || old > OPA_PORTPHYSSTATE_MAX ||
	    new < IB_PORTPHYSSTATE_NOP || new > OPA_PORTPHYSSTATE_MAX) {
		pr_warn("invalid physical state(s) (old %d new %d)\n", old,
			new);
		return HFI_TRANSITION_UNDEFINED;
	}

	if (new == IB_PORTPHYSSTATE_NOP)
		return HFI_TRANSITION_ALLOWED; /* always allowed */

	/* adjust states for indexing into physical_state_transitions */
	old -= IB_PORTPHYSSTATE_POLLING;
	new -= IB_PORTPHYSSTATE_POLLING;

	if (old < 0 || new < 0)
		return HFI_TRANSITION_UNDEFINED;
	return physical_state_transitions.allowed[old][new];
}

static int port_states_transition_allowed(struct hfi2_pportdata *ppd,
					  u32 logical_new, u32 physical_new)
{
	u32 physical_old = hfi2_driver_pstate(ppd);
	u32 logical_old = hfi2_driver_lstate(ppd);
	int ret, logical_allowed, physical_allowed;

	ret = logical_transition_allowed(logical_old, logical_new);
	logical_allowed = ret;

	if (ret == HFI_TRANSITION_DISALLOWED ||
	    ret == HFI_TRANSITION_UNDEFINED) {
		ppd_dev_warn(ppd, "invalid logical state transition %s -> %s\n",
			     ib_port_state_to_str(logical_old),
			     ib_port_state_to_str(logical_new));
		return ret;
	}

	ret = physical_transition_allowed(physical_old, physical_new);
	physical_allowed = ret;

	if (ret == HFI_TRANSITION_DISALLOWED ||
	    ret == HFI_TRANSITION_UNDEFINED) {
		ppd_dev_warn(ppd,
			     "invalid physical state transition %s -> %s\n",
			     hfi2_opa_pstate_name(physical_old),
			     hfi2_opa_pstate_name(physical_new));
		return ret;
	}

	if (logical_allowed == HFI_TRANSITION_IGNORED &&
	    physical_allowed == HFI_TRANSITION_IGNORED)
		return HFI_TRANSITION_IGNORED;

	/*
	 * A change request of Physical Port State from
	 * 'Offline' to 'Polling' should be ignored.
	 */
	if ((physical_old == OPA_PORTPHYSSTATE_OFFLINE) &&
	    (physical_new == IB_PORTPHYSSTATE_POLLING))
		return HFI_TRANSITION_IGNORED;

	/*
	 * Either physical_allowed or logical_allowed is
	 * HFI_TRANSITION_ALLOWED.
	 */
	return HFI_TRANSITION_ALLOWED;
}

static int set_port_states(struct hfi2_pportdata *ppd, struct opa_smp *smp,
			   u32 logical_state, u32 phys_state, int local_mad)
{
	struct hfi2_devdata *dd = ppd->dd;
	u32 link_state;
	int ret;

	ret = port_states_transition_allowed(ppd, logical_state, phys_state);
	if (ret == HFI_TRANSITION_DISALLOWED ||
	    ret == HFI_TRANSITION_UNDEFINED) {
		/* error message emitted above */
		smp->status |= IB_SMP_INVALID_FIELD;
		return 0;
	}

	if (ret == HFI_TRANSITION_IGNORED)
		return 0;

	if ((phys_state != IB_PORTPHYSSTATE_NOP) &&
	    !(logical_state == IB_PORT_DOWN || logical_state == IB_PORT_NOP)) {
		ppd_dev_warn(
			ppd,
			"SubnSet(OPA_PortInfo) port state invalid: logical_state 0x%x physical_state 0x%x\n",
			logical_state, phys_state);
		smp->status |= IB_SMP_INVALID_FIELD;
	}

	/*
	 * Logical state changes are summarized in OPAv1g1 spec.,
	 * Table 9-12; physical state changes are summarized in
	 * OPAv1g1 spec., Table 6.4.
	 */
	switch (logical_state) {
	case IB_PORT_NOP:
		if (phys_state == IB_PORTPHYSSTATE_NOP)
			break;
		fallthrough;
	case IB_PORT_DOWN:
		if (phys_state == IB_PORTPHYSSTATE_NOP) {
			link_state = HLS_DN_DOWNDEF;
		} else if (phys_state == IB_PORTPHYSSTATE_POLLING) {
			link_state = HLS_DN_POLL;
			hfi2_set_link_down_reason(
				ppd, OPA_LINKDOWN_REASON_FM_BOUNCE, 0,
				OPA_LINKDOWN_REASON_FM_BOUNCE);
		} else if (phys_state == IB_PORTPHYSSTATE_DISABLED) {
			link_state = HLS_DN_DISABLE;
		} else {
			ppd_dev_warn(
				ppd,
				"SubnSet(OPA_PortInfo) invalid physical state 0x%x\n",
				phys_state);
			smp->status |= IB_SMP_INVALID_FIELD;
			break;
		}

		if ((link_state == HLS_DN_POLL ||
		     link_state == HLS_DN_DOWNDEF)) {
			/*
			 * Going to poll.  No matter what the current state,
			 * always move offline first, then tune and start the
			 * link.  This correctly handles a FM link bounce and
			 * a link enable.  Going offline is a no-op if already
			 * offline.
			 */
			hfi2_set_link_state(ppd, HLS_DN_OFFLINE);
			hfi2_start_link(ppd);
		} else {
			hfi2_set_link_state(ppd, link_state);
		}
		if (link_state == HLS_DN_DISABLE &&
		    (ppd->offline_disabled_reason >
			     HFI2_ODR_MASK(OPA_LINKDOWN_REASON_SMA_DISABLED) ||
		     ppd->offline_disabled_reason ==
			     HFI2_ODR_MASK(OPA_LINKDOWN_REASON_NONE)))
			ppd->offline_disabled_reason =
				HFI2_ODR_MASK(OPA_LINKDOWN_REASON_SMA_DISABLED);
		/*
		 * Don't send a reply if the response would be sent
		 * through the disabled port.
		 */
		if (link_state == HLS_DN_DISABLE && !local_mad)
			return IB_MAD_RESULT_SUCCESS | IB_MAD_RESULT_CONSUMED;
		break;
	case IB_PORT_ARMED:
		ret = hfi2_set_link_state(ppd, HLS_UP_ARMED);
		if (!ret)
			hfi2_send_idle_sma(dd, SMA_IDLE_ARM);
		break;
	case IB_PORT_ACTIVE:
		if (ppd->neighbor_normal) {
			ret = hfi2_set_link_state(ppd, HLS_UP_ACTIVE);
			if (ret == 0)
				hfi2_send_idle_sma(dd, SMA_IDLE_ACTIVE);
		} else {
			ppd_dev_warn(
				ppd,
				"SubnSet(OPA_PortInfo) Cannot move to Active with NeighborNormal 0\n");
			smp->status |= IB_SMP_INVALID_FIELD;
		}
		break;
	default:
		ppd_dev_warn(
			ppd,
			"SubnSet(OPA_PortInfo) invalid logical state 0x%x\n",
			logical_state);
		smp->status |= IB_SMP_INVALID_FIELD;
	}

	return 0;
}

/*
 * Clone of set_port_states() that only updates device structures, does
 * not touch hardware. For adapters with a CPORT that handles the hardware.
 * The CPORT is never expected to send bad information.
 *
 * Note that intermediate states may be skipped, depending on how/when CPORT
 * notifies the driver.
 */
static void cport_set_port_states(struct hfi2_pportdata *ppd,
				  struct opa_port_info *pi, u32 logical_state,
				  u32 phys_state)
{
	u32 link_state;
	int ret;
	u32 cur_lstate;

	ret = port_states_transition_allowed(ppd, logical_state, phys_state);

	/*
	 * Logical state changes are summarized in OPAv1g1 spec.,
	 * Table 9-12; physical state changes are summarized in
	 * OPAv1g1 spec., Table 6.4.
	 */
	if (logical_state < IB_PORT_INIT) {
		/* this must be the new state, so do not ever skip processing */
		if (logical_state == IB_PORT_NOP &&
		    phys_state == IB_PORTPHYSSTATE_NOP) {
			link_state = HLS_DN_OFFLINE;
		} else if (phys_state == IB_PORTPHYSSTATE_NOP) {
			link_state = HLS_DN_DOWNDEF;
		} else if (phys_state == IB_PORTPHYSSTATE_POLLING) {
			link_state = HLS_DN_POLL;
			hfi2_set_link_down_reason(
				ppd, OPA_LINKDOWN_REASON_FM_BOUNCE, 0,
				OPA_LINKDOWN_REASON_FM_BOUNCE);
		} else if (phys_state == IB_PORTPHYSSTATE_DISABLED) {
			link_state = HLS_DN_DISABLE;
		} else {
			/* invalid - not possible? pick something */
			link_state = HLS_DN_DOWNDEF;
		}

		if ((link_state == HLS_DN_POLL ||
		     link_state == HLS_DN_DOWNDEF)) {
			/*
			 * Going to poll.  No matter what the current state,
			 * always move offline first, then tune and start the
			 * link.  This correctly handles a FM link bounce and
			 * a link enable.  Going offline is a no-op if already
			 * offline.
			 */
			hfi2_cport_set_link_state(ppd, pi, HLS_DN_OFFLINE);
			hfi2_cport_start_link(ppd, pi);
		} else {
			hfi2_cport_set_link_state(ppd, pi, link_state);
		}
		if (link_state == HLS_DN_DISABLE &&
		    (ppd->offline_disabled_reason >
			     HFI2_ODR_MASK(OPA_LINKDOWN_REASON_SMA_DISABLED) ||
		     ppd->offline_disabled_reason ==
			     HFI2_ODR_MASK(OPA_LINKDOWN_REASON_NONE)))
			ppd->offline_disabled_reason =
				HFI2_ODR_MASK(OPA_LINKDOWN_REASON_SMA_DISABLED);
		goto done;
	}
	cur_lstate = hfi2_driver_lstate(ppd);
	if (logical_state > IB_PORT_ACTIVE) {
		ppd_dev_warn(
			ppd,
			"%s: SubnSet(OPA_PortInfo) invalid logical state 0x%x\n",
			__func__, logical_state);
		goto done;
	}
	/* make certain all intermediate states are executed */
	if (logical_state < cur_lstate) {
		ppd_dev_warn(ppd, "%s: attempted move to lower LinkUp state\n",
			     __func__);
		goto done;
	}
	if (logical_state >= IB_PORT_INIT && cur_lstate < IB_PORT_INIT) {
		ret = hfi2_cport_set_link_state(ppd, pi, HLS_UP_INIT);
		if (ret)
			ppd_dev_warn(
				ppd,
				"%s: hfi2_cport_set_link_state INIT failed %d\n",
				__func__, ret);
	}
	if (logical_state >= IB_PORT_ARMED && cur_lstate < IB_PORT_ARMED) {
		ret = hfi2_cport_set_link_state(ppd, pi, HLS_UP_ARMED);
		if (ret)
			ppd_dev_warn(
				ppd,
				"%s: hfi2_cport_set_link_state ARMED failed %d\n",
				__func__, ret);
	}
	if (logical_state >= IB_PORT_ACTIVE && cur_lstate < IB_PORT_ACTIVE) {
		if (!ppd->neighbor_normal)
			ppd_dev_warn(
				ppd,
				"%s: Should not move to Active with NeighborNormal 0?\n",
				__func__);
		ret = hfi2_cport_set_link_state(ppd, pi, HLS_UP_ACTIVE);
		if (ret)
			ppd_dev_warn(
				ppd,
				"%s: hfi2_cport_set_link_state ACTIVE failed %d\n",
				__func__, ret);
	}
done:;
}

/*
 * subn_set_opa_portinfo - set port information
 * @smp: the incoming SM packet
 * @ibdev: the infiniband device
 * @port: the port on the device
 *
 */
static int __subn_set_opa_portinfo(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len, int local_mad)
{
	struct opa_port_info *pi = (struct opa_port_info *)data;
	struct ib_event event;
	struct hfi2_devdata *dd;
	struct hfi2_pportdata *ppd;
	struct hfi2_ibport *ibp;
	u8 clientrereg;
	unsigned long flags;
	u32 smlid;
	u32 lid;
	u8 ls_old, ls_new, ps_new;
	u8 vls;
	u8 msl;
	u8 crc_enabled;
	u16 lse, lwe, mtu;
	u32 num_ports = OPA_AM_NPORT(am);
	u32 start_of_sm_config = OPA_AM_START_SM_CFG(am);
	int ret, i, invalid = 0, call_set_mtu = 0;
	int call_link_downgrade_policy = 0;

	if (num_ports != 1 || smp_length_check(sizeof(*pi), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	lid = be32_to_cpu(pi->lid);
	if (lid & 0xFF000000) {
		pr_warn("OPA_PortInfo lid out of range: %X\n", lid);
		smp->status |= IB_SMP_INVALID_FIELD;
		goto get_only;
	}

	smlid = be32_to_cpu(pi->sm_lid);
	if (smlid & 0xFF000000) {
		pr_warn("OPA_PortInfo SM lid out of range: %X\n", smlid);
		smp->status |= IB_SMP_INVALID_FIELD;
		goto get_only;
	}

	clientrereg =
		(pi->clientrereg_subnettimeout & OPA_PI_MASK_CLIENT_REREGISTER);

	dd = dd_from_ibdev(ibdev);
	/* IB numbers ports from 1, hw from 0 */
	ppd = dd->pport + (port - 1);
	ibp = &ppd->ibport_data;
	event.device = ibdev;
	event.element.port_num = port;

	ls_old = hfi2_driver_lstate(ppd);

	ibp->rvp.mkey = pi->mkey;
	if (ibp->rvp.gid_prefix != pi->subnet_prefix) {
		ibp->rvp.gid_prefix = pi->subnet_prefix;
		event.event = IB_EVENT_GID_CHANGE;
		ib_dispatch_event(&event);
	}
	ibp->rvp.mkey_lease_period = be16_to_cpu(pi->mkey_lease_period);

	/* Must be a valid unicast LID address. */
	if ((lid == 0 && ls_old > IB_PORT_INIT) || (hfi2_is_16B_mcast(lid))) {
		smp->status |= IB_SMP_INVALID_FIELD;
		ppd_dev_warn(ppd, "SubnSet(OPA_PortInfo) lid invalid 0x%x\n",
			     lid);
	} else if (ppd->lid != lid ||
		   ppd->lmc != (pi->mkeyprotect_lmc & OPA_PI_MASK_LMC)) {
		if (ppd->lid != lid)
			hfi2_set_uevent_bits(ppd, _HFI2_EVENT_LID_CHANGE_BIT);
		if (ppd->lmc != (pi->mkeyprotect_lmc & OPA_PI_MASK_LMC))
			hfi2_set_uevent_bits(ppd, _HFI2_EVENT_LMC_CHANGE_BIT);
		hfi2_set_lid(ppd, lid, pi->mkeyprotect_lmc & OPA_PI_MASK_LMC);
		event.event = IB_EVENT_LID_CHANGE;
		ib_dispatch_event(&event);

		if (HFI2_PORT_GUID_INDEX + 1 < HFI2_GUIDS_PER_PORT) {
			/* Manufacture GID from LID to support extended
			 * addresses
			 */
			ppd->guids[HFI2_PORT_GUID_INDEX + 1] =
				be64_to_cpu(OPA_MAKE_ID(lid));
			event.event = IB_EVENT_GID_CHANGE;
			ib_dispatch_event(&event);
		}
	}

	msl = pi->smsl & OPA_PI_MASK_SMSL;
	if (pi->partenforce_filterraw & OPA_PI_MASK_LINKINIT_REASON)
		ppd->linkinit_reason = (pi->partenforce_filterraw &
					OPA_PI_MASK_LINKINIT_REASON);

	/* Must be a valid unicast LID address. */
	if ((smlid == 0 && ls_old > IB_PORT_INIT) ||
	    (hfi2_is_16B_mcast(smlid))) {
		smp->status |= IB_SMP_INVALID_FIELD;
		ppd_dev_warn(ppd, "SubnSet(OPA_PortInfo) smlid invalid 0x%x\n",
			     smlid);
	} else if (smlid != ibp->rvp.sm_lid || msl != ibp->rvp.sm_sl) {
		ppd_dev_warn(ppd, "SubnSet(OPA_PortInfo) smlid 0x%x\n", smlid);
		spin_lock_irqsave(&ibp->rvp.lock, flags);
		if (ibp->rvp.sm_ah) {
			if (smlid != ibp->rvp.sm_lid)
				hfi2_modify_qp0_ah(ibp, ibp->rvp.sm_ah, smlid);
			if (msl != ibp->rvp.sm_sl)
				rdma_ah_set_sl(&ibp->rvp.sm_ah->attr, msl);
		}
		spin_unlock_irqrestore(&ibp->rvp.lock, flags);
		if (smlid != ibp->rvp.sm_lid)
			ibp->rvp.sm_lid = smlid;
		if (msl != ibp->rvp.sm_sl)
			ibp->rvp.sm_sl = msl;
		event.event = IB_EVENT_SM_CHANGE;
		ib_dispatch_event(&event);
	}

	if (pi->link_down_reason == 0) {
		ppd->local_link_down_reason.sma = 0;
		ppd->local_link_down_reason.latest = 0;
	}

	if (pi->neigh_link_down_reason == 0) {
		ppd->neigh_link_down_reason.sma = 0;
		ppd->neigh_link_down_reason.latest = 0;
	}

	ppd->sm_trap_qp = be32_to_cpu(pi->sm_trap_qp);
	ppd->sa_qp = be32_to_cpu(pi->sa_qp);

	ppd->port_error_action = be32_to_cpu(pi->port_error_action);
	lwe = be16_to_cpu(pi->link_width.enabled);
	if (lwe) {
		if (lwe == OPA_LINK_WIDTH_RESET ||
		    lwe == OPA_LINK_WIDTH_RESET_OLD)
			set_link_width_enabled(ppd, ppd->link_width_supported);
		else if ((lwe & ~ppd->link_width_supported) == 0)
			set_link_width_enabled(ppd, lwe);
		else
			smp->status |= IB_SMP_INVALID_FIELD;
	}
	lwe = be16_to_cpu(pi->link_width_downgrade.enabled);
	/* LWD.E is always applied - 0 means "disabled" */
	if (lwe == OPA_LINK_WIDTH_RESET || lwe == OPA_LINK_WIDTH_RESET_OLD) {
		set_link_width_downgrade_enabled(
			ppd, ppd->link_width_downgrade_supported);
	} else if ((lwe & ~ppd->link_width_downgrade_supported) == 0) {
		/* only set and apply if something changed */
		if (lwe != ppd->link_width_downgrade_enabled) {
			set_link_width_downgrade_enabled(ppd, lwe);
			call_link_downgrade_policy = 1;
		}
	} else {
		smp->status |= IB_SMP_INVALID_FIELD;
	}
	lse = be16_to_cpu(pi->link_speed.enabled);
	if (lse) {
		if (lse & be16_to_cpu(pi->link_speed.supported))
			set_link_speed_enabled(ppd, lse);
		else
			smp->status |= IB_SMP_INVALID_FIELD;
	}

	ibp->rvp.mkeyprot = (pi->mkeyprotect_lmc & OPA_PI_MASK_MKEY_PROT_BIT) >>
			    6;
	ibp->rvp.vl_high_limit = be16_to_cpu(pi->vl.high_limit) & 0xFF;
	(void)hfi2_set_ib_cfg(ppd, HFI2_IB_CFG_VL_HIGH_LIMIT,
			      ibp->rvp.vl_high_limit);

	if (ppd->vls_supported / 2 > ARRAY_SIZE(pi->neigh_mtu.pvlx_to_mtu) ||
	    ppd->vls_supported > ARRAY_SIZE(ppd->vld)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}
	for (i = 0; i < ppd->vls_supported; i++) {
		if ((i % 2) == 0)
			mtu = hfi2_enum_to_mtu(
				(pi->neigh_mtu.pvlx_to_mtu[i / 2] >> 4) & 0xF);
		else
			mtu = hfi2_enum_to_mtu(
				pi->neigh_mtu.pvlx_to_mtu[i / 2] & 0xF);
		if (mtu == 0xffff) {
			ppd_dev_warn(
				ppd,
				"SubnSet(OPA_PortInfo) mtu invalid %d (0x%x)\n",
				mtu, (pi->neigh_mtu.pvlx_to_mtu[0] >> 4) & 0xF);
			smp->status |= IB_SMP_INVALID_FIELD;
			mtu = hfi2_max_mtu; /* use a valid MTU */
		}
		if (ppd->vld[i].mtu != mtu) {
			ppd_dev_info(ppd, "MTU change on vl %d from %d to %d\n",
				     i, ppd->vld[i].mtu, mtu);
			ppd->vld[i].mtu = mtu;
			call_set_mtu++;
		}
	}
	/* As per OPAV1 spec: VL15 must support and be configured
	 * for operation with a 2048 or larger MTU.
	 */
	mtu = hfi2_enum_to_mtu(pi->neigh_mtu.pvlx_to_mtu[15 / 2] & 0xF);
	if (mtu < 2048 || mtu == 0xffff)
		mtu = 2048;
	if (ppd->vld[15].mtu != mtu) {
		dd_dev_info(dd, "MTU change on vl 15 from %d to %d\n",
			    ppd->vld[15].mtu, mtu);
		ppd->vld[15].mtu = mtu;
		call_set_mtu++;
	}
	if (call_set_mtu)
		hfi2_set_mtu(ppd);

	/* Set operational VLs */
	vls = pi->operational_vls & OPA_PI_MASK_OPERATIONAL_VL;
	if (vls) {
		if (vls > ppd->vls_supported) {
			ppd_dev_warn(
				ppd,
				"SubnSet(OPA_PortInfo) VL's supported invalid %d\n",
				pi->operational_vls);
			smp->status |= IB_SMP_INVALID_FIELD;
		} else {
			if (hfi2_set_ib_cfg(ppd, HFI2_IB_CFG_OP_VLS, vls) ==
			    -EINVAL)
				smp->status |= IB_SMP_INVALID_FIELD;
		}
	}

	if (pi->mkey_violations == 0)
		ibp->rvp.mkey_violations = 0;

	if (pi->pkey_violations == 0)
		ibp->rvp.pkey_violations = 0;

	if (pi->qkey_violations == 0)
		ibp->rvp.qkey_violations = 0;

	ibp->rvp.subnet_timeout = pi->clientrereg_subnettimeout &
				  OPA_PI_MASK_SUBNET_TIMEOUT;

	crc_enabled = be16_to_cpu(pi->port_ltp_crc_mode);
	crc_enabled >>= 4;
	crc_enabled &= 0xf;

	if (crc_enabled != 0)
		ppd->port_crc_mode_enabled = hfi2_port_ltp_to_cap(crc_enabled);

	ppd->is_active_optimize_enabled = !!(be16_to_cpu(pi->port_mode) &
					     OPA_PI_MASK_PORT_ACTIVE_OPTOMIZE);

	ls_new = pi->port_states.portphysstate_portstate &
		 OPA_PI_MASK_PORT_STATE;
	ps_new = (pi->port_states.portphysstate_portstate &
		  OPA_PI_MASK_PORT_PHYSICAL_STATE) >>
		 4;

	if (ls_old == IB_PORT_INIT) {
		if (start_of_sm_config) {
			if (ls_new == ls_old || (ls_new == IB_PORT_ARMED))
				ppd->is_sm_config_started = 1;
		} else if (ls_new == IB_PORT_ARMED) {
			if (ppd->is_sm_config_started == 0) {
				invalid = 1;
				smp->status |= IB_SMP_INVALID_FIELD;
			}
		}
	}

	/* Handle CLIENT_REREGISTER event b/c SM asked us for it */
	if (clientrereg) {
		event.event = IB_EVENT_CLIENT_REREGISTER;
		ib_dispatch_event(&event);
	}

	/*
	 * Do the port state change now that the other link parameters
	 * have been set.
	 * Changing the port physical state only makes sense if the link
	 * is down or is being set to down.
	 */

	if (!invalid) {
		ret = set_port_states(ppd, smp, ls_new, ps_new, local_mad);
		if (ret)
			return ret;
	}

	ret = __subn_get_opa_portinfo(smp, am, data, ibdev, port, resp_len,
				      max_len);

	/* restore re-reg bit per o14-12.2.1 */
	pi->clientrereg_subnettimeout |= clientrereg;

	/*
	 * Apply the new link downgrade policy.  This may result in a link
	 * bounce.  Do this after everything else so things are settled.
	 * Possible problem: if setting the port state above fails, then
	 * the policy change is not applied.
	 */
	if (call_link_downgrade_policy)
		hfi2_apply_link_downgrade_policy(ppd, 0);

	return ret;

get_only:
	return __subn_get_opa_portinfo(smp, am, data, ibdev, port, resp_len,
				       max_len);
}

/**
 * set_pkeys - set the PKEY table for any block
 * @dd: the hfi2_ib device
 * @port: the IB port number
 * @start_block: starting block to modify
 * @num_blocks: number of blocks to modify
 * @pkeys: the PKEY table
 *
 * It is expected that the incoming pkeys are endianized for the host.
 * Return 0 on success, non-zero on error.
 */
static int set_pkeys(struct hfi2_devdata *dd, u32 port, u32 start_block,
		     u32 num_blocks, u16 *pkeys)
{
	struct hfi2_pportdata *ppd = dd->pport + (port - 1);
	int i;
	int changed = 0;
	int update_includes_mgmt_partition = 0;
	u32 start_index;
	u32 end_index;

	/* calculate the indices affected */
	start_index = start_block * OPA_PARTITION_TABLE_BLK_SIZE;
	end_index = min_t(u32, dd->params->pkey_table_size,
			  start_index +
				  (num_blocks * OPA_PARTITION_TABLE_BLK_SIZE));

	/*
	 * Block 0 must always contain the limited management key.
	 */
	if (start_block == 0) {
		int b0_end_index = min_t(int, OPA_PARTITION_TABLE_BLK_SIZE,
					 dd->params->pkey_table_size);

		for (i = 0; i < b0_end_index; i++) {
			if (pkeys[i] == LIM_MGMT_P_KEY) {
				update_includes_mgmt_partition = 1;
				break;
			}
		}

		if (!update_includes_mgmt_partition)
			return 1;
	}

	for (i = start_index; i < end_index; i++) {
		u16 key = pkeys[i - start_index];
		u16 okey = ppd->pkeys[i];

		if (key == okey)
			continue;
		/*
		 * The SM gives us the complete PKey table. We have
		 * to ensure that we put the PKeys in the matching
		 * slots.
		 */
		ppd->pkeys[i] = key;
		changed = 1;
	}

	if (changed) {
		(void)hfi2_set_ib_cfg(ppd, HFI2_IB_CFG_PKEYS, 0);
		hfi2_event_pkey_change(dd, port);
	}

	return 0;
}

static int __subn_set_opa_pkeytable(struct opa_smp *smp, u32 am, u8 *data,
				    struct ib_device *ibdev, u32 port,
				    u32 *resp_len, u32 max_len)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	u32 n_blocks_sent = OPA_AM_NBLK(am);
	u32 start_block = am & 0x7ff;
	u16 *p = (u16 *)data;
	__be16 *q = (__be16 *)data;
	int i;
	u32 n_blocks_avail;
	u32 npkeys = hfi2_get_npkeys(dd);
	u32 size = 0;

	if (n_blocks_sent == 0) {
		pr_warn("OPA Get PKey AM Invalid : P = %u; B = 0x%x; N = 0x%x\n",
			port, start_block, n_blocks_sent);
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	n_blocks_avail = DIV_ROUND_UP(npkeys, OPA_PARTITION_TABLE_BLK_SIZE);

	size = sizeof(u16) * (n_blocks_sent * OPA_PARTITION_TABLE_BLK_SIZE);

	if (smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	if (start_block + n_blocks_sent > n_blocks_avail ||
	    n_blocks_sent > OPA_NUM_PKEY_BLOCKS_PER_SMP) {
		pr_warn("OPA Set PKey AM Invalid : s 0x%x; req 0x%x; avail 0x%x; blk/smp 0x%lx\n",
			start_block, n_blocks_sent, n_blocks_avail,
			OPA_NUM_PKEY_BLOCKS_PER_SMP);
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	for (i = 0; i < n_blocks_sent * OPA_PARTITION_TABLE_BLK_SIZE; i++)
		p[i] = be16_to_cpu(q[i]);

	if (set_pkeys(dd, port, start_block, n_blocks_sent, p) != 0) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	return __subn_get_opa_pkeytable(smp, am, data, ibdev, port, resp_len,
					max_len);
}

#define ILLEGAL_VL 12
/*
 * filter_sc2vlt changes mappings to VL15 to ILLEGAL_VL (except
 * for SC15, which must map to VL15). If we don't remap things this
 * way it is possible for VL15 counters to increment when we try to
 * send on a SC which is mapped to an invalid VL.
 * When getting the table convert ILLEGAL_VL back to VL15.
 */
static void filter_sc2vlt(void *data, bool set)
{
	int i;
	u8 *pd = data;

	for (i = 0; i < OPA_MAX_SCS; i++) {
		if (i == 15)
			continue;

		if (set) {
			if ((pd[i] & 0x1f) == 0xf)
				pd[i] = ILLEGAL_VL;
		} else {
			if ((pd[i] & 0x1f) == ILLEGAL_VL)
				pd[i] = 0xf;
		}
	}
}

static int set_sc2vlt_tables(struct hfi2_pportdata *ppd, void *data)
{
	struct hfi2_devdata *dd = ppd->dd;
	u64 *val = data;
	int pidx = ppd->hw_pidx;

	filter_sc2vlt(data, true);

	write_eport_csr(dd, pidx, dd->params->send_sc2vlt0_reg, *val++);
	write_eport_csr(dd, pidx, dd->params->send_sc2vlt1_reg, *val++);
	write_eport_csr(dd, pidx, dd->params->send_sc2vlt2_reg, *val++);
	write_eport_csr(dd, pidx, dd->params->send_sc2vlt3_reg, *val++);
	write_seqlock_irq(&ppd->sc2vl_lock);
	memcpy(ppd->sc2vl, data, sizeof(ppd->sc2vl));
	write_sequnlock_irq(&ppd->sc2vl_lock);
	return 0;
}

int hfi2_get_sc2vlt_tables(struct hfi2_pportdata *ppd, void *data)
{
	struct hfi2_devdata *dd = ppd->dd;
	u64 *val = (u64 *)data;
	int pidx = ppd->hw_pidx;

	*val++ = read_eport_csr(dd, pidx, dd->params->send_sc2vlt0_reg);
	*val++ = read_eport_csr(dd, pidx, dd->params->send_sc2vlt1_reg);
	*val++ = read_eport_csr(dd, pidx, dd->params->send_sc2vlt2_reg);
	*val++ = read_eport_csr(dd, pidx, dd->params->send_sc2vlt3_reg);

	filter_sc2vlt((u64 *)data, false);
	return 0;
}

static int __subn_get_opa_sl_to_sc(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	u8 *p = data;
	size_t size = ARRAY_SIZE(ibp->sl_to_sc); /* == 32 */
	unsigned i;

	if (am || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	for (i = 0; i < ARRAY_SIZE(ibp->sl_to_sc); i++)
		*p++ = ibp->sl_to_sc[i];

	if (resp_len)
		*resp_len += size;

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_sl_to_sc(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	u8 *p = data;
	size_t size = ARRAY_SIZE(ibp->sl_to_sc);
	int i;
	u8 sc;

	if (am || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	for (i = 0; i < ARRAY_SIZE(ibp->sl_to_sc); i++) {
		sc = *p++;
		if (ibp->sl_to_sc[i] != sc) {
			ibp->sl_to_sc[i] = sc;

			/* Put all stale qps into error state */
			hfi2_error_port_qps(ibp, i);
		}
	}

	return __subn_get_opa_sl_to_sc(smp, am, data, ibdev, port, resp_len,
				       max_len);
}

static int __subn_get_opa_sc_to_sl(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	u8 *p = data;
	size_t size = ARRAY_SIZE(ibp->sc_to_sl); /* == 32 */
	unsigned i;

	if (am || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	for (i = 0; i < ARRAY_SIZE(ibp->sc_to_sl); i++)
		*p++ = ibp->sc_to_sl[i];

	if (resp_len)
		*resp_len += size;

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_sc_to_sl(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	size_t size = ARRAY_SIZE(ibp->sc_to_sl);
	u8 *p = data;
	int i;

	if (am || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	for (i = 0; i < ARRAY_SIZE(ibp->sc_to_sl); i++)
		ibp->sc_to_sl[i] = *p++;

	return __subn_get_opa_sc_to_sl(smp, am, data, ibdev, port, resp_len,
				       max_len);
}

static int __subn_get_opa_sc_to_vlt(struct opa_smp *smp, u32 am, u8 *data,
				    struct ib_device *ibdev, u32 port,
				    u32 *resp_len, u32 max_len)
{
	u32 n_blocks = OPA_AM_NBLK(am);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = &dd->pport[port - 1];
	void *vp = (void *)data;
	size_t size = 4 * sizeof(u64);

	if (n_blocks != 1 || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	hfi2_get_sc2vlt_tables(ppd, vp);

	if (resp_len)
		*resp_len += size;

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_sc_to_vlt(struct opa_smp *smp, u32 am, u8 *data,
				    struct ib_device *ibdev, u32 port,
				    u32 *resp_len, u32 max_len)
{
	u32 n_blocks = OPA_AM_NBLK(am);
	int async_update = OPA_AM_ASYNC(am);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	void *vp = (void *)data;
	struct hfi2_pportdata *ppd;
	int lstate;
	/*
	 * set_sc2vlt_tables writes the information contained in *data
	 * to four 64-bit registers SendSC2VLt[0-3]. We need to make
	 * sure *max_len is not greater than the total size of the four
	 * SendSC2VLt[0-3] registers.
	 */
	size_t size = 4 * sizeof(u64);

	if (n_blocks != 1 || async_update || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	/* IB numbers ports from 1, hw from 0 */
	ppd = dd->pport + (port - 1);
	lstate = hfi2_driver_lstate(ppd);
	/*
	 * it's known that async_update is 0 by this point, but include
	 * the explicit check for clarity
	 */
	if (!async_update &&
	    (lstate == IB_PORT_ARMED || lstate == IB_PORT_ACTIVE)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	set_sc2vlt_tables(ppd, vp);

	return __subn_get_opa_sc_to_vlt(smp, am, data, ibdev, port, resp_len,
					max_len);
}

static int __subn_get_opa_sc_to_vlnt(struct opa_smp *smp, u32 am, u8 *data,
				     struct ib_device *ibdev, u32 port,
				     u32 *resp_len, u32 max_len)
{
	u32 n_blocks = OPA_AM_NPORT(am);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd;
	void *vp = (void *)data;
	int size = sizeof(struct sc2vlnt);

	if (n_blocks != 1 || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	ppd = dd->pport + (port - 1);

	hfi2_fm_get_table(ppd, FM_TBL_SC2VLNT, vp);

	if (resp_len)
		*resp_len += size;

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_sc_to_vlnt(struct opa_smp *smp, u32 am, u8 *data,
				     struct ib_device *ibdev, u32 port,
				     u32 *resp_len, u32 max_len)
{
	u32 n_blocks = OPA_AM_NPORT(am);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd;
	void *vp = (void *)data;
	int lstate;
	int size = sizeof(struct sc2vlnt);

	if (n_blocks != 1 || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	/* IB numbers ports from 1, hw from 0 */
	ppd = dd->pport + (port - 1);
	lstate = hfi2_driver_lstate(ppd);
	if (lstate == IB_PORT_ARMED || lstate == IB_PORT_ACTIVE) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	ppd = dd->pport + (port - 1);

	hfi2_fm_set_table(ppd, FM_TBL_SC2VLNT, vp);

	return __subn_get_opa_sc_to_vlnt(smp, am, data, ibdev, port, resp_len,
					 max_len);
}

static int __subn_get_opa_psi(struct opa_smp *smp, u32 am, u8 *data,
			      struct ib_device *ibdev, u32 port, u32 *resp_len,
			      u32 max_len)
{
	u32 nports = OPA_AM_NPORT(am);
	u32 start_of_sm_config = OPA_AM_START_SM_CFG(am);
	u32 lstate;
	struct hfi2_ibport *ibp;
	struct hfi2_pportdata *ppd;
	struct opa_port_state_info *psi = (struct opa_port_state_info *)data;

	if (nports != 1 || smp_length_check(sizeof(*psi), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	ibp = to_iport(ibdev, port);
	ppd = ppd_from_ibp(ibp);

	lstate = hfi2_driver_lstate(ppd);

	if (start_of_sm_config && (lstate == IB_PORT_INIT))
		ppd->is_sm_config_started = 1;

	psi->port_states.ledenable_offlinereason = ppd->neighbor_normal << 4;
	psi->port_states.ledenable_offlinereason |= ppd->is_sm_config_started
						    << 5;
	psi->port_states.ledenable_offlinereason |=
		ppd->offline_disabled_reason;

	psi->port_states.portphysstate_portstate =
		(hfi2_driver_pstate(ppd) << 4) | (lstate & 0xf);
	psi->link_width_downgrade_tx_active =
		cpu_to_be16(ppd->link_width_downgrade_tx_active);
	psi->link_width_downgrade_rx_active =
		cpu_to_be16(ppd->link_width_downgrade_rx_active);
	if (resp_len)
		*resp_len += sizeof(struct opa_port_state_info);

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_psi(struct opa_smp *smp, u32 am, u8 *data,
			      struct ib_device *ibdev, u32 port, u32 *resp_len,
			      u32 max_len, int local_mad)
{
	u32 nports = OPA_AM_NPORT(am);
	u32 start_of_sm_config = OPA_AM_START_SM_CFG(am);
	u32 ls_old;
	u8 ls_new, ps_new;
	struct hfi2_ibport *ibp;
	struct hfi2_pportdata *ppd;
	struct opa_port_state_info *psi = (struct opa_port_state_info *)data;
	int ret, invalid = 0;

	if (nports != 1 || smp_length_check(sizeof(*psi), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	ibp = to_iport(ibdev, port);
	ppd = ppd_from_ibp(ibp);

	ls_old = hfi2_driver_lstate(ppd);

	ls_new = port_states_to_logical_state(&psi->port_states);
	ps_new = port_states_to_phys_state(&psi->port_states);

	if (ls_old == IB_PORT_INIT) {
		if (start_of_sm_config) {
			if (ls_new == ls_old || (ls_new == IB_PORT_ARMED))
				ppd->is_sm_config_started = 1;
		} else if (ls_new == IB_PORT_ARMED) {
			if (ppd->is_sm_config_started == 0) {
				invalid = 1;
				smp->status |= IB_SMP_INVALID_FIELD;
			}
		}
	}

	if (!invalid) {
		ret = set_port_states(ppd, smp, ls_new, ps_new, local_mad);
		if (ret)
			return ret;
	}

	return __subn_get_opa_psi(smp, am, data, ibdev, port, resp_len,
				  max_len);
}

static int __subn_get_opa_cable_info(struct opa_smp *smp, u32 am, u8 *data,
				     struct ib_device *ibdev, u32 port,
				     u32 *resp_len, u32 max_len)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = dd->pport + (port - 1);
	u32 addr = OPA_AM_CI_ADDR(am);
	u32 len = OPA_AM_CI_LEN(am) + 1;
	int ret;

	if (ppd->port_type != PORT_TYPE_QSFP ||
	    smp_length_check(len, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

#define __CI_PAGE_SIZE BIT(7) /* 128 bytes */
#define __CI_PAGE_MASK ~(__CI_PAGE_SIZE - 1)
#define __CI_PAGE_NUM(a) ((a) & __CI_PAGE_MASK)

	/*
	 * check that addr is within spec, and
	 * addr and (addr + len - 1) are on the same "page"
	 */
	if (addr >= 4096 ||
	    (__CI_PAGE_NUM(addr) != __CI_PAGE_NUM(addr + len - 1))) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	ret = hfi2_get_cable_info(ppd, addr, len, data);

	if (ret == -ENODEV) {
		smp->status |= IB_SMP_UNSUP_METH_ATTR;
		return reply((struct ib_mad_hdr *)smp);
	}

	/* The address range for the CableInfo SMA query is wider than the
	 * memory available on the QSFP cable. We want to return a valid
	 * response, albeit zeroed out, for address ranges beyond available
	 * memory but that are within the CableInfo query spec
	 */
	if (ret < 0 && ret != -ERANGE) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	if (resp_len)
		*resp_len += len;

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_get_opa_bct(struct opa_smp *smp, u32 am, u8 *data,
			      struct ib_device *ibdev, u32 port, u32 *resp_len,
			      u32 max_len)
{
	u32 num_ports = OPA_AM_NPORT(am);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd;
	struct buffer_control *p = (struct buffer_control *)data;
	int size = sizeof(struct buffer_control);

	if (num_ports != 1 || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	ppd = dd->pport + (port - 1);
	hfi2_fm_get_table(ppd, FM_TBL_BUFFER_CONTROL, p);
	trace_bct_get(dd, p);
	if (resp_len)
		*resp_len += size;

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_bct(struct opa_smp *smp, u32 am, u8 *data,
			      struct ib_device *ibdev, u32 port, u32 *resp_len,
			      u32 max_len)
{
	u32 num_ports = OPA_AM_NPORT(am);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd;
	struct buffer_control *p = (struct buffer_control *)data;

	if (num_ports != 1 || smp_length_check(sizeof(*p), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}
	ppd = dd->pport + (port - 1);
	trace_bct_set(dd, p);
	if (hfi2_fm_set_table(ppd, FM_TBL_BUFFER_CONTROL, p) < 0) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	return __subn_get_opa_bct(smp, am, data, ibdev, port, resp_len,
				  max_len);
}

static int __subn_get_opa_vl_arb(struct opa_smp *smp, u32 am, u8 *data,
				 struct ib_device *ibdev, u32 port,
				 u32 *resp_len, u32 max_len)
{
	struct hfi2_pportdata *ppd = ppd_from_ibp(to_iport(ibdev, port));
	u32 num_ports = OPA_AM_NPORT(am);
	u8 section = (am & 0x00ff0000) >> 16;
	u8 *p = data;
	int size = 256;

	if (num_ports != 1 || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	switch (section) {
	case OPA_VLARB_LOW_ELEMENTS:
		hfi2_fm_get_table(ppd, FM_TBL_VL_LOW_ARB, p);
		break;
	case OPA_VLARB_HIGH_ELEMENTS:
		hfi2_fm_get_table(ppd, FM_TBL_VL_HIGH_ARB, p);
		break;
	case OPA_VLARB_PREEMPT_ELEMENTS:
		hfi2_fm_get_table(ppd, FM_TBL_VL_PREEMPT_ELEMS, p);
		break;
	case OPA_VLARB_PREEMPT_MATRIX:
		hfi2_fm_get_table(ppd, FM_TBL_VL_PREEMPT_MATRIX, p);
		break;
	default:
		ppd_dev_warn(ppd, "OPA SubnGet(VL Arb) AM Invalid : 0x%x\n",
			     be32_to_cpu(smp->attr_mod));
		smp->status |= IB_SMP_INVALID_FIELD;
		size = 0;
		break;
	}

	if (size > 0 && resp_len)
		*resp_len += size;

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_vl_arb(struct opa_smp *smp, u32 am, u8 *data,
				 struct ib_device *ibdev, u32 port,
				 u32 *resp_len, u32 max_len)
{
	struct hfi2_pportdata *ppd = ppd_from_ibp(to_iport(ibdev, port));
	u32 num_ports = OPA_AM_NPORT(am);
	u8 section = (am & 0x00ff0000) >> 16;
	u8 *p = data;
	int size = 256;

	if (num_ports != 1 || smp_length_check(size, max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	switch (section) {
	case OPA_VLARB_LOW_ELEMENTS:
		(void)hfi2_fm_set_table(ppd, FM_TBL_VL_LOW_ARB, p);
		break;
	case OPA_VLARB_HIGH_ELEMENTS:
		(void)hfi2_fm_set_table(ppd, FM_TBL_VL_HIGH_ARB, p);
		break;
	/*
	 * neither OPA_VLARB_PREEMPT_ELEMENTS, or OPA_VLARB_PREEMPT_MATRIX
	 * can be changed from the default values
	 */
	case OPA_VLARB_PREEMPT_ELEMENTS:
	case OPA_VLARB_PREEMPT_MATRIX:
		smp->status |= IB_SMP_UNSUP_METH_ATTR;
		break;
	default:
		ppd_dev_warn(ppd, "OPA SubnSet(VL Arb) AM Invalid : 0x%x\n",
			     be32_to_cpu(smp->attr_mod));
		smp->status |= IB_SMP_INVALID_FIELD;
		break;
	}

	return __subn_get_opa_vl_arb(smp, am, data, ibdev, port, resp_len,
				     max_len);
}

struct opa_pma_mad {
	struct ib_mad_hdr mad_hdr;
	u8 data[2024];
} __packed;

struct opa_port_status_req {
	__u8 port_num;
	__u8 reserved[3];
	__be32 vl_select_mask;
};

#define VL_MASK_ALL 0x00000000000080ffUL

struct opa_port_status_rsp {
	__u8 port_num;
	__u8 reserved[3];
	__be32 vl_select_mask;

	/* Data counters */
	__be64 port_xmit_data;
	__be64 port_rcv_data;
	__be64 port_xmit_pkts;
	__be64 port_rcv_pkts;
	__be64 port_multicast_xmit_pkts;
	__be64 port_multicast_rcv_pkts;
	__be64 port_xmit_wait;
	__be64 sw_port_congestion;
	__be64 port_rcv_fecn;
	__be64 port_rcv_becn;
	__be64 port_xmit_time_cong;
	__be64 port_xmit_wasted_bw;
	__be64 port_xmit_wait_data;
	__be64 port_rcv_bubble;
	__be64 port_mark_fecn;
	/* Error counters */
	__be64 port_rcv_constraint_errors;
	__be64 port_rcv_switch_relay_errors;
	__be64 port_xmit_discards;
	__be64 port_xmit_constraint_errors;
	__be64 port_rcv_remote_physical_errors;
	__be64 local_link_integrity_errors;
	__be64 port_rcv_errors;
	__be64 excessive_buffer_overruns;
	__be64 fm_config_errors;
	__be32 link_error_recovery;
	__be32 link_downed;
	u8 uncorrectable_errors;

	u8 link_quality_indicator; /* 5res, 3bit */
	u8 res2[6];
	struct _vls_pctrs {
		/* per-VL Data counters */
		__be64 port_vl_xmit_data;
		__be64 port_vl_rcv_data;
		__be64 port_vl_xmit_pkts;
		__be64 port_vl_rcv_pkts;
		__be64 port_vl_xmit_wait;
		__be64 sw_port_vl_congestion;
		__be64 port_vl_rcv_fecn;
		__be64 port_vl_rcv_becn;
		__be64 port_xmit_time_cong;
		__be64 port_vl_xmit_wasted_bw;
		__be64 port_vl_xmit_wait_data;
		__be64 port_vl_rcv_bubble;
		__be64 port_vl_mark_fecn;
		__be64 port_vl_xmit_discards;
	} vls[]; /* real array size defined by # bits set in vl_select_mask */
};

enum counter_selects {
	CS_PORT_XMIT_DATA = (1 << 31),
	CS_PORT_RCV_DATA = (1 << 30),
	CS_PORT_XMIT_PKTS = (1 << 29),
	CS_PORT_RCV_PKTS = (1 << 28),
	CS_PORT_MCAST_XMIT_PKTS = (1 << 27),
	CS_PORT_MCAST_RCV_PKTS = (1 << 26),
	CS_PORT_XMIT_WAIT = (1 << 25),
	CS_SW_PORT_CONGESTION = (1 << 24),
	CS_PORT_RCV_FECN = (1 << 23),
	CS_PORT_RCV_BECN = (1 << 22),
	CS_PORT_XMIT_TIME_CONG = (1 << 21),
	CS_PORT_XMIT_WASTED_BW = (1 << 20),
	CS_PORT_XMIT_WAIT_DATA = (1 << 19),
	CS_PORT_RCV_BUBBLE = (1 << 18),
	CS_PORT_MARK_FECN = (1 << 17),
	CS_PORT_RCV_CONSTRAINT_ERRORS = (1 << 16),
	CS_PORT_RCV_SWITCH_RELAY_ERRORS = (1 << 15),
	CS_PORT_XMIT_DISCARDS = (1 << 14),
	CS_PORT_XMIT_CONSTRAINT_ERRORS = (1 << 13),
	CS_PORT_RCV_REMOTE_PHYSICAL_ERRORS = (1 << 12),
	CS_LOCAL_LINK_INTEGRITY_ERRORS = (1 << 11),
	CS_PORT_RCV_ERRORS = (1 << 10),
	CS_EXCESSIVE_BUFFER_OVERRUNS = (1 << 9),
	CS_FM_CONFIG_ERRORS = (1 << 8),
	CS_LINK_ERROR_RECOVERY = (1 << 7),
	CS_LINK_DOWNED = (1 << 6),
	CS_UNCORRECTABLE_ERRORS = (1 << 5),
};

struct opa_clear_port_status {
	__be64 port_select_mask[4];
	__be32 counter_select_mask;
};

struct opa_aggregate {
	__be16 attr_id;
	__be16 err_reqlength; /* 1 bit, 8 res, 7 bit */
	__be32 attr_mod;
	u8 data[];
};

#define MSK_LLI 0x000000f0
#define MSK_LLI_SFT 4
#define MSK_LER 0x0000000f
#define MSK_LER_SFT 0
#define ADD_LLI 8
#define ADD_LER 2

/* Request contains first three fields, response contains those plus the rest */
struct opa_port_data_counters_msg {
	__be64 port_select_mask[4];
	__be32 vl_select_mask;
	__be32 resolution;

	/* Response fields follow */
	struct _port_dctrs {
		u8 port_number;
		u8 reserved2[3];
		__be32 link_quality_indicator; /* 29res, 3bit */

		/* Data counters */
		__be64 port_xmit_data;
		__be64 port_rcv_data;
		__be64 port_xmit_pkts;
		__be64 port_rcv_pkts;
		__be64 port_multicast_xmit_pkts;
		__be64 port_multicast_rcv_pkts;
		__be64 port_xmit_wait;
		__be64 sw_port_congestion;
		__be64 port_rcv_fecn;
		__be64 port_rcv_becn;
		__be64 port_xmit_time_cong;
		__be64 port_xmit_wasted_bw;
		__be64 port_xmit_wait_data;
		__be64 port_rcv_bubble;
		__be64 port_mark_fecn;

		__be64 port_error_counter_summary;
		/* Sum of error counts/port */

		struct _vls_dctrs {
			/* per-VL Data counters */
			__be64 port_vl_xmit_data;
			__be64 port_vl_rcv_data;
			__be64 port_vl_xmit_pkts;
			__be64 port_vl_rcv_pkts;
			__be64 port_vl_xmit_wait;
			__be64 sw_port_vl_congestion;
			__be64 port_vl_rcv_fecn;
			__be64 port_vl_rcv_becn;
			__be64 port_xmit_time_cong;
			__be64 port_vl_xmit_wasted_bw;
			__be64 port_vl_xmit_wait_data;
			__be64 port_vl_rcv_bubble;
			__be64 port_vl_mark_fecn;
		} vls[];
		/* array size defined by #bits set in vl_select_mask*/
	} port;
};

struct opa_port_error_counters64_msg {
	/*
	 * Request contains first two fields, response contains the
	 * whole magilla
	 */
	__be64 port_select_mask[4];
	__be32 vl_select_mask;

	/* Response-only fields follow */
	__be32 reserved1;
	struct _port_ectrs {
		u8 port_number;
		u8 reserved2[7];
		__be64 port_rcv_constraint_errors;
		__be64 port_rcv_switch_relay_errors;
		__be64 port_xmit_discards;
		__be64 port_xmit_constraint_errors;
		__be64 port_rcv_remote_physical_errors;
		__be64 local_link_integrity_errors;
		__be64 port_rcv_errors;
		__be64 excessive_buffer_overruns;
		__be64 fm_config_errors;
		__be32 link_error_recovery;
		__be32 link_downed;
		u8 uncorrectable_errors;
		u8 reserved3[7];
		struct _vls_ectrs {
			__be64 port_vl_xmit_discards;
		} vls[];
		/* array size defined by #bits set in vl_select_mask */
	} port;
};

struct opa_port_error_info_msg {
	__be64 port_select_mask[4];
	__be32 error_info_select_mask;
	__be32 reserved1;
	struct _port_ei {
		u8 port_number;
		u8 reserved2[7];

		/* PortRcvErrorInfo */
		struct {
			u8 status_and_code;
			union {
				u8 raw[17];
				struct {
					/* EI1to12 format */
					u8 packet_flit1[8];
					u8 packet_flit2[8];
					u8 remaining_flit_bits12;
				} ei1to12;
				struct {
					u8 packet_bytes[8];
					u8 remaining_flit_bits;
				} ei13;
			} ei;
			u8 reserved3[6];
		} __packed port_rcv_ei;

		/* ExcessiveBufferOverrunInfo */
		struct {
			u8 status_and_sc;
			u8 reserved4[7];
		} __packed excessive_buffer_overrun_ei;

		/* PortXmitConstraintErrorInfo */
		struct {
			u8 status;
			u8 reserved5;
			__be16 pkey;
			__be32 slid;
		} __packed port_xmit_constraint_ei;

		/* PortRcvConstraintErrorInfo */
		struct {
			u8 status;
			u8 reserved6;
			__be16 pkey;
			__be32 slid;
		} __packed port_rcv_constraint_ei;

		/* PortRcvSwitchRelayErrorInfo */
		struct {
			u8 status_and_code;
			u8 reserved7[3];
			__u32 error_info;
		} __packed port_rcv_switch_relay_ei;

		/* UncorrectableErrorInfo */
		struct {
			u8 status_and_code;
			u8 reserved8;
		} __packed uncorrectable_ei;

		/* FMConfigErrorInfo */
		struct {
			u8 status_and_code;
			u8 error_info;
		} __packed fm_config_ei;
		__u32 reserved9;
	} port;
};

/* opa_port_error_info_msg error_info_select_mask bit definitions */
enum error_info_selects {
	ES_PORT_RCV_ERROR_INFO = (1 << 31),
	ES_EXCESSIVE_BUFFER_OVERRUN_INFO = (1 << 30),
	ES_PORT_XMIT_CONSTRAINT_ERROR_INFO = (1 << 29),
	ES_PORT_RCV_CONSTRAINT_ERROR_INFO = (1 << 28),
	ES_PORT_RCV_SWITCH_RELAY_ERROR_INFO = (1 << 27),
	ES_UNCORRECTABLE_ERROR_INFO = (1 << 26),
	ES_FM_CONFIG_ERROR_INFO = (1 << 25)
};

static int pma_get_opa_classportinfo(struct opa_pma_mad *pmp,
				     struct ib_device *ibdev, u32 *resp_len)
{
	struct opa_class_port_info *p = (struct opa_class_port_info *)pmp->data;

	memset(pmp->data, 0, sizeof(pmp->data));

	if (pmp->mad_hdr.attr_mod != 0)
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;

	p->base_version = OPA_MGMT_BASE_VERSION;
	p->class_version = OPA_SM_CLASS_VERSION;
	/*
	 * Expected response time is 4.096 usec. * 2^18 == 1.073741824 sec.
	 */
	p->cap_mask2_resp_time = cpu_to_be32(18);

	if (resp_len)
		*resp_len += sizeof(*p);

	return reply((struct ib_mad_hdr *)pmp);
}

static void a0_portstatus(struct hfi2_pportdata *ppd,
			  struct opa_port_status_rsp *rsp)
{
	if (!hfi2_is_bx(ppd->dd)) {
		unsigned long vl;
		u64 sum_vl_xmit_wait = 0;
		unsigned long vl_all_mask = VL_MASK_ALL;

		for_each_set_bit(vl, &vl_all_mask, BITS_PER_LONG) {
			u64 tmp = sum_vl_xmit_wait +
				  hfi2_read_port_cntr(ppd, C_TX_WAIT_VL,
						      idx_from_vl(vl));
			if (tmp < sum_vl_xmit_wait) {
				/* we wrapped */
				sum_vl_xmit_wait = (u64)~0;
				break;
			}
			sum_vl_xmit_wait = tmp;
		}
		if (be64_to_cpu(rsp->port_xmit_wait) > sum_vl_xmit_wait)
			rsp->port_xmit_wait = cpu_to_be64(sum_vl_xmit_wait);
	}
}

/**
 * hfi2_tx_link_width - convert link width bitmask to integer
 * value representing actual link width.
 * @link_width: width of active link
 * @return: return index of the bit set in link_width var
 *
 * The function convert and return the index of bit set
 * that indicate the current link width.
 */
u16 hfi2_tx_link_width(u16 link_width)
{
	int n = LINK_WIDTH_DEFAULT;
	u16 tx_width = n;

	while (link_width && n) {
		if (link_width & (1 << (n - 1))) {
			tx_width = n;
			break;
		}
		n--;
	}

	return tx_width;
}

/**
 * hfi2_get_xmit_wait_counters - Convert HFI 's SendWaitCnt/SendWaitVlCnt
 * counter in unit of TXE cycle times to flit times.
 * @ppd: info of physical Hfi port
 * @link_width: width of active link
 * @link_speed: speed of active link
 * @vl: represent VL0-VL7, VL15 for PortVLXmitWait counters request
 * and if vl value is C_VL_COUNT, it represent SendWaitCnt
 * counter request
 * @return: return SendWaitCnt/SendWaitVlCnt counter value per vl.
 *
 * Convert SendWaitCnt/SendWaitVlCnt counter from TXE cycle times to
 * flit times. Call this function to samples these counters. This
 * function will calculate for previous state transition and update
 * current state at end of function using ppd->prev_link_width and
 * ppd->port_vl_xmit_wait_last to port_vl_xmit_wait_curr and link_width.
 */
u64 hfi2_get_xmit_wait_counters(struct hfi2_pportdata *ppd, u16 link_width,
				u16 link_speed, int vl)
{
	u64 port_vl_xmit_wait_curr;
	u64 delta_vl_xmit_wait;
	u64 xmit_wait_val;

	if (vl > C_VL_COUNT)
		return 0;
	if (vl < C_VL_COUNT)
		port_vl_xmit_wait_curr =
			hfi2_read_port_cntr(ppd, C_TX_WAIT_VL, vl);
	else
		port_vl_xmit_wait_curr =
			hfi2_read_port_cntr(ppd, C_TX_WAIT, CNTR_INVALID_VL);

	xmit_wait_val =
		port_vl_xmit_wait_curr - ppd->port_vl_xmit_wait_last[vl];
	delta_vl_xmit_wait = convert_xmit_counter(
		xmit_wait_val, ppd->prev_link_width, link_speed);

	ppd->vl_xmit_flit_cnt[vl] += delta_vl_xmit_wait;
	ppd->port_vl_xmit_wait_last[vl] = port_vl_xmit_wait_curr;
	ppd->prev_link_width = link_width;

	return ppd->vl_xmit_flit_cnt[vl];
}

static int pma_get_opa_portstatus(struct opa_pma_mad *pmp,
				  struct ib_device *ibdev, u32 port,
				  u32 *resp_len)
{
	struct opa_port_status_req *req =
		(struct opa_port_status_req *)pmp->data;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct opa_port_status_rsp *rsp;
	unsigned long vl_select_mask = be32_to_cpu(req->vl_select_mask);
	unsigned long vl;
	size_t response_data_size;
	u32 nports = be32_to_cpu(pmp->mad_hdr.attr_mod) >> 24;
	u32 port_num = req->port_num;
	u8 hfi2_num_vls = hweight64(vl_select_mask);
	struct _vls_pctrs *vlinfo;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	int vfi;
	u64 tmp, tmp2;
	u16 link_width;
	u16 link_speed;

	response_data_size = struct_size(rsp, vls, hfi2_num_vls);
	if (response_data_size > sizeof(pmp->data)) {
		pmp->mad_hdr.status |= OPA_PM_STATUS_REQUEST_TOO_LARGE;
		return reply((struct ib_mad_hdr *)pmp);
	}

	if (nports != 1 || (port_num && port_num != port) ||
	    hfi2_num_vls > OPA_MAX_VLS || (vl_select_mask & ~VL_MASK_ALL)) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	memset(pmp->data, 0, sizeof(pmp->data));

	rsp = (struct opa_port_status_rsp *)pmp->data;
	if (port_num)
		rsp->port_num = port_num;
	else
		rsp->port_num = port;

	rsp->port_rcv_constraint_errors = cpu_to_be64(
		hfi2_read_port_cntr(ppd, C_SW_RCV_CSTR_ERR, CNTR_INVALID_VL));

	dd->params->read_link_quality(ppd, &rsp->link_quality_indicator);

	rsp->vl_select_mask = cpu_to_be32((u32)vl_select_mask);
	rsp->port_xmit_data = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_XMIT_FLITS, CNTR_INVALID_VL));
	rsp->port_rcv_data = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_FLITS, CNTR_INVALID_VL));
	rsp->port_xmit_pkts = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_XMIT_PKTS, CNTR_INVALID_VL));
	rsp->port_rcv_pkts = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_PKTS, CNTR_INVALID_VL));
	rsp->port_multicast_xmit_pkts = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_MC_XMIT_PKTS, CNTR_INVALID_VL));
	rsp->port_multicast_rcv_pkts = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_MC_RCV_PKTS, CNTR_INVALID_VL));
	/*
	 * Convert PortXmitWait counter from TXE cycle times
	 * to flit times.
	 */
	link_width = hfi2_tx_link_width(ppd->link_width_downgrade_tx_active);
	link_speed = get_link_speed(ppd->link_speed_active);
	rsp->port_xmit_wait = cpu_to_be64(hfi2_get_xmit_wait_counters(
		ppd, link_width, link_speed, C_VL_COUNT));
	rsp->port_rcv_fecn = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_FCN, CNTR_INVALID_VL));
	rsp->port_rcv_becn = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_BCN, CNTR_INVALID_VL));
	rsp->port_xmit_discards = cpu_to_be64(
		hfi2_read_port_cntr(ppd, C_SW_XMIT_DSCD, CNTR_INVALID_VL));
	rsp->port_xmit_constraint_errors = cpu_to_be64(
		hfi2_read_port_cntr(ppd, C_SW_XMIT_CSTR_ERR, CNTR_INVALID_VL));
	rsp->port_rcv_remote_physical_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RMT_PHY_ERR, CNTR_INVALID_VL));
	rsp->local_link_integrity_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RX_REPLAY, CNTR_INVALID_VL));
	tmp = hfi2_read_dev_cntr(dd, C_DC_SEQ_CRC_CNT, CNTR_INVALID_VL);
	tmp2 = tmp + hfi2_read_dev_cntr(dd, C_DC_REINIT_FROM_PEER_CNT,
					CNTR_INVALID_VL);
	if (tmp2 > (u32)UINT_MAX || tmp2 < tmp) {
		/* overflow/wrapped */
		rsp->link_error_recovery = cpu_to_be32(~0);
	} else {
		rsp->link_error_recovery = cpu_to_be32(tmp2);
	}
	rsp->port_rcv_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_ERR, CNTR_INVALID_VL));
	rsp->excessive_buffer_overruns = cpu_to_be64(
		hfi2_read_port_cntr(ppd, C_RCV_OVF, CNTR_INVALID_VL));
	rsp->fm_config_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_FM_CFG_ERR, CNTR_INVALID_VL));
	rsp->link_downed = cpu_to_be32(
		hfi2_read_port_cntr(ppd, C_SW_LINK_DOWN, CNTR_INVALID_VL));

	/* rsp->uncorrectable_errors is 8 bits wide, and it pegs at 0xff */
	tmp = hfi2_read_dev_cntr(dd, C_DC_UNC_ERR, CNTR_INVALID_VL);
	rsp->uncorrectable_errors = tmp < 0x100 ? (tmp & 0xff) : 0xff;

	vlinfo = &rsp->vls[0];
	vfi = 0;
	/* The vl_select_mask has been checked above, and we know
	 * that it contains only entries which represent valid VLs.
	 * So in the for_each_set_bit() loop below, we don't need
	 * any additional checks for vl.
	 */
	for_each_set_bit(vl, &vl_select_mask, BITS_PER_LONG) {
		memset(vlinfo, 0, sizeof(*vlinfo));

		tmp = hfi2_read_dev_cntr(dd, C_DC_RX_FLIT_VL, idx_from_vl(vl));
		rsp->vls[vfi].port_vl_rcv_data = cpu_to_be64(tmp);

		rsp->vls[vfi].port_vl_rcv_pkts = cpu_to_be64(hfi2_read_dev_cntr(
			dd, C_DC_RX_PKT_VL, idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_xmit_data =
			cpu_to_be64(hfi2_read_port_cntr(ppd, C_TX_FLIT_VL,
							idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_xmit_pkts = cpu_to_be64(
			hfi2_read_port_cntr(ppd, C_TX_PKT_VL, idx_from_vl(vl)));
		/*
		 * Convert PortVlXmitWait counter from TXE cycle
		 * times to flit times.
		 */
		rsp->vls[vfi].port_vl_xmit_wait = cpu_to_be64(
			hfi2_get_xmit_wait_counters(ppd, link_width, link_speed,
						    idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_rcv_fecn = cpu_to_be64(hfi2_read_dev_cntr(
			dd, C_DC_RCV_FCN_VL, idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_rcv_becn = cpu_to_be64(hfi2_read_dev_cntr(
			dd, C_DC_RCV_BCN_VL, idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_xmit_discards =
			cpu_to_be64(hfi2_read_port_cntr(ppd, C_SW_XMIT_DSCD_VL,
							idx_from_vl(vl)));
		vlinfo++;
		vfi++;
	}

	a0_portstatus(ppd, rsp);

	if (resp_len)
		*resp_len += response_data_size;

	return reply((struct ib_mad_hdr *)pmp);
}

static u64 get_error_counter_summary(struct ib_device *ibdev, u32 port,
				     u8 res_lli, u8 res_ler)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	u64 error_counter_summary = 0, tmp;

	error_counter_summary +=
		hfi2_read_port_cntr(ppd, C_SW_RCV_CSTR_ERR, CNTR_INVALID_VL);
	/* port_rcv_switch_relay_errors is 0 for HFIs */
	error_counter_summary +=
		hfi2_read_port_cntr(ppd, C_SW_XMIT_DSCD, CNTR_INVALID_VL);
	error_counter_summary +=
		hfi2_read_port_cntr(ppd, C_SW_XMIT_CSTR_ERR, CNTR_INVALID_VL);
	error_counter_summary +=
		hfi2_read_dev_cntr(dd, C_DC_RMT_PHY_ERR, CNTR_INVALID_VL);
	/* local link integrity must be right-shifted by the lli resolution */
	error_counter_summary +=
		(hfi2_read_dev_cntr(dd, C_DC_RX_REPLAY, CNTR_INVALID_VL) >>
		 res_lli);
	/* link error recovery must b right-shifted by the ler resolution */
	tmp = hfi2_read_dev_cntr(dd, C_DC_SEQ_CRC_CNT, CNTR_INVALID_VL);
	tmp += hfi2_read_dev_cntr(dd, C_DC_REINIT_FROM_PEER_CNT,
				  CNTR_INVALID_VL);
	error_counter_summary += (tmp >> res_ler);
	error_counter_summary +=
		hfi2_read_dev_cntr(dd, C_DC_RCV_ERR, CNTR_INVALID_VL);
	error_counter_summary +=
		hfi2_read_port_cntr(ppd, C_RCV_OVF, CNTR_INVALID_VL);
	error_counter_summary +=
		hfi2_read_dev_cntr(dd, C_DC_FM_CFG_ERR, CNTR_INVALID_VL);
	/* ppd->link_downed is a 32-bit value */
	error_counter_summary +=
		hfi2_read_port_cntr(ppd, C_SW_LINK_DOWN, CNTR_INVALID_VL);
	tmp = hfi2_read_dev_cntr(dd, C_DC_UNC_ERR, CNTR_INVALID_VL);
	/* this is an 8-bit quantity */
	error_counter_summary += tmp < 0x100 ? (tmp & 0xff) : 0xff;

	return error_counter_summary;
}

static void a0_datacounters(struct hfi2_pportdata *ppd, struct _port_dctrs *rsp)
{
	if (!hfi2_is_bx(ppd->dd)) {
		unsigned long vl;
		u64 sum_vl_xmit_wait = 0;
		unsigned long vl_all_mask = VL_MASK_ALL;

		for_each_set_bit(vl, &vl_all_mask, BITS_PER_LONG) {
			u64 tmp = sum_vl_xmit_wait +
				  hfi2_read_port_cntr(ppd, C_TX_WAIT_VL,
						      idx_from_vl(vl));
			if (tmp < sum_vl_xmit_wait) {
				/* we wrapped */
				sum_vl_xmit_wait = (u64)~0;
				break;
			}
			sum_vl_xmit_wait = tmp;
		}
		if (be64_to_cpu(rsp->port_xmit_wait) > sum_vl_xmit_wait)
			rsp->port_xmit_wait = cpu_to_be64(sum_vl_xmit_wait);
	}
}

static void pma_get_opa_port_dctrs(struct ib_device *ibdev,
				   struct _port_dctrs *rsp)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);

	rsp->port_xmit_data = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_XMIT_FLITS, CNTR_INVALID_VL));
	rsp->port_rcv_data = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_FLITS, CNTR_INVALID_VL));
	rsp->port_xmit_pkts = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_XMIT_PKTS, CNTR_INVALID_VL));
	rsp->port_rcv_pkts = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_PKTS, CNTR_INVALID_VL));
	rsp->port_multicast_xmit_pkts = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_MC_XMIT_PKTS, CNTR_INVALID_VL));
	rsp->port_multicast_rcv_pkts = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_MC_RCV_PKTS, CNTR_INVALID_VL));
}

static int pma_get_opa_datacounters(struct opa_pma_mad *pmp,
				    struct ib_device *ibdev, u32 port,
				    u32 *resp_len)
{
	struct opa_port_data_counters_msg *req =
		(struct opa_port_data_counters_msg *)pmp->data;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	struct _port_dctrs *rsp;
	struct _vls_dctrs *vlinfo;
	size_t response_data_size;
	u32 num_ports;
	u8 lq, hfi2_num_vls;
	u8 res_lli, res_ler;
	u64 port_mask;
	u32 port_num;
	unsigned long vl;
	unsigned long vl_select_mask;
	int vfi;
	u16 link_width;
	u16 link_speed;

	num_ports = be32_to_cpu(pmp->mad_hdr.attr_mod) >> 24;
	hfi2_num_vls = hweight32(be32_to_cpu(req->vl_select_mask));
	vl_select_mask = be32_to_cpu(req->vl_select_mask);
	res_lli = (u8)(be32_to_cpu(req->resolution) & MSK_LLI) >> MSK_LLI_SFT;
	res_lli = res_lli ? res_lli + ADD_LLI : 0;
	res_ler = (u8)(be32_to_cpu(req->resolution) & MSK_LER) >> MSK_LER_SFT;
	res_ler = res_ler ? res_ler + ADD_LER : 0;

	if (num_ports != 1 || (vl_select_mask & ~VL_MASK_ALL)) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	/* Sanity check */
	response_data_size = struct_size(req, port.vls, hfi2_num_vls);

	if (response_data_size > sizeof(pmp->data)) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	/*
	 * The bit set in the mask needs to be consistent with the
	 * port the request came in on.
	 */
	port_mask = be64_to_cpu(req->port_select_mask[3]);
	port_num = find_first_bit((unsigned long *)&port_mask,
				  sizeof(port_mask) * 8);

	if (port_num != port) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	rsp = &req->port;
	memset(rsp, 0, sizeof(*rsp));

	rsp->port_number = port;
	/*
	 * Note that link_quality_indicator is a 32 bit quantity in
	 * 'datacounters' queries (as opposed to 'portinfo' queries,
	 * where it's a byte).
	 */
	dd->params->read_link_quality(ppd, &lq);
	rsp->link_quality_indicator = cpu_to_be32((u32)lq);
	pma_get_opa_port_dctrs(ibdev, rsp);

	/*
	 * Convert PortXmitWait counter from TXE
	 * cycle times to flit times.
	 */
	link_width = hfi2_tx_link_width(ppd->link_width_downgrade_tx_active);
	link_speed = get_link_speed(ppd->link_speed_active);
	rsp->port_xmit_wait = cpu_to_be64(hfi2_get_xmit_wait_counters(
		ppd, link_width, link_speed, C_VL_COUNT));
	rsp->port_rcv_fecn = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_FCN, CNTR_INVALID_VL));
	rsp->port_rcv_becn = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_BCN, CNTR_INVALID_VL));
	rsp->port_error_counter_summary = cpu_to_be64(
		get_error_counter_summary(ibdev, port, res_lli, res_ler));

	vlinfo = &rsp->vls[0];
	vfi = 0;
	/* The vl_select_mask has been checked above, and we know
	 * that it contains only entries which represent valid VLs.
	 * So in the for_each_set_bit() loop below, we don't need
	 * any additional checks for vl.
	 */
	for_each_set_bit(vl, &vl_select_mask, BITS_PER_LONG) {
		memset(vlinfo, 0, sizeof(*vlinfo));

		rsp->vls[vfi].port_vl_xmit_data =
			cpu_to_be64(hfi2_read_port_cntr(ppd, C_TX_FLIT_VL,
							idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_rcv_data = cpu_to_be64(hfi2_read_dev_cntr(
			dd, C_DC_RX_FLIT_VL, idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_xmit_pkts = cpu_to_be64(
			hfi2_read_port_cntr(ppd, C_TX_PKT_VL, idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_rcv_pkts = cpu_to_be64(hfi2_read_dev_cntr(
			dd, C_DC_RX_PKT_VL, idx_from_vl(vl)));

		/*
		 * Convert PortVlXmitWait counter from TXE
		 * cycle times to flit times.
		 */
		rsp->vls[vfi].port_vl_xmit_wait = cpu_to_be64(
			hfi2_get_xmit_wait_counters(ppd, link_width, link_speed,
						    idx_from_vl(vl)));

		rsp->vls[vfi].port_vl_rcv_fecn = cpu_to_be64(hfi2_read_dev_cntr(
			dd, C_DC_RCV_FCN_VL, idx_from_vl(vl)));
		rsp->vls[vfi].port_vl_rcv_becn = cpu_to_be64(hfi2_read_dev_cntr(
			dd, C_DC_RCV_BCN_VL, idx_from_vl(vl)));

		/* rsp->port_vl_xmit_time_cong is 0 for HFIs */
		/* rsp->port_vl_xmit_wasted_bw ??? */
		/* port_vl_xmit_wait_data - TXE (table 13-9 HFI spec) ???
		 * does this differ from rsp->vls[vfi].port_vl_xmit_wait
		 */
		/*rsp->vls[vfi].port_vl_mark_fecn =
		 *	cpu_to_be64(hfi2_read_csr(dd, DCC_PRF_PORT_VL_MARK_FECN_CNT
		 *		+ offset));
		 */
		vlinfo++;
		vfi++;
	}

	a0_datacounters(ppd, rsp);

	if (resp_len)
		*resp_len += response_data_size;

	return reply((struct ib_mad_hdr *)pmp);
}

static int pma_get_ib_portcounters_ext(struct ib_pma_mad *pmp,
				       struct ib_device *ibdev, u32 port)
{
	struct ib_pma_portcounters_ext *p =
		(struct ib_pma_portcounters_ext *)pmp->data;
	struct _port_dctrs rsp;

	if (pmp->mad_hdr.attr_mod != 0 || p->port_select != port) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		goto bail;
	}

	memset(&rsp, 0, sizeof(rsp));
	pma_get_opa_port_dctrs(ibdev, &rsp);

	p->port_xmit_data = rsp.port_xmit_data;
	p->port_rcv_data = rsp.port_rcv_data;
	p->port_xmit_packets = rsp.port_xmit_pkts;
	p->port_rcv_packets = rsp.port_rcv_pkts;
	p->port_unicast_xmit_packets = 0;
	p->port_unicast_rcv_packets = 0;
	p->port_multicast_xmit_packets = rsp.port_multicast_xmit_pkts;
	p->port_multicast_rcv_packets = rsp.port_multicast_rcv_pkts;

bail:
	return reply((struct ib_mad_hdr *)pmp);
}

static void pma_get_opa_port_ectrs(struct ib_device *ibdev,
				   struct _port_ectrs *rsp, u32 port)
{
	u64 tmp, tmp2;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);

	tmp = hfi2_read_dev_cntr(dd, C_DC_SEQ_CRC_CNT, CNTR_INVALID_VL);
	tmp2 = tmp + hfi2_read_dev_cntr(dd, C_DC_REINIT_FROM_PEER_CNT,
					CNTR_INVALID_VL);
	if (tmp2 > (u32)UINT_MAX || tmp2 < tmp) {
		/* overflow/wrapped */
		rsp->link_error_recovery = cpu_to_be32(~0);
	} else {
		rsp->link_error_recovery = cpu_to_be32(tmp2);
	}

	rsp->link_downed = cpu_to_be32(
		hfi2_read_port_cntr(ppd, C_SW_LINK_DOWN, CNTR_INVALID_VL));
	rsp->port_rcv_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_ERR, CNTR_INVALID_VL));
	rsp->port_rcv_remote_physical_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RMT_PHY_ERR, CNTR_INVALID_VL));
	rsp->port_rcv_switch_relay_errors = 0;
	rsp->port_xmit_discards = cpu_to_be64(
		hfi2_read_port_cntr(ppd, C_SW_XMIT_DSCD, CNTR_INVALID_VL));
	rsp->port_xmit_constraint_errors = cpu_to_be64(
		hfi2_read_port_cntr(ppd, C_SW_XMIT_CSTR_ERR, CNTR_INVALID_VL));
	rsp->port_rcv_constraint_errors = cpu_to_be64(
		hfi2_read_port_cntr(ppd, C_SW_RCV_CSTR_ERR, CNTR_INVALID_VL));
	rsp->local_link_integrity_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RX_REPLAY, CNTR_INVALID_VL));
	rsp->excessive_buffer_overruns = cpu_to_be64(
		hfi2_read_port_cntr(ppd, C_RCV_OVF, CNTR_INVALID_VL));
}

static int pma_get_opa_porterrors(struct opa_pma_mad *pmp,
				  struct ib_device *ibdev, u32 port,
				  u32 *resp_len)
{
	size_t response_data_size;
	struct _port_ectrs *rsp;
	u32 port_num;
	struct opa_port_error_counters64_msg *req;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	u32 num_ports;
	u8 num_pslm;
	u8 hfi2_num_vls;
	struct hfi2_ibport *ibp;
	struct hfi2_pportdata *ppd;
	struct _vls_ectrs *vlinfo;
	unsigned long vl;
	u64 port_mask, tmp;
	unsigned long vl_select_mask;
	int vfi;

	req = (struct opa_port_error_counters64_msg *)pmp->data;

	num_ports = be32_to_cpu(pmp->mad_hdr.attr_mod) >> 24;

	num_pslm = hweight64(be64_to_cpu(req->port_select_mask[3]));
	hfi2_num_vls = hweight32(be32_to_cpu(req->vl_select_mask));

	if (num_ports != 1 || num_ports != num_pslm) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	response_data_size = struct_size(req, port.vls, hfi2_num_vls);

	if (response_data_size > sizeof(pmp->data)) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}
	/*
	 * The bit set in the mask needs to be consistent with the
	 * port the request came in on.
	 */
	port_mask = be64_to_cpu(req->port_select_mask[3]);
	port_num = find_first_bit((unsigned long *)&port_mask,
				  sizeof(port_mask) * 8);

	if (port_num != port) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	rsp = &req->port;

	ibp = to_iport(ibdev, port_num);
	ppd = ppd_from_ibp(ibp);

	memset(rsp, 0, sizeof(*rsp));
	rsp->port_number = port_num;

	pma_get_opa_port_ectrs(ibdev, rsp, port_num);

	rsp->port_rcv_remote_physical_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RMT_PHY_ERR, CNTR_INVALID_VL));
	rsp->fm_config_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_FM_CFG_ERR, CNTR_INVALID_VL));
	tmp = hfi2_read_dev_cntr(dd, C_DC_UNC_ERR, CNTR_INVALID_VL);

	rsp->uncorrectable_errors = tmp < 0x100 ? (tmp & 0xff) : 0xff;
	rsp->port_rcv_errors = cpu_to_be64(
		hfi2_read_dev_cntr(dd, C_DC_RCV_ERR, CNTR_INVALID_VL));
	vlinfo = &rsp->vls[0];
	vfi = 0;
	vl_select_mask = be32_to_cpu(req->vl_select_mask);
	for_each_set_bit(vl, &vl_select_mask, BITS_PER_LONG) {
		memset(vlinfo, 0, sizeof(*vlinfo));
		rsp->vls[vfi].port_vl_xmit_discards =
			cpu_to_be64(hfi2_read_port_cntr(ppd, C_SW_XMIT_DSCD_VL,
							idx_from_vl(vl)));
		vlinfo += 1;
		vfi++;
	}

	if (resp_len)
		*resp_len += response_data_size;

	return reply((struct ib_mad_hdr *)pmp);
}

static int pma_get_ib_portcounters(struct ib_pma_mad *pmp,
				   struct ib_device *ibdev, u32 port)
{
	struct ib_pma_portcounters *p = (struct ib_pma_portcounters *)pmp->data;
	struct _port_ectrs rsp;
	u64 temp_link_overrun_errors;
	u64 temp_64;
	u32 temp_32;

	memset(&rsp, 0, sizeof(rsp));
	pma_get_opa_port_ectrs(ibdev, &rsp, port);

	if (pmp->mad_hdr.attr_mod != 0 || p->port_select != port) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		goto bail;
	}

	p->symbol_error_counter = 0; /* N/A for OPA */

	temp_32 = be32_to_cpu(rsp.link_error_recovery);
	if (temp_32 > 0xFFUL)
		p->link_error_recovery_counter = 0xFF;
	else
		p->link_error_recovery_counter = (u8)temp_32;

	temp_32 = be32_to_cpu(rsp.link_downed);
	if (temp_32 > 0xFFUL)
		p->link_downed_counter = 0xFF;
	else
		p->link_downed_counter = (u8)temp_32;

	temp_64 = be64_to_cpu(rsp.port_rcv_errors);
	if (temp_64 > 0xFFFFUL)
		p->port_rcv_errors = cpu_to_be16(0xFFFF);
	else
		p->port_rcv_errors = cpu_to_be16((u16)temp_64);

	temp_64 = be64_to_cpu(rsp.port_rcv_remote_physical_errors);
	if (temp_64 > 0xFFFFUL)
		p->port_rcv_remphys_errors = cpu_to_be16(0xFFFF);
	else
		p->port_rcv_remphys_errors = cpu_to_be16((u16)temp_64);

	temp_64 = be64_to_cpu(rsp.port_rcv_switch_relay_errors);
	p->port_rcv_switch_relay_errors = cpu_to_be16((u16)temp_64);

	temp_64 = be64_to_cpu(rsp.port_xmit_discards);
	if (temp_64 > 0xFFFFUL)
		p->port_xmit_discards = cpu_to_be16(0xFFFF);
	else
		p->port_xmit_discards = cpu_to_be16((u16)temp_64);

	temp_64 = be64_to_cpu(rsp.port_xmit_constraint_errors);
	if (temp_64 > 0xFFUL)
		p->port_xmit_constraint_errors = 0xFF;
	else
		p->port_xmit_constraint_errors = (u8)temp_64;

	temp_64 = be64_to_cpu(rsp.port_rcv_constraint_errors);
	if (temp_64 > 0xFFUL)
		p->port_rcv_constraint_errors = 0xFFUL;
	else
		p->port_rcv_constraint_errors = (u8)temp_64;

	/* LocalLink: 7:4, BufferOverrun: 3:0 */
	temp_64 = be64_to_cpu(rsp.local_link_integrity_errors);
	if (temp_64 > 0xFUL)
		temp_64 = 0xFUL;

	temp_link_overrun_errors = temp_64 << 4;

	temp_64 = be64_to_cpu(rsp.excessive_buffer_overruns);
	if (temp_64 > 0xFUL)
		temp_64 = 0xFUL;
	temp_link_overrun_errors |= temp_64;

	p->link_overrun_errors = (u8)temp_link_overrun_errors;

	p->vl15_dropped = 0; /* N/A for OPA */

bail:
	return reply((struct ib_mad_hdr *)pmp);
}

static int pma_get_opa_errorinfo(struct opa_pma_mad *pmp,
				 struct ib_device *ibdev, u32 port,
				 u32 *resp_len)
{
	size_t response_data_size;
	struct _port_ei *rsp;
	struct opa_port_error_info_msg *req;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	u64 port_mask;
	u32 num_ports;
	u32 port_num;
	u8 num_pslm;
	u64 reg;

	req = (struct opa_port_error_info_msg *)pmp->data;
	rsp = &req->port;

	num_ports = OPA_AM_NPORT(be32_to_cpu(pmp->mad_hdr.attr_mod));
	num_pslm = hweight64(be64_to_cpu(req->port_select_mask[3]));

	memset(rsp, 0, sizeof(*rsp));

	if (num_ports != 1 || num_ports != num_pslm) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	/* Sanity check */
	response_data_size = sizeof(struct opa_port_error_info_msg);

	if (response_data_size > sizeof(pmp->data)) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	/*
	 * The bit set in the mask needs to be consistent with the port
	 * the request came in on.
	 */
	port_mask = be64_to_cpu(req->port_select_mask[3]);
	port_num = find_first_bit((unsigned long *)&port_mask,
				  sizeof(port_mask) * 8);

	if (port_num != port) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}
	rsp->port_number = port;

	/* PortRcvErrorInfo */
	rsp->port_rcv_ei.status_and_code = dd->err_info_rcvport.status_and_code;
	memcpy(&rsp->port_rcv_ei.ei.ei1to12.packet_flit1,
	       &dd->err_info_rcvport.packet_flit1, sizeof(u64));
	memcpy(&rsp->port_rcv_ei.ei.ei1to12.packet_flit2,
	       &dd->err_info_rcvport.packet_flit2, sizeof(u64));

	/* ExcessiverBufferOverrunInfo */
	reg = read_iport_csr(dd, ppd->hw_pidx, dd->params->rcv_err_info_reg);
	if (reg & RCV_ERR_INFO_RCV_EXCESS_BUFFER_OVERRUN_SMASK) {
		/*
		 * if the RcvExcessBufferOverrun bit is set, save SC of
		 * first pkt that encountered an excess buffer overrun
		 */
		u8 tmp = (u8)reg;

		tmp &= RCV_ERR_INFO_RCV_EXCESS_BUFFER_OVERRUN_SC_SMASK;
		tmp <<= 2;
		rsp->excessive_buffer_overrun_ei.status_and_sc = tmp;
		/* set the status bit */
		rsp->excessive_buffer_overrun_ei.status_and_sc |= 0x80;
	}

	rsp->port_xmit_constraint_ei.status =
		dd->err_info_xmit_constraint.status;
	rsp->port_xmit_constraint_ei.pkey =
		cpu_to_be16(dd->err_info_xmit_constraint.pkey);
	rsp->port_xmit_constraint_ei.slid =
		cpu_to_be32(dd->err_info_xmit_constraint.slid);

	rsp->port_rcv_constraint_ei.status = dd->err_info_rcv_constraint.status;
	rsp->port_rcv_constraint_ei.pkey =
		cpu_to_be16(dd->err_info_rcv_constraint.pkey);
	rsp->port_rcv_constraint_ei.slid =
		cpu_to_be32(dd->err_info_rcv_constraint.slid);

	/* UncorrectableErrorInfo */
	rsp->uncorrectable_ei.status_and_code = dd->err_info_uncorrectable;

	/* FMConfigErrorInfo */
	rsp->fm_config_ei.status_and_code = dd->err_info_fmconfig;

	if (resp_len)
		*resp_len += response_data_size;

	return reply((struct ib_mad_hdr *)pmp);
}

static int pma_set_opa_portstatus(struct opa_pma_mad *pmp,
				  struct ib_device *ibdev, u32 port,
				  u32 *resp_len)
{
	struct opa_clear_port_status *req =
		(struct opa_clear_port_status *)pmp->data;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	u32 nports = be32_to_cpu(pmp->mad_hdr.attr_mod) >> 24;
	u64 portn = be64_to_cpu(req->port_select_mask[3]);
	u32 counter_select = be32_to_cpu(req->counter_select_mask);
	unsigned long vl_select_mask = VL_MASK_ALL; /* clear all per-vl cnts */
	unsigned long vl;

	if ((nports != 1) || (portn != 1 << port)) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}
	/*
	 * only counters returned by pma_get_opa_portstatus() are
	 * handled, so when pma_get_opa_portstatus() gets a fix,
	 * the corresponding change should be made here as well.
	 */

	if (counter_select & CS_PORT_XMIT_DATA)
		hfi2_write_dev_cntr(dd, C_DC_XMIT_FLITS, CNTR_INVALID_VL, 0);

	if (counter_select & CS_PORT_RCV_DATA)
		hfi2_write_dev_cntr(dd, C_DC_RCV_FLITS, CNTR_INVALID_VL, 0);

	if (counter_select & CS_PORT_XMIT_PKTS)
		hfi2_write_dev_cntr(dd, C_DC_XMIT_PKTS, CNTR_INVALID_VL, 0);

	if (counter_select & CS_PORT_RCV_PKTS)
		hfi2_write_dev_cntr(dd, C_DC_RCV_PKTS, CNTR_INVALID_VL, 0);

	if (counter_select & CS_PORT_MCAST_XMIT_PKTS)
		hfi2_write_dev_cntr(dd, C_DC_MC_XMIT_PKTS, CNTR_INVALID_VL, 0);

	if (counter_select & CS_PORT_MCAST_RCV_PKTS)
		hfi2_write_dev_cntr(dd, C_DC_MC_RCV_PKTS, CNTR_INVALID_VL, 0);

	if (counter_select & CS_PORT_XMIT_WAIT) {
		hfi2_write_port_cntr(ppd, C_TX_WAIT, CNTR_INVALID_VL, 0);
		ppd->port_vl_xmit_wait_last[C_VL_COUNT] = 0;
		ppd->vl_xmit_flit_cnt[C_VL_COUNT] = 0;
	}
	/* ignore cs_sw_portCongestion for HFIs */

	if (counter_select & CS_PORT_RCV_FECN)
		hfi2_write_dev_cntr(dd, C_DC_RCV_FCN, CNTR_INVALID_VL, 0);

	if (counter_select & CS_PORT_RCV_BECN)
		hfi2_write_dev_cntr(dd, C_DC_RCV_BCN, CNTR_INVALID_VL, 0);

	/* ignore cs_port_xmit_time_cong for HFIs */
	/* ignore cs_port_xmit_wasted_bw for now */
	/* ignore cs_port_xmit_wait_data for now */
	if (counter_select & CS_PORT_RCV_BUBBLE)
		hfi2_write_dev_cntr(dd, C_DC_RCV_BBL, CNTR_INVALID_VL, 0);

	/* Only applicable for switch */
	/* if (counter_select & CS_PORT_MARK_FECN)
	 *	hfi2_write_csr(dd, DCC_PRF_PORT_MARK_FECN_CNT, 0);
	 */

	if (counter_select & CS_PORT_RCV_CONSTRAINT_ERRORS)
		hfi2_write_port_cntr(ppd, C_SW_RCV_CSTR_ERR, CNTR_INVALID_VL,
				     0);

	/* ignore cs_port_rcv_switch_relay_errors for HFIs */
	if (counter_select & CS_PORT_XMIT_DISCARDS)
		hfi2_write_port_cntr(ppd, C_SW_XMIT_DSCD, CNTR_INVALID_VL, 0);

	if (counter_select & CS_PORT_XMIT_CONSTRAINT_ERRORS)
		hfi2_write_port_cntr(ppd, C_SW_XMIT_CSTR_ERR, CNTR_INVALID_VL,
				     0);

	if (counter_select & CS_PORT_RCV_REMOTE_PHYSICAL_ERRORS)
		hfi2_write_dev_cntr(dd, C_DC_RMT_PHY_ERR, CNTR_INVALID_VL, 0);

	if (counter_select & CS_LOCAL_LINK_INTEGRITY_ERRORS)
		hfi2_write_dev_cntr(dd, C_DC_RX_REPLAY, CNTR_INVALID_VL, 0);

	if (counter_select & CS_LINK_ERROR_RECOVERY) {
		hfi2_write_dev_cntr(dd, C_DC_SEQ_CRC_CNT, CNTR_INVALID_VL, 0);
		hfi2_write_dev_cntr(dd, C_DC_REINIT_FROM_PEER_CNT,
				    CNTR_INVALID_VL, 0);
	}

	if (counter_select & CS_PORT_RCV_ERRORS)
		hfi2_write_dev_cntr(dd, C_DC_RCV_ERR, CNTR_INVALID_VL, 0);

	if (counter_select & CS_EXCESSIVE_BUFFER_OVERRUNS) {
		hfi2_write_port_cntr(ppd, C_RCV_OVF, CNTR_INVALID_VL, 0);
		ppd->rcv_ovfl_cnt = 0;
	}

	if (counter_select & CS_FM_CONFIG_ERRORS)
		hfi2_write_dev_cntr(dd, C_DC_FM_CFG_ERR, CNTR_INVALID_VL, 0);

	if (counter_select & CS_LINK_DOWNED)
		hfi2_write_port_cntr(ppd, C_SW_LINK_DOWN, CNTR_INVALID_VL, 0);

	if (counter_select & CS_UNCORRECTABLE_ERRORS)
		hfi2_write_dev_cntr(dd, C_DC_UNC_ERR, CNTR_INVALID_VL, 0);

	for_each_set_bit(vl, &vl_select_mask, BITS_PER_LONG) {
		if (counter_select & CS_PORT_XMIT_DATA)
			hfi2_write_port_cntr(ppd, C_TX_FLIT_VL, idx_from_vl(vl),
					     0);

		if (counter_select & CS_PORT_RCV_DATA)
			hfi2_write_dev_cntr(dd, C_DC_RX_FLIT_VL,
					    idx_from_vl(vl), 0);

		if (counter_select & CS_PORT_XMIT_PKTS)
			hfi2_write_port_cntr(ppd, C_TX_PKT_VL, idx_from_vl(vl),
					     0);

		if (counter_select & CS_PORT_RCV_PKTS)
			hfi2_write_dev_cntr(dd, C_DC_RX_PKT_VL, idx_from_vl(vl),
					    0);

		if (counter_select & CS_PORT_XMIT_WAIT) {
			hfi2_write_port_cntr(ppd, C_TX_WAIT_VL, idx_from_vl(vl),
					     0);
			ppd->port_vl_xmit_wait_last[idx_from_vl(vl)] = 0;
			ppd->vl_xmit_flit_cnt[idx_from_vl(vl)] = 0;
		}

		/* sw_port_vl_congestion is 0 for HFIs */
		if (counter_select & CS_PORT_RCV_FECN)
			hfi2_write_dev_cntr(dd, C_DC_RCV_FCN_VL,
					    idx_from_vl(vl), 0);

		if (counter_select & CS_PORT_RCV_BECN)
			hfi2_write_dev_cntr(dd, C_DC_RCV_BCN_VL,
					    idx_from_vl(vl), 0);

		/* port_vl_xmit_time_cong is 0 for HFIs */
		/* port_vl_xmit_wasted_bw ??? */
		/* port_vl_xmit_wait_data - TXE (table 13-9 HFI spec) ??? */
		if (counter_select & CS_PORT_RCV_BUBBLE)
			hfi2_write_dev_cntr(dd, C_DC_RCV_BBL_VL,
					    idx_from_vl(vl), 0);

		/* if (counter_select & CS_PORT_MARK_FECN)
		 *     hfi2_write_csr(dd, DCC_PRF_PORT_VL_MARK_FECN_CNT + offset, 0);
		 */
		if (counter_select & C_SW_XMIT_DSCD_VL)
			hfi2_write_port_cntr(ppd, C_SW_XMIT_DSCD_VL,
					     idx_from_vl(vl), 0);
	}

	if (resp_len)
		*resp_len += sizeof(*req);

	return reply((struct ib_mad_hdr *)pmp);
}

static int pma_set_opa_errorinfo(struct opa_pma_mad *pmp,
				 struct ib_device *ibdev, u32 port,
				 u32 *resp_len)
{
	struct _port_ei *rsp;
	struct opa_port_error_info_msg *req;
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	u64 port_mask;
	u32 num_ports;
	u32 port_num;
	u8 num_pslm;
	u32 error_info_select;

	req = (struct opa_port_error_info_msg *)pmp->data;
	rsp = &req->port;

	num_ports = OPA_AM_NPORT(be32_to_cpu(pmp->mad_hdr.attr_mod));
	num_pslm = hweight64(be64_to_cpu(req->port_select_mask[3]));

	memset(rsp, 0, sizeof(*rsp));

	if (num_ports != 1 || num_ports != num_pslm) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	/*
	 * The bit set in the mask needs to be consistent with the port
	 * the request came in on.
	 */
	port_mask = be64_to_cpu(req->port_select_mask[3]);
	port_num = find_first_bit((unsigned long *)&port_mask,
				  sizeof(port_mask) * 8);

	if (port_num != port) {
		pmp->mad_hdr.status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)pmp);
	}

	error_info_select = be32_to_cpu(req->error_info_select_mask);

	/* PortRcvErrorInfo */
	if (error_info_select & ES_PORT_RCV_ERROR_INFO)
		/* turn off status bit */
		dd->err_info_rcvport.status_and_code &= ~OPA_EI_STATUS_SMASK;

	/* ExcessiveBufferOverrunInfo */
	if (error_info_select & ES_EXCESSIVE_BUFFER_OVERRUN_INFO) {
		/*
		 * status bit is essentially kept in the h/w - bit 5 of
		 * RCV_ERR_INFO
		 */
		write_iport_csr(dd, ppd->hw_pidx, dd->params->rcv_err_info_reg,
				RCV_ERR_INFO_RCV_EXCESS_BUFFER_OVERRUN_SMASK);
		if (dd->is_sriov)
			write_iport_csr(
				dd, loopback_pidx(ppd),
				dd->params->rcv_err_info_reg,
				RCV_ERR_INFO_RCV_EXCESS_BUFFER_OVERRUN_SMASK);
	}

	if (error_info_select & ES_PORT_XMIT_CONSTRAINT_ERROR_INFO)
		dd->err_info_xmit_constraint.status &= ~OPA_EI_STATUS_SMASK;

	if (error_info_select & ES_PORT_RCV_CONSTRAINT_ERROR_INFO)
		dd->err_info_rcv_constraint.status &= ~OPA_EI_STATUS_SMASK;

	/* UncorrectableErrorInfo */
	if (error_info_select & ES_UNCORRECTABLE_ERROR_INFO)
		/* turn off status bit */
		dd->err_info_uncorrectable &= ~OPA_EI_STATUS_SMASK;

	/* FMConfigErrorInfo */
	if (error_info_select & ES_FM_CONFIG_ERROR_INFO)
		/* turn off status bit */
		dd->err_info_fmconfig &= ~OPA_EI_STATUS_SMASK;

	if (resp_len)
		*resp_len += sizeof(*req);

	return reply((struct ib_mad_hdr *)pmp);
}

struct opa_congestion_info_attr {
	__be16 congestion_info;
	u8 control_table_cap; /* Multiple of 64 entry unit CCTs */
	u8 congestion_log_length;
} __packed;

static int __subn_get_opa_cong_info(struct opa_smp *smp, u32 am, u8 *data,
				    struct ib_device *ibdev, u32 port,
				    u32 *resp_len, u32 max_len)
{
	struct opa_congestion_info_attr *p =
		(struct opa_congestion_info_attr *)data;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);

	if (smp_length_check(sizeof(*p), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	p->congestion_info = 0;
	p->control_table_cap = ppd->cc_max_table_entries;
	p->congestion_log_length = OPA_CONG_LOG_ELEMS;

	if (resp_len)
		*resp_len += sizeof(*p);

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_get_opa_cong_setting(struct opa_smp *smp, u32 am, u8 *data,
				       struct ib_device *ibdev, u32 port,
				       u32 *resp_len, u32 max_len)
{
	int i;
	struct opa_congestion_setting_attr *p =
		(struct opa_congestion_setting_attr *)data;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	struct opa_congestion_setting_entry_shadow *entries;
	struct cc_state *cc_state;

	if (smp_length_check(sizeof(*p), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	rcu_read_lock();

	cc_state = get_cc_state(ppd);

	if (!cc_state) {
		rcu_read_unlock();
		return reply((struct ib_mad_hdr *)smp);
	}

	entries = cc_state->cong_setting.entries;
	p->port_control = cpu_to_be16(cc_state->cong_setting.port_control);
	p->control_map = cpu_to_be32(cc_state->cong_setting.control_map);
	for (i = 0; i < OPA_MAX_SLS; i++) {
		p->entries[i].ccti_increase = entries[i].ccti_increase;
		p->entries[i].ccti_timer = cpu_to_be16(entries[i].ccti_timer);
		p->entries[i].trigger_threshold = entries[i].trigger_threshold;
		p->entries[i].ccti_min = entries[i].ccti_min;
	}

	rcu_read_unlock();

	if (resp_len)
		*resp_len += sizeof(*p);

	return reply((struct ib_mad_hdr *)smp);
}

/*
 * Apply congestion control information stored in the ppd to the
 * active structure.
 */
static void apply_cc_state(struct hfi2_pportdata *ppd)
{
	struct cc_state *old_cc_state, *new_cc_state;

	new_cc_state = kzalloc_obj(*new_cc_state, GFP_KERNEL);
	if (!new_cc_state)
		return;

	/*
	 * Hold the lock for updating *and* to prevent ppd information
	 * from changing during the update.
	 */
	spin_lock(&ppd->cc_state_lock);

	old_cc_state = get_cc_state_protected(ppd);
	if (!old_cc_state) {
		/* never active, or shutting down */
		spin_unlock(&ppd->cc_state_lock);
		kfree(new_cc_state);
		return;
	}

	*new_cc_state = *old_cc_state;

	if (ppd->total_cct_entry)
		new_cc_state->cct.ccti_limit = ppd->total_cct_entry - 1;
	else
		new_cc_state->cct.ccti_limit = 0;

	memcpy(new_cc_state->cct.entries, ppd->ccti_entries,
	       ppd->total_cct_entry * sizeof(struct ib_cc_table_entry));

	new_cc_state->cong_setting.port_control = IB_CC_CCS_PC_SL_BASED;
	new_cc_state->cong_setting.control_map = ppd->cc_sl_control_map;
	memcpy(new_cc_state->cong_setting.entries, ppd->congestion_entries,
	       OPA_MAX_SLS * sizeof(struct opa_congestion_setting_entry));

	rcu_assign_pointer(ppd->cc_state, new_cc_state);

	spin_unlock(&ppd->cc_state_lock);

	kfree_rcu(old_cc_state, rcu);
}

static int __subn_set_opa_cong_setting(struct opa_smp *smp, u32 am, u8 *data,
				       struct ib_device *ibdev, u32 port,
				       u32 *resp_len, u32 max_len)
{
	struct opa_congestion_setting_attr *p =
		(struct opa_congestion_setting_attr *)data;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	struct opa_congestion_setting_entry_shadow *entries;
	int i;

	if (smp_length_check(sizeof(*p), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	/*
	 * Save details from packet into the ppd.  Hold the cc_state_lock so
	 * our information is consistent with anyone trying to apply the state.
	 */
	spin_lock(&ppd->cc_state_lock);
	ppd->cc_sl_control_map = be32_to_cpu(p->control_map);

	entries = ppd->congestion_entries;
	for (i = 0; i < OPA_MAX_SLS; i++) {
		entries[i].ccti_increase = p->entries[i].ccti_increase;
		entries[i].ccti_timer = be16_to_cpu(p->entries[i].ccti_timer);
		entries[i].trigger_threshold = p->entries[i].trigger_threshold;
		entries[i].ccti_min = p->entries[i].ccti_min;
	}
	spin_unlock(&ppd->cc_state_lock);

	/* now apply the information */
	apply_cc_state(ppd);

	return __subn_get_opa_cong_setting(smp, am, data, ibdev, port, resp_len,
					   max_len);
}

static int __subn_get_opa_hfi2_cong_log(struct opa_smp *smp, u32 am, u8 *data,
					struct ib_device *ibdev, u32 port,
					u32 *resp_len, u32 max_len)
{
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	struct opa_hfi2_cong_log *cong_log = (struct opa_hfi2_cong_log *)data;
	u64 ts;
	int i;

	if (am || smp_length_check(sizeof(*cong_log), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	spin_lock_irq(&ppd->cc_log_lock);

	cong_log->log_type = OPA_CC_LOG_TYPE_HFI;
	cong_log->congestion_flags = 0;
	cong_log->threshold_event_counter =
		cpu_to_be16(ppd->threshold_event_counter);
	memcpy(cong_log->threshold_cong_event_map,
	       ppd->threshold_cong_event_map,
	       sizeof(cong_log->threshold_cong_event_map));
	/* keep timestamp in units of 1.024 usec */
	ts = ktime_get_ns() / 1024;
	cong_log->current_time_stamp = cpu_to_be32(ts);
	for (i = 0; i < OPA_CONG_LOG_ELEMS; i++) {
		struct opa_hfi2_cong_log_event_internal *cce =
			&ppd->cc_events[ppd->cc_mad_idx++];
		if (ppd->cc_mad_idx == OPA_CONG_LOG_ELEMS)
			ppd->cc_mad_idx = 0;
		/*
		 * Entries which are older than twice the time
		 * required to wrap the counter are supposed to
		 * be zeroed (CA10-49 IBTA, release 1.2.1, V1).
		 */
		if ((ts - cce->timestamp) / 2 > U32_MAX)
			continue;
		memcpy(cong_log->events[i].local_qp_cn_entry, &cce->lqpn, 3);
		memcpy(cong_log->events[i].remote_qp_number_cn_entry,
		       &cce->rqpn, 3);
		cong_log->events[i].sl_svc_type_cn_entry =
			((cce->sl & 0x1f) << 3) | (cce->svc_type & 0x7);
		cong_log->events[i].remote_lid_cn_entry =
			cpu_to_be32(cce->rlid);
		cong_log->events[i].timestamp_cn_entry =
			cpu_to_be32(cce->timestamp);
	}

	/*
	 * Reset threshold_cong_event_map, and threshold_event_counter
	 * to 0 when log is read.
	 */
	memset(ppd->threshold_cong_event_map, 0x0,
	       sizeof(ppd->threshold_cong_event_map));
	ppd->threshold_event_counter = 0;

	spin_unlock_irq(&ppd->cc_log_lock);

	if (resp_len)
		*resp_len += sizeof(struct opa_hfi2_cong_log);

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_get_opa_cc_table(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct ib_cc_table_attr *cc_table_attr =
		(struct ib_cc_table_attr *)data;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	u32 start_block = OPA_AM_START_BLK(am);
	u32 n_blocks = OPA_AM_NBLK(am);
	struct ib_cc_table_entry_shadow *entries;
	int i, j;
	u32 sentry, eentry;
	struct cc_state *cc_state;
	u32 size = sizeof(u16) * (IB_CCT_ENTRIES * n_blocks + 1);

	/* sanity check n_blocks, start_block */
	if (n_blocks == 0 || smp_length_check(size, max_len) ||
	    start_block + n_blocks > ppd->cc_max_table_entries) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	rcu_read_lock();

	cc_state = get_cc_state(ppd);

	if (!cc_state) {
		rcu_read_unlock();
		return reply((struct ib_mad_hdr *)smp);
	}

	sentry = start_block * IB_CCT_ENTRIES;
	eentry = sentry + (IB_CCT_ENTRIES * n_blocks);

	cc_table_attr->ccti_limit = cpu_to_be16(cc_state->cct.ccti_limit);

	entries = cc_state->cct.entries;

	/* return n_blocks, though the last block may not be full */
	for (j = 0, i = sentry; i < eentry; j++, i++)
		cc_table_attr->ccti_entries[j].entry =
			cpu_to_be16(entries[i].entry);

	rcu_read_unlock();

	if (resp_len)
		*resp_len += size;

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_cc_table(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct ib_cc_table_attr *p = (struct ib_cc_table_attr *)data;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	u32 start_block = OPA_AM_START_BLK(am);
	u32 n_blocks = OPA_AM_NBLK(am);
	struct ib_cc_table_entry_shadow *entries;
	int i, j;
	u32 sentry, eentry;
	u16 ccti_limit;
	u32 size = sizeof(u16) * (IB_CCT_ENTRIES * n_blocks + 1);

	/* sanity check n_blocks, start_block */
	if (n_blocks == 0 || smp_length_check(size, max_len) ||
	    start_block + n_blocks > ppd->cc_max_table_entries) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	sentry = start_block * IB_CCT_ENTRIES;
	eentry = sentry + ((n_blocks - 1) * IB_CCT_ENTRIES) +
		 (be16_to_cpu(p->ccti_limit)) % IB_CCT_ENTRIES + 1;

	/* sanity check ccti_limit */
	ccti_limit = be16_to_cpu(p->ccti_limit);
	if (ccti_limit + 1 > eentry) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	/*
	 * Save details from packet into the ppd.  Hold the cc_state_lock so
	 * our information is consistent with anyone trying to apply the state.
	 */
	spin_lock(&ppd->cc_state_lock);
	ppd->total_cct_entry = ccti_limit + 1;
	entries = ppd->ccti_entries;
	for (j = 0, i = sentry; i < eentry; j++, i++)
		entries[i].entry = be16_to_cpu(p->ccti_entries[j].entry);
	spin_unlock(&ppd->cc_state_lock);

	/* now apply the information */
	apply_cc_state(ppd);

	return __subn_get_opa_cc_table(smp, am, data, ibdev, port, resp_len,
				       max_len);
}

struct opa_led_info {
	__be32 rsvd_led_mask;
	__be32 rsvd;
};

#define OPA_LED_SHIFT 31
#define OPA_LED_MASK BIT(OPA_LED_SHIFT)

static int __subn_get_opa_led_info(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = &dd->pport[port - 1];
	struct opa_led_info *p = (struct opa_led_info *)data;
	u32 nport = OPA_AM_NPORT(am);
	u32 is_beaconing_active;

	if (nport != 1 || smp_length_check(sizeof(*p), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	/*
	 * This pairs with the memory barrier in hfi2_start_led_override to
	 * ensure that we read the correct state of LED beaconing represented
	 * by led_override_timer_active
	 */
	smp_rmb();
	is_beaconing_active = !!atomic_read(&ppd->led_override_timer_active);
	p->rsvd_led_mask = cpu_to_be32(is_beaconing_active << OPA_LED_SHIFT);

	if (resp_len)
		*resp_len += sizeof(struct opa_led_info);

	return reply((struct ib_mad_hdr *)smp);
}

static int __subn_set_opa_led_info(struct opa_smp *smp, u32 am, u8 *data,
				   struct ib_device *ibdev, u32 port,
				   u32 *resp_len, u32 max_len)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = &dd->pport[port - 1];
	struct opa_led_info *p = (struct opa_led_info *)data;
	u32 nport = OPA_AM_NPORT(am);
	int on = !!(be32_to_cpu(p->rsvd_led_mask) & OPA_LED_MASK);

	if (nport != 1 || smp_length_check(sizeof(*p), max_len)) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	if (on)
		dd->params->start_led_override(ppd, 2000, 1500);
	else
		dd->params->hfi2_shutdown_led_override(ppd);

	return __subn_get_opa_led_info(smp, am, data, ibdev, port, resp_len,
				       max_len);
}

static int subn_get_opa_sma(__be16 attr_id, struct opa_smp *smp, u32 am,
			    u8 *data, struct ib_device *ibdev, u32 port,
			    u32 *resp_len, u32 max_len)
{
	int ret;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);

	switch (attr_id) {
	case IB_SMP_ATTR_NODE_DESC:
		ret = __subn_get_opa_nodedesc(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case IB_SMP_ATTR_NODE_INFO:
		ret = __subn_get_opa_nodeinfo(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case IB_SMP_ATTR_PORT_INFO:
		ret = __subn_get_opa_portinfo(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case IB_SMP_ATTR_PKEY_TABLE:
		ret = __subn_get_opa_pkeytable(smp, am, data, ibdev, port,
					       resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_SL_TO_SC_MAP:
		ret = __subn_get_opa_sl_to_sc(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_SC_TO_SL_MAP:
		ret = __subn_get_opa_sc_to_sl(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_SC_TO_VLT_MAP:
		ret = __subn_get_opa_sc_to_vlt(smp, am, data, ibdev, port,
					       resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_SC_TO_VLNT_MAP:
		ret = __subn_get_opa_sc_to_vlnt(smp, am, data, ibdev, port,
						resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_PORT_STATE_INFO:
		ret = __subn_get_opa_psi(smp, am, data, ibdev, port, resp_len,
					 max_len);
		break;
	case OPA_ATTRIB_ID_BUFFER_CONTROL_TABLE:
		ret = __subn_get_opa_bct(smp, am, data, ibdev, port, resp_len,
					 max_len);
		break;
	case OPA_ATTRIB_ID_CABLE_INFO:
		ret = __subn_get_opa_cable_info(smp, am, data, ibdev, port,
						resp_len, max_len);
		break;
	case IB_SMP_ATTR_VL_ARB_TABLE:
		ret = __subn_get_opa_vl_arb(smp, am, data, ibdev, port,
					    resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_CONGESTION_INFO:
		ret = __subn_get_opa_cong_info(smp, am, data, ibdev, port,
					       resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_HFI_CONGESTION_SETTING:
		ret = __subn_get_opa_cong_setting(smp, am, data, ibdev, port,
						  resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_HFI_CONGESTION_LOG:
		ret = __subn_get_opa_hfi2_cong_log(smp, am, data, ibdev, port,
						   resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_CONGESTION_CONTROL_TABLE:
		ret = __subn_get_opa_cc_table(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case IB_SMP_ATTR_LED_INFO:
		ret = __subn_get_opa_led_info(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case IB_SMP_ATTR_SM_INFO:
		if (ibp->rvp.port_cap_flags & IB_PORT_SM_DISABLED)
			return IB_MAD_RESULT_SUCCESS | IB_MAD_RESULT_CONSUMED;
		if (ibp->rvp.port_cap_flags & IB_PORT_SM)
			return IB_MAD_RESULT_SUCCESS;
		fallthrough;
	default:
		smp->status |= IB_SMP_UNSUP_METH_ATTR;
		ret = reply((struct ib_mad_hdr *)smp);
		break;
	}
	return ret;
}

static int subn_set_opa_sma(__be16 attr_id, struct opa_smp *smp, u32 am,
			    u8 *data, struct ib_device *ibdev, u32 port,
			    u32 *resp_len, u32 max_len, int local_mad)
{
	int ret;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);

	switch (attr_id) {
	case IB_SMP_ATTR_PORT_INFO:
		ret = __subn_set_opa_portinfo(smp, am, data, ibdev, port,
					      resp_len, max_len, local_mad);
		break;
	case IB_SMP_ATTR_PKEY_TABLE:
		ret = __subn_set_opa_pkeytable(smp, am, data, ibdev, port,
					       resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_SL_TO_SC_MAP:
		ret = __subn_set_opa_sl_to_sc(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_SC_TO_SL_MAP:
		ret = __subn_set_opa_sc_to_sl(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_SC_TO_VLT_MAP:
		ret = __subn_set_opa_sc_to_vlt(smp, am, data, ibdev, port,
					       resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_SC_TO_VLNT_MAP:
		ret = __subn_set_opa_sc_to_vlnt(smp, am, data, ibdev, port,
						resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_PORT_STATE_INFO:
		ret = __subn_set_opa_psi(smp, am, data, ibdev, port, resp_len,
					 max_len, local_mad);
		break;
	case OPA_ATTRIB_ID_BUFFER_CONTROL_TABLE:
		ret = __subn_set_opa_bct(smp, am, data, ibdev, port, resp_len,
					 max_len);
		break;
	case IB_SMP_ATTR_VL_ARB_TABLE:
		ret = __subn_set_opa_vl_arb(smp, am, data, ibdev, port,
					    resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_HFI_CONGESTION_SETTING:
		ret = __subn_set_opa_cong_setting(smp, am, data, ibdev, port,
						  resp_len, max_len);
		break;
	case OPA_ATTRIB_ID_CONGESTION_CONTROL_TABLE:
		ret = __subn_set_opa_cc_table(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case IB_SMP_ATTR_LED_INFO:
		ret = __subn_set_opa_led_info(smp, am, data, ibdev, port,
					      resp_len, max_len);
		break;
	case IB_SMP_ATTR_SM_INFO:
		if (ibp->rvp.port_cap_flags & IB_PORT_SM_DISABLED)
			return IB_MAD_RESULT_SUCCESS | IB_MAD_RESULT_CONSUMED;
		if (ibp->rvp.port_cap_flags & IB_PORT_SM)
			return IB_MAD_RESULT_SUCCESS;
		fallthrough;
	default:
		smp->status |= IB_SMP_UNSUP_METH_ATTR;
		ret = reply((struct ib_mad_hdr *)smp);
		break;
	}
	return ret;
}

static inline void set_aggr_error(struct opa_aggregate *ag)
{
	ag->err_reqlength |= cpu_to_be16(0x8000);
}

static int subn_get_opa_aggregate(struct opa_smp *smp, struct ib_device *ibdev,
				  u32 port, u32 *resp_len)
{
	int i;
	u32 num_attr = be32_to_cpu(smp->attr_mod) & 0x000000ff;
	u8 *next_smp = opa_get_smp_data(smp);

	if (num_attr < 1 || num_attr > 117) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	for (i = 0; i < num_attr; i++) {
		struct opa_aggregate *agg;
		size_t agg_data_len;
		size_t agg_size;
		u32 am;

		agg = (struct opa_aggregate *)next_smp;
		agg_data_len = (be16_to_cpu(agg->err_reqlength) & 0x007f) * 8;
		agg_size = sizeof(*agg) + agg_data_len;
		am = be32_to_cpu(agg->attr_mod);

		*resp_len += agg_size;

		if (next_smp + agg_size > ((u8 *)smp) + sizeof(*smp)) {
			smp->status |= IB_SMP_INVALID_FIELD;
			return reply((struct ib_mad_hdr *)smp);
		}

		/* zero the payload for this segment */
		memset(next_smp + sizeof(*agg), 0, agg_data_len);

		(void)subn_get_opa_sma(agg->attr_id, smp, am, agg->data, ibdev,
				       port, NULL, (u32)agg_data_len);

		if (smp->status & IB_SMP_INVALID_FIELD)
			break;
		if (smp->status & ~IB_SMP_DIRECTION) {
			set_aggr_error(agg);
			return reply((struct ib_mad_hdr *)smp);
		}
		next_smp += agg_size;
	}

	return reply((struct ib_mad_hdr *)smp);
}

static int subn_set_opa_aggregate(struct opa_smp *smp, struct ib_device *ibdev,
				  u32 port, u32 *resp_len, int local_mad)
{
	int i;
	u32 num_attr = be32_to_cpu(smp->attr_mod) & 0x000000ff;
	u8 *next_smp = opa_get_smp_data(smp);

	if (num_attr < 1 || num_attr > 117) {
		smp->status |= IB_SMP_INVALID_FIELD;
		return reply((struct ib_mad_hdr *)smp);
	}

	for (i = 0; i < num_attr; i++) {
		struct opa_aggregate *agg;
		size_t agg_data_len;
		size_t agg_size;
		u32 am;

		agg = (struct opa_aggregate *)next_smp;
		agg_data_len = (be16_to_cpu(agg->err_reqlength) & 0x007f) * 8;
		agg_size = sizeof(*agg) + agg_data_len;
		am = be32_to_cpu(agg->attr_mod);

		*resp_len += agg_size;

		if (next_smp + agg_size > ((u8 *)smp) + sizeof(*smp)) {
			smp->status |= IB_SMP_INVALID_FIELD;
			return reply((struct ib_mad_hdr *)smp);
		}

		(void)subn_set_opa_sma(agg->attr_id, smp, am, agg->data, ibdev,
				       port, NULL, (u32)agg_data_len,
				       local_mad);

		if (smp->status & IB_SMP_INVALID_FIELD)
			break;
		if (smp->status & ~IB_SMP_DIRECTION) {
			set_aggr_error(agg);
			return reply((struct ib_mad_hdr *)smp);
		}
		next_smp += agg_size;
	}

	return reply((struct ib_mad_hdr *)smp);
}

/*
 * OPAv1 specifies that, on the transition to link up, these counters
 * are cleared:
 *   PortRcvErrors [*]
 *   LinkErrorRecovery
 *   LocalLinkIntegrityErrors
 *   ExcessiveBufferOverruns [*]
 *
 * [*] Error info associated with these counters is retained, but the
 * error info status is reset to 0.
 */
void hfi2_clear_linkup_counters(struct hfi2_pportdata *ppd)
{
	struct hfi2_devdata *dd = ppd->dd;

	/* PortRcvErrors */
	hfi2_write_dev_cntr(dd, C_DC_RCV_ERR, CNTR_INVALID_VL, 0);
	dd->err_info_rcvport.status_and_code &= ~OPA_EI_STATUS_SMASK;
	/* LinkErrorRecovery */
	hfi2_write_dev_cntr(dd, C_DC_SEQ_CRC_CNT, CNTR_INVALID_VL, 0);
	hfi2_write_dev_cntr(dd, C_DC_REINIT_FROM_PEER_CNT, CNTR_INVALID_VL, 0);
	/* LocalLinkIntegrityErrors */
	hfi2_write_dev_cntr(dd, C_DC_RX_REPLAY, CNTR_INVALID_VL, 0);
	/* ExcessiveBufferOverruns */
	hfi2_write_port_cntr(ppd, C_RCV_OVF, CNTR_INVALID_VL, 0);
	ppd->rcv_ovfl_cnt = 0;
	dd->err_info_xmit_constraint.status &= ~OPA_EI_STATUS_SMASK;
}

static int is_full_mgmt_pkey_in_table(struct hfi2_ibport *ibp)
{
	unsigned int i;
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);

	for (i = 0; i < ppd->dd->params->pkey_table_size; ++i)
		if (ppd->pkeys[i] == FULL_MGMT_P_KEY)
			return 1;

	return 0;
}

/*
 * is_local_mad() returns 1 if 'mad' is sent from, and destined to the
 * local node, 0 otherwise.
 */
static int is_local_mad(struct hfi2_ibport *ibp, const struct opa_mad *mad,
			const struct ib_wc *in_wc)
{
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	const struct opa_smp *smp = (const struct opa_smp *)mad;

	if (smp->mgmt_class == IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE) {
		return (smp->hop_cnt == 0 &&
			smp->route.dr.dr_slid == OPA_LID_PERMISSIVE &&
			smp->route.dr.dr_dlid == OPA_LID_PERMISSIVE);
	}

	return (in_wc->slid == ppd->lid);
}

/*
 * opa_local_smp_check() should only be called on MADs for which
 * is_local_mad() returns true. It applies the SMP checks that are
 * specific to SMPs which are sent from, and destined to this node.
 * opa_local_smp_check() returns 0 if the SMP passes its checks, 1
 * otherwise.
 *
 * SMPs which arrive from other nodes are instead checked by
 * opa_smp_check().
 */
static int opa_local_smp_check(struct hfi2_ibport *ibp,
			       const struct ib_wc *in_wc)
{
	struct hfi2_pportdata *ppd = ppd_from_ibp(ibp);
	u16 pkey;

	if (in_wc->pkey_index >= ppd->dd->params->pkey_table_size)
		return 1;

	pkey = ppd->pkeys[in_wc->pkey_index];
	/*
	 * We need to do the "node-local" checks specified in OPAv1,
	 * rev 0.90, section 9.10.26, which are:
	 *   - pkey is 0x7fff, or 0xffff
	 *   - Source QPN == 0 || Destination QPN == 0
	 *   - the MAD header's management class is either
	 *     IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE or
	 *     IB_MGMT_CLASS_SUBN_LID_ROUTED
	 *   - SLID != 0
	 *
	 * However, we know (and so don't need to check again) that,
	 * for local SMPs, the MAD stack passes MADs with:
	 *   - Source QPN of 0
	 *   - MAD mgmt_class is IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE
	 *   - SLID is either: OPA_LID_PERMISSIVE (0xFFFFFFFF), or
	 *     our own port's lid
	 *
	 */
	if (pkey == LIM_MGMT_P_KEY || pkey == FULL_MGMT_P_KEY)
		return 0;
	ingress_pkey_table_fail(ppd, pkey, in_wc->slid);
	return 1;
}

/**
 * hfi2_pkey_validation_pma - It validates PKEYs for incoming PMA MAD packets.
 * @ibp: IB port data
 * @in_mad: MAD packet with header and data
 * @in_wc: Work completion data such as source LID, port number, etc.
 *
 * These are all the possible logic rules for validating a pkey:
 *
 * a) If pkey neither FULL_MGMT_P_KEY nor LIM_MGMT_P_KEY,
 *    and NOT self-originated packet:
 *     Drop MAD packet as it should always be part of the
 *     management partition unless it's a self-originated packet.
 *
 * b) If pkey_index -> FULL_MGMT_P_KEY, and LIM_MGMT_P_KEY in pkey table:
 *     The packet is coming from a management node and the receiving node
 *     is also a management node, so it is safe for the packet to go through.
 *
 * c) If pkey_index -> FULL_MGMT_P_KEY, and LIM_MGMT_P_KEY is NOT in pkey table:
 *     Drop the packet as LIM_MGMT_P_KEY should always be in the pkey table.
 *     It could be an FM misconfiguration.
 *
 * d) If pkey_index -> LIM_MGMT_P_KEY and FULL_MGMT_P_KEY is NOT in pkey table:
 *     It is safe for the packet to go through since a non-management node is
 *     talking to another non-management node.
 *
 * e) If pkey_index -> LIM_MGMT_P_KEY and FULL_MGMT_P_KEY in pkey table:
 *     Drop the packet because a non-management node is talking to a
 *     management node, and it could be an attack.
 *
 * For the implementation, these rules can be simplied to only checking
 * for (a) and (e). There's no need to check for rule (b) as
 * the packet doesn't need to be dropped. Rule (c) is not possible in
 * the driver as LIM_MGMT_P_KEY is always in the pkey table.
 *
 * Return:
 * 0 - pkey is okay, -EINVAL it's a bad pkey
 */
static int hfi2_pkey_validation_pma(struct hfi2_ibport *ibp,
				    const struct opa_mad *in_mad,
				    const struct ib_wc *in_wc)
{
	u16 pkey_value = hfi2_lookup_pkey_value(ibp, in_wc->pkey_index);

	/* Rule (a) from above */
	if (!is_local_mad(ibp, in_mad, in_wc) && pkey_value != LIM_MGMT_P_KEY &&
	    pkey_value != FULL_MGMT_P_KEY)
		return -EINVAL;

	/* Rule (e) from above */
	if (pkey_value == LIM_MGMT_P_KEY && is_full_mgmt_pkey_in_table(ibp))
		return -EINVAL;

	return 0;
}

static int process_subn_opa(struct ib_device *ibdev, int mad_flags, u32 port,
			    const struct opa_mad *in_mad,
			    struct opa_mad *out_mad, u32 *resp_len,
			    int local_mad)
{
	struct opa_smp *smp = (struct opa_smp *)out_mad;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	u8 *data;
	u32 am, data_size;
	__be16 attr_id;
	int ret;

	*out_mad = *in_mad;
	data = opa_get_smp_data(smp);
	data_size = (u32)opa_get_smp_data_size(smp);

	am = be32_to_cpu(smp->attr_mod);
	attr_id = smp->attr_id;
	if (smp->class_version != OPA_SM_CLASS_VERSION) {
		smp->status |= IB_SMP_UNSUP_VERSION;
		ret = reply((struct ib_mad_hdr *)smp);
		return ret;
	}
	ret = check_mkey(ibp, (struct ib_mad_hdr *)smp, mad_flags, smp->mkey,
			 smp->route.dr.dr_slid, smp->route.dr.return_path,
			 smp->hop_cnt);
	if (ret) {
		u32 port_num = be32_to_cpu(smp->attr_mod);

		/*
		 * If this is a get/set portinfo, we already check the
		 * M_Key if the MAD is for another port and the M_Key
		 * is OK on the receiving port. This check is needed
		 * to increment the error counters when the M_Key
		 * fails to match on *both* ports.
		 */
		if (attr_id == IB_SMP_ATTR_PORT_INFO &&
		    (smp->method == IB_MGMT_METHOD_GET ||
		     smp->method == IB_MGMT_METHOD_SET) &&
		    port_num && port_num <= ibdev->phys_port_cnt &&
		    port != port_num)
			(void)check_mkey(to_iport(ibdev, port_num),
					 (struct ib_mad_hdr *)smp, 0, smp->mkey,
					 smp->route.dr.dr_slid,
					 smp->route.dr.return_path,
					 smp->hop_cnt);
		ret = IB_MAD_RESULT_FAILURE;
		return ret;
	}

	*resp_len = opa_get_smp_header_size(smp);

	switch (smp->method) {
	case IB_MGMT_METHOD_GET:
		switch (attr_id) {
		default:
			clear_opa_smp_data(smp);
			ret = subn_get_opa_sma(attr_id, smp, am, data, ibdev,
					       port, resp_len, data_size);
			break;
		case OPA_ATTRIB_ID_AGGREGATE:
			ret = subn_get_opa_aggregate(smp, ibdev, port,
						     resp_len);
			break;
		}
		break;
	case IB_MGMT_METHOD_SET:
		switch (attr_id) {
		default:
			ret = subn_set_opa_sma(attr_id, smp, am, data, ibdev,
					       port, resp_len, data_size,
					       local_mad);
			break;
		case OPA_ATTRIB_ID_AGGREGATE:
			ret = subn_set_opa_aggregate(smp, ibdev, port, resp_len,
						     local_mad);
			break;
		}
		break;
	case IB_MGMT_METHOD_TRAP:
	case IB_MGMT_METHOD_REPORT:
	case IB_MGMT_METHOD_REPORT_RESP:
	case IB_MGMT_METHOD_GET_RESP:
		/*
		 * The ib_mad module will call us to process responses
		 * before checking for other consumers.
		 * Just tell the caller to process it normally.
		 */
		ret = IB_MAD_RESULT_SUCCESS;
		break;
	case IB_MGMT_METHOD_TRAP_REPRESS:
		subn_handle_opa_trap_repress(ibp, smp);
		/* Always successful */
		ret = IB_MAD_RESULT_SUCCESS;
		break;
	default:
		smp->status |= IB_SMP_UNSUP_METHOD;
		ret = reply((struct ib_mad_hdr *)smp);
		break;
	}

	return ret;
}

static int process_subn(struct ib_device *ibdev, int mad_flags, u32 port,
			const struct ib_mad *in_mad, struct ib_mad *out_mad)
{
	struct ib_smp *smp = (struct ib_smp *)out_mad;
	struct hfi2_ibport *ibp = to_iport(ibdev, port);
	int ret;

	*out_mad = *in_mad;
	if (smp->class_version != 1) {
		smp->status |= IB_SMP_UNSUP_VERSION;
		ret = reply((struct ib_mad_hdr *)smp);
		return ret;
	}

	ret = check_mkey(ibp, (struct ib_mad_hdr *)smp, mad_flags, smp->mkey,
			 (__force __be32)smp->dr_slid, smp->return_path,
			 smp->hop_cnt);
	if (ret) {
		u32 port_num = be32_to_cpu(smp->attr_mod);

		/*
		 * If this is a get/set portinfo, we already check the
		 * M_Key if the MAD is for another port and the M_Key
		 * is OK on the receiving port. This check is needed
		 * to increment the error counters when the M_Key
		 * fails to match on *both* ports.
		 */
		if (in_mad->mad_hdr.attr_id == IB_SMP_ATTR_PORT_INFO &&
		    (smp->method == IB_MGMT_METHOD_GET ||
		     smp->method == IB_MGMT_METHOD_SET) &&
		    port_num && port_num <= ibdev->phys_port_cnt &&
		    port != port_num)
			(void)check_mkey(to_iport(ibdev, port_num),
					 (struct ib_mad_hdr *)smp, 0, smp->mkey,
					 (__force __be32)smp->dr_slid,
					 smp->return_path, smp->hop_cnt);
		ret = IB_MAD_RESULT_FAILURE;
		return ret;
	}

	switch (smp->method) {
	case IB_MGMT_METHOD_GET:
		switch (smp->attr_id) {
		case IB_SMP_ATTR_NODE_INFO:
			ret = subn_get_nodeinfo(smp, ibdev, port);
			break;
		default:
			smp->status |= IB_SMP_UNSUP_METH_ATTR;
			ret = reply((struct ib_mad_hdr *)smp);
			break;
		}
		break;
	}

	return ret;
}

static int process_perf(struct ib_device *ibdev, u32 port,
			const struct ib_mad *in_mad, struct ib_mad *out_mad)
{
	struct ib_pma_mad *pmp = (struct ib_pma_mad *)out_mad;
	struct ib_class_port_info *cpi =
		(struct ib_class_port_info *)&pmp->data;
	int ret = IB_MAD_RESULT_FAILURE;

	*out_mad = *in_mad;
	if (pmp->mad_hdr.class_version != 1) {
		pmp->mad_hdr.status |= IB_SMP_UNSUP_VERSION;
		ret = reply((struct ib_mad_hdr *)pmp);
		return ret;
	}

	switch (pmp->mad_hdr.method) {
	case IB_MGMT_METHOD_GET:
		switch (pmp->mad_hdr.attr_id) {
		case IB_PMA_PORT_COUNTERS:
			ret = pma_get_ib_portcounters(pmp, ibdev, port);
			break;
		case IB_PMA_PORT_COUNTERS_EXT:
			ret = pma_get_ib_portcounters_ext(pmp, ibdev, port);
			break;
		case IB_PMA_CLASS_PORT_INFO:
			cpi->capability_mask = IB_PMA_CLASS_CAP_EXT_WIDTH;
			ret = reply((struct ib_mad_hdr *)pmp);
			break;
		default:
			pmp->mad_hdr.status |= IB_SMP_UNSUP_METH_ATTR;
			ret = reply((struct ib_mad_hdr *)pmp);
			break;
		}
		break;

	case IB_MGMT_METHOD_SET:
		if (pmp->mad_hdr.attr_id) {
			pmp->mad_hdr.status |= IB_SMP_UNSUP_METH_ATTR;
			ret = reply((struct ib_mad_hdr *)pmp);
		}
		break;

	case IB_MGMT_METHOD_TRAP:
	case IB_MGMT_METHOD_GET_RESP:
		/*
		 * The ib_mad module will call us to process responses
		 * before checking for other consumers.
		 * Just tell the caller to process it normally.
		 */
		ret = IB_MAD_RESULT_SUCCESS;
		break;

	default:
		pmp->mad_hdr.status |= IB_SMP_UNSUP_METHOD;
		ret = reply((struct ib_mad_hdr *)pmp);
		break;
	}

	return ret;
}

static int process_perf_opa(struct ib_device *ibdev, u32 port,
			    const struct opa_mad *in_mad,
			    struct opa_mad *out_mad, u32 *resp_len)
{
	struct opa_pma_mad *pmp = (struct opa_pma_mad *)out_mad;
	int ret;

	*out_mad = *in_mad;

	if (pmp->mad_hdr.class_version != OPA_SM_CLASS_VERSION) {
		pmp->mad_hdr.status |= IB_SMP_UNSUP_VERSION;
		return reply((struct ib_mad_hdr *)pmp);
	}

	*resp_len = sizeof(pmp->mad_hdr);

	switch (pmp->mad_hdr.method) {
	case IB_MGMT_METHOD_GET:
		switch (pmp->mad_hdr.attr_id) {
		case IB_PMA_CLASS_PORT_INFO:
			ret = pma_get_opa_classportinfo(pmp, ibdev, resp_len);
			break;
		case OPA_PM_ATTRIB_ID_PORT_STATUS:
			ret = pma_get_opa_portstatus(pmp, ibdev, port,
						     resp_len);
			break;
		case OPA_PM_ATTRIB_ID_DATA_PORT_COUNTERS:
			ret = pma_get_opa_datacounters(pmp, ibdev, port,
						       resp_len);
			break;
		case OPA_PM_ATTRIB_ID_ERROR_PORT_COUNTERS:
			ret = pma_get_opa_porterrors(pmp, ibdev, port,
						     resp_len);
			break;
		case OPA_PM_ATTRIB_ID_ERROR_INFO:
			ret = pma_get_opa_errorinfo(pmp, ibdev, port, resp_len);
			break;
		default:
			pmp->mad_hdr.status |= IB_SMP_UNSUP_METH_ATTR;
			ret = reply((struct ib_mad_hdr *)pmp);
			break;
		}
		break;

	case IB_MGMT_METHOD_SET:
		switch (pmp->mad_hdr.attr_id) {
		case OPA_PM_ATTRIB_ID_CLEAR_PORT_STATUS:
			ret = pma_set_opa_portstatus(pmp, ibdev, port,
						     resp_len);
			break;
		case OPA_PM_ATTRIB_ID_ERROR_INFO:
			ret = pma_set_opa_errorinfo(pmp, ibdev, port, resp_len);
			break;
		default:
			pmp->mad_hdr.status |= IB_SMP_UNSUP_METH_ATTR;
			ret = reply((struct ib_mad_hdr *)pmp);
			break;
		}
		break;

	case IB_MGMT_METHOD_TRAP:
	case IB_MGMT_METHOD_GET_RESP:
		/*
		 * The ib_mad module will call us to process responses
		 * before checking for other consumers.
		 * Just tell the caller to process it normally.
		 */
		ret = IB_MAD_RESULT_SUCCESS;
		break;

	default:
		pmp->mad_hdr.status |= IB_SMP_UNSUP_METHOD;
		ret = reply((struct ib_mad_hdr *)pmp);
		break;
	}

	return ret;
}

static int hfi2_process_opa_mad(struct ib_device *ibdev, int mad_flags,
				u32 port, const struct ib_wc *in_wc,
				const struct ib_grh *in_grh,
				const struct opa_mad *in_mad,
				struct opa_mad *out_mad, size_t *out_mad_size,
				u16 *out_mad_pkey_index)
{
	int ret;
	int pkey_idx;
	int local_mad = 0;
	u32 resp_len = in_wc->byte_len - sizeof(*in_grh);
	struct hfi2_ibport *ibp = to_iport(ibdev, port);

	pkey_idx = hfi2_lookup_pkey_idx(ibp, LIM_MGMT_P_KEY);
	if (pkey_idx < 0) {
		pr_warn("failed to find limited mgmt pkey, defaulting 0x%x\n",
			hfi2_get_pkey(ibp, 1));
		pkey_idx = 1;
	}
	*out_mad_pkey_index = (u16)pkey_idx;

	switch (in_mad->mad_hdr.mgmt_class) {
	case IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE:
	case IB_MGMT_CLASS_SUBN_LID_ROUTED:
		local_mad = is_local_mad(ibp, in_mad, in_wc);
		if (local_mad) {
			ret = opa_local_smp_check(ibp, in_wc);
			if (ret)
				return IB_MAD_RESULT_FAILURE;
		}
		ret = process_subn_opa(ibdev, mad_flags, port, in_mad, out_mad,
				       &resp_len, local_mad);
		goto bail;
	case IB_MGMT_CLASS_PERF_MGMT:
		ret = hfi2_pkey_validation_pma(ibp, in_mad, in_wc);
		if (ret)
			return IB_MAD_RESULT_FAILURE;

		ret = process_perf_opa(ibdev, port, in_mad, out_mad, &resp_len);
		goto bail;

	default:
		ret = IB_MAD_RESULT_SUCCESS;
	}

bail:
	if (ret & IB_MAD_RESULT_REPLY)
		*out_mad_size = round_up(resp_len, 8);
	else if (ret & IB_MAD_RESULT_SUCCESS)
		*out_mad_size = in_wc->byte_len - sizeof(struct ib_grh);

	return ret;
}

static int hfi2_process_ib_mad(struct ib_device *ibdev, int mad_flags, u32 port,
			       const struct ib_wc *in_wc,
			       const struct ib_grh *in_grh,
			       const struct ib_mad *in_mad,
			       struct ib_mad *out_mad)
{
	int ret;

	switch (in_mad->mad_hdr.mgmt_class) {
	case IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE:
	case IB_MGMT_CLASS_SUBN_LID_ROUTED:
		ret = process_subn(ibdev, mad_flags, port, in_mad, out_mad);
		break;
	case IB_MGMT_CLASS_PERF_MGMT:
		ret = process_perf(ibdev, port, in_mad, out_mad);
		break;
	default:
		ret = IB_MAD_RESULT_SUCCESS;
		break;
	}

	return ret;
}

static const char *smp_attr_id_str(u16 attr_id)
{
	const char *s = "Unknown";

	switch (attr_id) {
	case IB_SMP_ATTR_NODE_DESC:
		s = "NODE_DESC";
		break;
	case IB_SMP_ATTR_NODE_INFO:
		s = "NODE_INFO";
		break;
	case OPA_ATTRIB_ID_PORT_INFO:
		s = "PORT_INFO";
		break;
	case IB_SMP_ATTR_PKEY_TABLE:
		s = "PKEY_TABLE";
		break;
	case OPA_ATTRIB_ID_SL_TO_SC_MAP:
		s = "SL_TO_SC_MAP";
		break;
	case IB_SMP_ATTR_VL_ARB_TABLE:
		s = "VL_ARB_TABLE";
		break;
	case OPA_ATTRIB_ID_SC_TO_SL_MAP:
		s = "SC_TO_SL_MAP";
		break;
	case OPA_ATTRIB_ID_SC_TO_VLR_MAP:
		s = "SC_TO_VLR_MAP";
		break;
	case OPA_ATTRIB_ID_SC_TO_VLT_MAP:
		s = "SC_TO_VLT_MAP";
		break;
	case OPA_ATTRIB_ID_SC_TO_VLNT_MAP:
		s = "SC_TO_VLNT_MAP";
		break;
	case OPA_ATTRIB_ID_PORT_STATE_INFO:
		s = "PORT_STATE_INFO";
		break;
	case OPA_ATTRIB_ID_BUFFER_CONTROL_TABLE:
		s = "BUFFER_CONTROL_TABLE";
		break;
	case OPA_ATTRIB_ID_CONGESTION_INFO:
		s = "CONGESTION_INFO";
		break;
	case OPA_ATTRIB_ID_HFI_CONGESTION_LOG:
		s = "CONGESTION_LOG";
		break;
	case OPA_ATTRIB_ID_HFI_CONGESTION_SETTING:
		s = "CONGESTION_SETTING";
		break;
	case OPA_ATTRIB_ID_CONGESTION_CONTROL_TABLE:
		s = "CONGESTION_CONTROL_TABLE";
		break;
	case IB_SMP_ATTR_NOTICE:
		s = "NOTICE";
		break;
	case IB_SMP_ATTR_SM_INFO:
		s = "SM_INFO";
		break;
	case IB_SMP_ATTR_LED_INFO:
		s = "LED_INFO";
		break;
	case OPA_ATTRIB_ID_CABLE_INFO:
		s = "CABLE_INFO";
		break;
	}

	return s;
}

static const char *sa_attr_id_str(u16 attr_id)
{
	const char *s = "Unknown";

	switch (be16_to_cpu(attr_id)) {
	case IB_SA_ATTR_CLASS_PORTINFO:
		s = "IB_CLASS_PORT_INFO";
		break;
	case IB_SA_ATTR_PATH_REC:
		s = "IB_PATH_REC";
		break;
	case IB_SA_ATTR_MC_MEMBER_REC:
		s = "IB_MC_MEMBER_REC";
		break;
	case IB_SA_ATTR_GUID_INFO_REC:
		s = "IB_GUID_INFO_REC";
		break;
	}

	return s;
}

static const char *pma_attr_id_str(u16 attr_id)
{
	const char *s = "Unknown";

	switch (attr_id) {
	case IB_PMA_PORT_COUNTERS:
		s = "IB_PORT_COUNTERS";
		break;
	case IB_PMA_PORT_COUNTERS_EXT:
		s = "IB_PORT_COUNTERS_EXT";
		break;
	case IB_PMA_CLASS_PORT_INFO:
		s = "IB_CLASS_PORT_INFO";
		break;
	case OPA_PM_ATTRIB_ID_PORT_STATUS:
		s = "OPA_PORT_STATUS";
		break;
	case OPA_PM_ATTRIB_ID_DATA_PORT_COUNTERS:
		s = "OPA_DATA_PORT_COUNTERS";
		break;
	case OPA_PM_ATTRIB_ID_ERROR_PORT_COUNTERS:
		s = "OPA_ERROR_PORT_COUNTERS";
		break;
	case OPA_PM_ATTRIB_ID_ERROR_INFO:
		s = "OPA_ERROR_INFO";
		break;
	}

	return s;
}

static void print_mad(struct hfi2_devdata *dd, u32 port,
		      const struct ib_mad *in_mad, const char *caller, bool rcv)
{
	const char *bv;
	const char *mc;
	const char *method_str;
	char mc_buf[64];
	char method_buf[64];
	char extra[64];
	bool do_smp = false;
	bool do_sa = false;
	bool do_pma = false;

	if (in_mad->mad_hdr.base_version == OPA_MGMT_BASE_VERSION)
		bv = "OPA";
	else if (in_mad->mad_hdr.base_version == IB_MGMT_BASE_VERSION)
		bv = "IB";
	else
		bv = "Unknown";

	switch (in_mad->mad_hdr.mgmt_class) {
	case IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE:
		mc = "DR";
		do_smp = true;
		break;
	case IB_MGMT_CLASS_SUBN_LID_ROUTED:
		mc = "LR";
		do_smp = true;
		break;
	case IB_MGMT_CLASS_SUBN_ADM:
		mc = "SA";
		do_sa = true;
		break;
	case IB_MGMT_CLASS_PERF_MGMT:
		mc = "Perf";
		do_pma = true;
		break;
	default:
		scnprintf(mc_buf, sizeof(mc_buf), "?class (0x%x)",
			  in_mad->mad_hdr.mgmt_class);
		mc = mc_buf;
		break;
	}

	switch (in_mad->mad_hdr.method) {
	case IB_MGMT_METHOD_GET:
		method_str = "GET";
		break;
	case IB_MGMT_METHOD_SET:
		method_str = "SET";
		break;
	case IB_MGMT_METHOD_GET_RESP:
		method_str = "GET_RESP";
		break;
	case IB_MGMT_METHOD_SEND:
		method_str = "SEND";
		break;
	case IB_MGMT_METHOD_TRAP:
		method_str = "TRAP";
		break;
	case IB_MGMT_METHOD_REPORT:
		method_str = "REPORT";
		break;
	case IB_MGMT_METHOD_REPORT_RESP:
		method_str = "REPORT_RESP";
		break;
	case IB_MGMT_METHOD_TRAP_REPRESS:
		method_str = "TRAP_REPRESS";
		break;
	default:
		scnprintf(method_buf, sizeof(method_buf), "?method (0x%x)",
			  in_mad->mad_hdr.method);
		method_str = method_buf;
		break;
	}

	extra[0] = 0;
	if (do_smp) {
		scnprintf(extra, sizeof(extra), "attr_id 0x%04x %s",
			  be16_to_cpu(in_mad->mad_hdr.attr_id),
			  smp_attr_id_str(in_mad->mad_hdr.attr_id));
	}
	if (do_sa) {
		scnprintf(extra, sizeof(extra), "attr_id 0x%04x %s",
			  be16_to_cpu(in_mad->mad_hdr.attr_id),
			  sa_attr_id_str(in_mad->mad_hdr.attr_id));
	}
	if (do_pma) {
		scnprintf(extra, sizeof(extra), "attr_id 0x%04x %s",
			  be16_to_cpu(in_mad->mad_hdr.attr_id),
			  pma_attr_id_str(in_mad->mad_hdr.attr_id));
	}

	pr_notice("hfi2_%u.%u: %s(%c): %s %s MAD %s %s tid 0x%llx\n", dd->unit,
		  port - 1, caller, rcv ? 'r' : 's', bv, mc, method_str, extra,
		  be64_to_cpu(in_mad->mad_hdr.tid));
}

#define OPA_ATTRIB_ID_MCTP_OVER_MAD cpu_to_be16(0xff30)
#define OPA_ATTRIB_MOD_MCTP_INCOMING cpu_to_be32(0x0001)
/*
 * Send a MAD to CPORT over MCTXT as a pass-through.
 * We always use 9B for now.
 */
int hfi2_cport_send_only_mad(struct hfi2_devdata *dd, u8 sb, const void *mad,
			     int len)
{
	u8 *buf;
	int size = len + MAD_9B_OFFSET;
	int ret;

	if (size > OPA_MGMT_MAD_SIZE) {
#ifdef CPORT_MAD_TRACE
		dd_dev_info(dd, "MCTXT MAD length %d > %d, error\n", size,
			    OPA_MGMT_MAD_SIZE);
		/* dump only enough for identification */
		print_hex_dump(KERN_INFO, "MCTXT MAD ", DUMP_PREFIX_OFFSET, 16,
			       1, mad, 64, false);
#endif
		return -EFBIG;
	}
	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf + MAD_9B_OFFSET, mad, size - MAD_9B_OFFSET);
	((struct ib_header *)buf)->u.oth.u.ud.deth[1] = cpu_to_be32(2);
	ret = hfi2_cport_send_notif(dd, CH_OP_MAD_9B, sb, buf, size,
				    hfi2_cport_adm_to * HZ);
	kfree(buf);
	return ret;
}

/*
 * Send a MAD to CPORT over MCTXT and wait for response.
 * We always use 9B.
 */
int hfi2_cport_send_recv_mad(struct hfi2_devdata *dd, u8 sb, const void *mad,
			     int len, void *omad, size_t *omad_len)
{
	u8 *buf;
	int size = len + MAD_9B_OFFSET;
	int ret;
	void *rsp = NULL;
	int rsp_len = 0;
	long to;

	to = cport_mad_to <= 0 ? MAX_SCHEDULE_TIMEOUT : cport_mad_to * HZ;
	if (size > OPA_MGMT_MAD_SIZE) {
		/* too big for MCTXT - truncate */
#ifdef CPORT_MAD_TRACE
		dd_dev_info(dd, "MCTXT MAD length %d > %d, truncating\n", size,
			    OPA_MGMT_MAD_SIZE);
		/* dump only enough for identification */
		print_hex_dump(KERN_INFO, "MCTXT MAD ", DUMP_PREFIX_OFFSET, 16,
			       1, mad, 64, false);
#endif
		size = 2048;
	}
	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf + MAD_9B_OFFSET, mad, size - MAD_9B_OFFSET);
	((struct ib_header *)buf)->u.oth.u.ud.deth[1] = cpu_to_be32(2);

	ret = hfi2_cport_send_req(dd, CH_OP_MAD_9B, sb, buf, size, &rsp,
				  &rsp_len, to);
	kfree(buf);
	if (ret)
		goto out;
	if (rsp_len < MAD_9B_OFFSET) {
		ret = -EINVAL;
		goto out;
	}
	rsp_len -= MAD_9B_OFFSET;
	if (rsp_len > *omad_len) {
		dd_dev_warn(dd,
			    "CPORT response length 0x%x > 0x%lx, truncating\n",
			    rsp_len, *omad_len);
		rsp_len = *omad_len;
	}
	memcpy(omad, rsp + MAD_9B_OFFSET, rsp_len);
	*omad_len = rsp_len;
out:
	kfree(rsp);
	return ret;
}

/**
 * hfi2_cport_process_mad - redirect an incoming MAD packet to CPORT for processing
 * @ibdev: the infiniband device this packet came in on
 * @mad_flags: MAD flags
 * @port: the port number this packet came in on
 * @in_wc: the work completion entry for this packet
 * @in_grh: the global route header for this packet
 * @in_mad: the incoming MAD
 * @out_mad: any outgoing MAD reply
 * @out_mad_size: size of the outgoing MAD reply
 * @out_mad_pkey_index: used to apss back the packet key index
 */
int hfi2_cport_process_mad(struct ib_device *ibdev, int mad_flags, u32 port,
			   const struct ib_wc *in_wc,
			   const struct ib_grh *in_grh,
			   const struct ib_mad *in_mad, struct ib_mad *out_mad,
			   size_t *out_mad_size, u16 *out_mad_pkey_index)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	u8 sb = port; /* 1.. */
	int mad_len;
	int ret;
	bool pass = false;
	int mad_result;

#ifdef CPORT_MAD_TRACE
#ifndef GET_PORT_INFO_DEBUG
	if (in_mad->mad_hdr.base_version != OPA_MGMT_BASE_VERSION ||
	    in_mad->mad_hdr.class_version != OPA_SM_CLASS_VERSION ||
	    in_mad->mad_hdr.method != IB_MGMT_METHOD_GET ||
	    in_mad->mad_hdr.attr_id != IB_SMP_ATTR_PORT_INFO)
#endif
		print_mad(dd, port, in_mad, __func__, !!in_grh);
#endif
	switch (in_mad->mad_hdr.mgmt_class) {
	case IB_MGMT_CLASS_SUBN_LID_ROUTED:
	case IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE:
		if (in_mad->mad_hdr.base_version == OPA_MGMT_BASE_VERSION) {
			if (in_mad->mad_hdr.attr_id ==
			    OPA_ATTRIB_ID_MCTP_OVER_MAD) {
				if (in_mad->mad_hdr.attr_mod &
				    OPA_ATTRIB_MOD_MCTP_INCOMING) {
					mad_result = IB_MAD_RESULT_SUCCESS;
					goto done;
				}
				pass = (in_mad->mad_hdr.method ==
					IB_MGMT_METHOD_GET_RESP);
				goto pass_thru;
			}
			if (in_mad->mad_hdr.method == IB_MGMT_METHOD_TRAP ||
			    in_mad->mad_hdr.method == IB_MGMT_METHOD_REPORT ||
			    in_mad->mad_hdr.method ==
				    IB_MGMT_METHOD_REPORT_RESP ||
			    in_mad->mad_hdr.method == IB_MGMT_METHOD_GET_RESP) {
				mad_result = IB_MAD_RESULT_SUCCESS;
				goto done;
			}
			if (in_mad->mad_hdr.method ==
			    IB_MGMT_METHOD_TRAP_REPRESS) {
				/* Need this to prevent TRAP storms */
				return hfi2_process_mad(ibdev, mad_flags, port,
							in_wc, in_grh, in_mad,
							out_mad, out_mad_size,
							out_mad_pkey_index);
			}
		}
		break; /* pass to CPORT */
	case IB_MGMT_CLASS_PERF_MGMT:
		break; /* pass to CPORT */
	default:
		mad_result = IB_MAD_RESULT_SUCCESS;
		goto done;
	}
	/* short-circuit all responses - no processing needed */
	if (ib_response_mad(&in_mad->mad_hdr)) {
		/*
		 * The ib_mad module will call us to process responses before
		 * checking for other consumers.  Tell the caller to process
		 * it normally.
		 */
		mad_result = IB_MAD_RESULT_SUCCESS;
		goto done;
	}
pass_thru:
	/* if this MAD arrived on MCTXT UMAD, do not ever send back to CPORT */
	if (in_wc && in_wc->vendor_err == OPA_WC_MCTXT_UMAD) {
#ifdef CPORT_UMAD_TRACE
		print_hex_dump(KERN_INFO, "MCTXT UMAD2 ", DUMP_PREFIX_OFFSET,
			       16, 1, in_mad, 64, false);
#endif
		mad_result = IB_MAD_RESULT_SUCCESS;
		goto done;
	}
	/*
	 * The alternative is to pass the MAD to CPORT using a hfi2_loopback
	 * port.
	 *
	 * Can this routine sleep? Regardless of which method passes
	 * the MAD to CPORT, we need to wait for the response.
	 */
	if (in_mad->mad_hdr.base_version == OPA_MGMT_BASE_VERSION)
		mad_len = in_wc ? in_wc->byte_len : sizeof(struct opa_mad);
	else
		mad_len = sizeof(struct ib_mad);

	if (pass) {
		mad_result = IB_MAD_RESULT_SUCCESS;
		if (dd->is_vf)
			ret = hfi2_vf2pf_send_only_mad(dd, sb, in_mad, mad_len);
		else
			ret = hfi2_cport_send_only_mad(dd, sb, in_mad, mad_len);
		if (ret)
			mad_result = IB_MAD_RESULT_FAILURE;
		return mad_result;
	}
	if (dd->is_vf) {
		long to = cport_mad_to <= 0 ? MAX_SCHEDULE_TIMEOUT :
					      (cport_mad_to + 2) * HZ;

		ret = hfi2_vf2pf_send_recv_mad(dd, sb, in_mad, mad_len, out_mad,
					       out_mad_size, to);
	} else {
		ret = hfi2_cport_send_recv_mad(dd, sb, in_mad, mad_len, out_mad,
					       out_mad_size);
	}
	if (ret) {
		mad_result = IB_MAD_RESULT_FAILURE;
		goto done;
	}
	if (*out_mad_size > 0)
		mad_result = IB_MAD_RESULT_SUCCESS | IB_MAD_RESULT_REPLY;
	else
		mad_result = IB_MAD_RESULT_SUCCESS;
done:
#ifdef CPORT_MAD_TRACE
#ifndef GET_PORT_INFO_DEBUG
	if (in_mad->mad_hdr.base_version != OPA_MGMT_BASE_VERSION ||
	    in_mad->mad_hdr.class_version != OPA_SM_CLASS_VERSION ||
	    in_mad->mad_hdr.method != IB_MGMT_METHOD_GET ||
	    in_mad->mad_hdr.attr_id != IB_SMP_ATTR_PORT_INFO)
#endif
		print_mad_ret(dd, port, out_mad, mad_result, __func__);
#endif
	return mad_result;
}

int hfi2_vf_process_mad(struct ib_device *ibdev, int mad_flags, u32 port,
			const struct ib_wc *in_wc, const struct ib_grh *in_grh,
			const struct ib_mad *in_mad, struct ib_mad *out_mad,
			size_t *out_mad_size, u16 *out_mad_pkey_index)
{
	/*
	 * If this requires any different/additional processing,
	 * place that here. hfi2_cport_process_mad() checks dd->is_vf to
	 * determine how to pass the MAD (ultimately) to CPORT.
	 */
	return hfi2_cport_process_mad(ibdev, mad_flags, port, in_wc, in_grh,
				      in_mad, out_mad, out_mad_size,
				      out_mad_pkey_index);
}

/**
 * hfi2_process_mad - process an incoming MAD packet
 * @ibdev: the infiniband device this packet came in on
 * @mad_flags: MAD flags
 * @port: the port number this packet came in on
 * @in_wc: the work completion entry for this packet
 * @in_grh: the global route header for this packet
 * @in_mad: the incoming MAD
 * @out_mad: any outgoing MAD reply
 * @out_mad_size: size of the outgoing MAD reply
 * @out_mad_pkey_index: used to apss back the packet key index
 *
 * Returns IB_MAD_RESULT_SUCCESS if this is a MAD that we are not
 * interested in processing.
 *
 * Note that the verbs framework has already done the MAD sanity checks,
 * and hop count/pointer updating for IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE
 * MADs.
 *
 * This is called by the ib_mad module.
 */
int hfi2_process_mad(struct ib_device *ibdev, int mad_flags, u32 port,
		     const struct ib_wc *in_wc, const struct ib_grh *in_grh,
		     const struct ib_mad *in_mad, struct ib_mad *out_mad,
		     size_t *out_mad_size, u16 *out_mad_pkey_index)
{
	int ret;

#ifdef CPORT_MAD_TRACE
	print_mad(dd_from_ibdev(ibdev), port, in_mad, __func__, !!in_grh);
#endif
	switch (in_mad->mad_hdr.base_version) {
	case OPA_MGMT_BASE_VERSION:
		ret = hfi2_process_opa_mad(ibdev, mad_flags, port, in_wc,
					   in_grh, (struct opa_mad *)in_mad,
					   (struct opa_mad *)out_mad,
					   out_mad_size, out_mad_pkey_index);
		break;
	case IB_MGMT_BASE_VERSION:
		ret = hfi2_process_ib_mad(ibdev, mad_flags, port, in_wc, in_grh,
					  in_mad, out_mad);
		break;
	default:
		ret = IB_MAD_RESULT_FAILURE;
		break;
	}
#ifdef CPORT_MAD_TRACE
	print_mad_ret(dd_from_ibdev(ibdev), port, out_mad, ret, __func__);
#endif

	return ret;
}

/*
 * The driver received a GET or SET NODE_DESC from CPORT.
 * Currently, we only tell CPORT what the NodedDescription should be,
 * according to the IB device. We don't allow anyone to change that.
 */
static int cport_do_opa_nodedesc(struct hfi2_pportdata *ppd,
				 struct opa_smp *smp, u8 *data)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct opa_node_description *nd = (struct opa_node_description *)data;
	struct hfi2_ibdev *dev = &dd->verbs_dev;
	struct ib_device *ibdev = &dev->rdi.ibdev;

	memcpy(nd->data, ibdev->node_desc, sizeof(nd->data));
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "NODEDESC \"%s\"\n", nd->data);
#endif
	return 0;
}

static int cport_set_opa_nodeinfo(struct hfi2_pportdata *ppd,
				  struct opa_smp *smp, u8 *data)
{
	struct opa_node_info *ni = (struct opa_node_info *)data;

	ni->system_image_guid = hfi2_ib_hfi2_sys_image_guid;
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(
		ppd,
		"NODEINFO %04x:%02x%02x%02x type=%d nport=%d/%d guid sys=%016llx node=%016llx port=%016llx\n",
		be16_to_cpu(ni->device_id), ni->vendor_id[0], ni->vendor_id[1],
		ni->vendor_id[2], ni->node_type, ni->local_port_num,
		ni->num_ports, be64_to_cpu(ni->system_image_guid),
		be64_to_cpu(ni->node_guid), be64_to_cpu(ni->port_guid));
#endif
	return 0;
}

static int cport_get_opa_nodeinfo(struct hfi2_pportdata *ppd,
				  struct opa_smp *smp, u8 *data)
{
	struct opa_node_info *ni = (struct opa_node_info *)data;

	/* override with the system image */
	ni->system_image_guid = hfi2_ib_hfi2_sys_image_guid;
	return MSG_RSP_STATUS_OK;
}

/*
 * Take a GET_RESP PORT_INFO MAD and use it to update the hfi2 device data structures.
 * The entire MAD is referenced through 'smp'.
 */
int hfi2_update_from_opa_portinfo(struct hfi2_pportdata *ppd,
				  struct opa_smp *smp, struct opa_port_info *pi)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct ib_device *ibdev = &dd->verbs_dev.rdi.ibdev;
	struct ib_event event;
	struct hfi2_ibport *ibp = &ppd->ibport_data;
	u32 smlid;
	u32 lid;
	u8 ls_old, ls_new, ps_new;
	u8 lmc;
	u8 clientrereg;
	u8 vls;
	u8 msl;
	u8 crc_enabled;
	unsigned long flags;
	int i;
	u16 lse, lwe, mtu;
	u32 am = be32_to_cpu(smp->attr_mod);
	u32 start_of_sm_config = OPA_AM_START_SM_CFG(am);

	/*
	 * This code is modeled after __subn_set_opa_portinfo(), eliminating
	 * things that attempt to manipulate hardware or are otherwise handled
	 * by CPORT now. This routine DOES NOT change the port state in
	 * hardware or on the fabric, it simply makes the driver "view"
	 * match what CPORT tells us via an OPA_ATTRIB_ID_PORT_INFO MAD.
	 * It also implicitly trusts the information provided by CPORT.
	 */

	lid = be32_to_cpu(pi->lid);
	smlid = be32_to_cpu(pi->sm_lid);
	ls_old = hfi2_driver_lstate(ppd);
	clientrereg =
		(pi->clientrereg_subnettimeout & OPA_PI_MASK_CLIENT_REREGISTER);

	event.device = ibdev;
	event.element.port_num = ppd->port;

	ibp->rvp.mkey = pi->mkey;
	if (ibp->rvp.gid_prefix != pi->subnet_prefix) {
		ibp->rvp.gid_prefix = pi->subnet_prefix;
		event.event = IB_EVENT_GID_CHANGE;
		ib_dispatch_event(&event);

		/* Refresh ipoib with new subnet prefixes from updated ib_gid_table */
		event.event = IB_EVENT_CLIENT_REREGISTER;
		ib_dispatch_event(&event);
	}
	ibp->rvp.mkey_lease_period = be16_to_cpu(pi->mkey_lease_period);

	lmc = pi->mkeyprotect_lmc & OPA_PI_MASK_LMC;
	if (ppd->lid != lid || ppd->lmc != lmc) {
		ppd_dev_info(ppd, "port %d: setting LID/LMC 0x%x/0x%x\n",
			     ppd->port, lid, lmc);
		if (ppd->lid != lid)
			hfi2_set_uevent_bits(ppd, _HFI2_EVENT_LID_CHANGE_BIT);
		if (ppd->lmc != (pi->mkeyprotect_lmc & OPA_PI_MASK_LMC))
			hfi2_set_uevent_bits(ppd, _HFI2_EVENT_LMC_CHANGE_BIT);
		/* relevant parts of hfi2_set_lid() */
		ppd->lid = lid;
		ppd->lmc = lmc;
		hfi2_sdma_update_lmc(
			dd, ~((1U << lmc) - 1),
			lid >= be16_to_cpu(IB_MULTICAST_LID_BASE) ? 0 : lid);
		/* ppd_dev_info(ppd, "got a lid: 0x%x\n", lid); */
		event.event = IB_EVENT_LID_CHANGE;
		ib_dispatch_event(&event);

		if (HFI2_PORT_GUID_INDEX + 1 < HFI2_GUIDS_PER_PORT) {
			/* Manufacture GID from LID to support extended
			 * addresses
			 */
			ppd->guids[HFI2_PORT_GUID_INDEX + 1] =
				be64_to_cpu(OPA_MAKE_ID(lid));
			event.event = IB_EVENT_GID_CHANGE;
			ib_dispatch_event(&event);
		}
	}

	msl = pi->smsl & OPA_PI_MASK_SMSL;
	if (pi->partenforce_filterraw & OPA_PI_MASK_LINKINIT_REASON)
		ppd->linkinit_reason = (pi->partenforce_filterraw &
					OPA_PI_MASK_LINKINIT_REASON);

	if (smlid != ibp->rvp.sm_lid || msl != ibp->rvp.sm_sl) {
		ppd_dev_warn(ppd, "SubnSet(OPA_PortInfo cport) smlid 0x%x\n",
			     smlid);
		spin_lock_irqsave(&ibp->rvp.lock, flags);
		if (ibp->rvp.sm_ah) {
			if (smlid != ibp->rvp.sm_lid)
				hfi2_modify_qp0_ah(ibp, ibp->rvp.sm_ah, smlid);
			if (msl != ibp->rvp.sm_sl)
				rdma_ah_set_sl(&ibp->rvp.sm_ah->attr, msl);
		}
		spin_unlock_irqrestore(&ibp->rvp.lock, flags);
		if (smlid != ibp->rvp.sm_lid)
			ibp->rvp.sm_lid = smlid;
		if (msl != ibp->rvp.sm_sl)
			ibp->rvp.sm_sl = msl;
		event.event = IB_EVENT_SM_CHANGE;
		ib_dispatch_event(&event);
	}

	if (pi->link_down_reason == 0) {
		ppd->local_link_down_reason.sma = 0;
		ppd->local_link_down_reason.latest = 0;
	}

	if (pi->neigh_link_down_reason == 0) {
		ppd->neigh_link_down_reason.sma = 0;
		ppd->neigh_link_down_reason.latest = 0;
	}

	ppd->sm_trap_qp = be32_to_cpu(pi->sm_trap_qp);
	ppd->sa_qp = be32_to_cpu(pi->sa_qp);

	ppd->port_error_action = be32_to_cpu(pi->port_error_action);

	lwe = be16_to_cpu(pi->link_width.enabled);
	if (lwe) {
		if (lwe == OPA_LINK_WIDTH_RESET ||
		    lwe == OPA_LINK_WIDTH_RESET_OLD)
			set_link_width_enabled(ppd, ppd->link_width_supported);
		else if ((lwe & ~ppd->link_width_supported) == 0)
			set_link_width_enabled(ppd, lwe);
	}
	lwe = be16_to_cpu(pi->link_width_downgrade.enabled);
	if (lwe == OPA_LINK_WIDTH_RESET || lwe == OPA_LINK_WIDTH_RESET_OLD) {
		set_link_width_downgrade_enabled(
			ppd, ppd->link_width_downgrade_supported);
	} else if ((lwe & ~ppd->link_width_downgrade_supported) == 0) {
		if (lwe != ppd->link_width_downgrade_enabled)
			set_link_width_downgrade_enabled(ppd, lwe);
	}
	lse = be16_to_cpu(pi->link_speed.enabled);
	if (lse)
		set_link_speed_enabled(ppd, lse);

	ibp->rvp.mkeyprot = (pi->mkeyprotect_lmc & OPA_PI_MASK_MKEY_PROT_BIT) >>
			    6;
	ibp->rvp.vl_high_limit = be16_to_cpu(pi->vl.high_limit) & 0xFF;

	/*
	 * The only part of hfi2_set_mtu() that's required here is to
	 * determine the maximum MTU (ppd->ibmtu) and resulting maximum length
	 * (ppd->ibmaxlen). Do that here as we check every MTU.
	 */
	ppd->ibmtu = 0;
	for (i = 0; i < ppd->vls_supported; i++) {
		mtu = pi->neigh_mtu.pvlx_to_mtu[i / 2];
		if ((i % 2) == 0)
			mtu >>= 4;
		mtu = hfi2_enum_to_mtu(mtu & 0xF);
		if (mtu == 0xffff) {
			ppd_dev_warn(ppd, "%s: mtu invalid %d (0x%x)\n",
				     __func__, mtu,
				     (pi->neigh_mtu.pvlx_to_mtu[0] >> 4) & 0xF);
			mtu = hfi2_max_mtu; /* use a valid MTU */
		}
		if (ppd->ibmtu < mtu)
			ppd->ibmtu = mtu;
		if (ppd->vld[i].mtu != mtu) {
			ppd_dev_info(ppd, "MTU change on vl %d from %d to %d\n",
				     i, ppd->vld[i].mtu, mtu);
			ppd->vld[i].mtu = mtu;
		}
	}
	/* As per OPAV1 spec: VL15 must support and be configured
	 * for operation with a 2048 or larger MTU.
	 */
	mtu = hfi2_enum_to_mtu(pi->neigh_mtu.pvlx_to_mtu[15 / 2] & 0xF);
	if (mtu < 2048 || mtu == 0xffff)
		mtu = 2048;
	if (ppd->ibmtu < mtu)
		ppd->ibmtu = mtu;
	if (ppd->vld[15].mtu != mtu) {
		ppd_dev_info(ppd, "MTU change on vl 15 from %d to %d\n",
			     ppd->vld[15].mtu, mtu);
		ppd->vld[15].mtu = mtu;
	}
	ppd->ibmaxlen = ppd->ibmtu + hfi2_lrh_max_header_bytes(ppd);

	vls = pi->operational_vls & OPA_PI_MASK_OPERATIONAL_VL;
	/* hfi2_set_ib_cfg(HFI2_IB_CFG_OP_VLS) is hardware-neutral */
	if (vls)
		hfi2_set_ib_cfg(ppd, HFI2_IB_CFG_OP_VLS, vls);

	if (pi->mkey_violations == 0)
		ibp->rvp.mkey_violations = 0;

	if (pi->pkey_violations == 0)
		ibp->rvp.pkey_violations = 0;

	if (pi->qkey_violations == 0)
		ibp->rvp.qkey_violations = 0;

	ibp->rvp.subnet_timeout = pi->clientrereg_subnettimeout &
				  OPA_PI_MASK_SUBNET_TIMEOUT;

	crc_enabled = be16_to_cpu(pi->port_ltp_crc_mode);
	crc_enabled >>= 4;
	crc_enabled &= 0xf;
	if (crc_enabled)
		ppd->port_crc_mode_enabled = hfi2_port_ltp_to_cap(crc_enabled);

	ppd->is_active_optimize_enabled = !!(be16_to_cpu(pi->port_mode) &
					     OPA_PI_MASK_PORT_ACTIVE_OPTOMIZE);

	ls_new = port_states_to_logical_state(&pi->port_states);
	ps_new = port_states_to_phys_state(&pi->port_states);
	if (ls_old == IB_PORT_INIT) {
		if (start_of_sm_config) {
			if (ls_new == ls_old || ls_new == IB_PORT_ARMED)
				ppd->is_sm_config_started = 1;
		}
	}

	if (clientrereg) {
		event.event = IB_EVENT_CLIENT_REREGISTER;
		ib_dispatch_event(&event);
	}

	ppd->neighbor_normal = (pi->port_states.ledenable_offlinereason >> 4) &
			       1;

	/* Finally, perform "safe" version of set_port_states() */
	cport_set_port_states(ppd, pi, ls_new, ps_new);
	if (dd->is_sriov && !dd->is_vf) {
		/*
		 */
		i = hfi2_pf2vf_push_portinfo(ppd, smp, pi, VF2PF_SI_ALL);
		if (i)
			ppd_dev_err(ppd, "hfi2_pf2vf_push_portinfo failed %d\n",
				    i);
	}

	return 0;
}

#ifdef CPORT_MAD_TRACE
static const char *ps_state_name(struct opa_port_states *ps)
{
	static const char *const state_name[] = {
		[IB_PORT_NOP] = "NOP",
		[IB_PORT_DOWN] = "DOWN",
		[IB_PORT_INIT] = "INIT",
		[IB_PORT_ARMED] = "ARMED",
		[IB_PORT_ACTIVE] = "ACTIVE",
		[IB_PORT_ACTIVE_DEFER] = "ACTIVE_DEFER"
	};
	u8 ls = port_states_to_logical_state(ps);

	if (ls > IB_PORT_ACTIVE_DEFER)
		return "???";
	return state_name[ls];
}
#endif

/*
 * When snooping MADs between FM and CPORT, the following will be seen:
 * (FM typically observes the port in INIT when it starts)
 * FM sends a SET PortInfo MAD with the designated state. The driver
 * sees the GET response generated by the CPORT, and the state in this
 * response is not necessarily the state requested by the FM (if the
 * transition takes time, it may not be reflected in the MAD response).
 * The following focuses on the logical port state, with the physical
 * port state when applicable.
 *
 * current   FM sends    driver sees    new state
 * -------   --------    -----------    ---------
 * INIT      ARMED       ???            ARMED
 * ARMED     ACTIVE      ???            ACTIVE
 * ACTIVE    (nothing sent)             ACTIVE
 *
 * (*FM bounces port*)
 * (prev)    NOP/POLL    (prev)         INIT (eventually)
 *
 * (*FM changes LID*)
 * (prev)    ---?        (prev)         (prev)
 */
static int cport_set_opa_portinfo(struct hfi2_pportdata *ppd,
				  struct opa_smp *smp, u8 *data)
{
	struct opa_port_info *pi = (struct opa_port_info *)data;
	int ret = 0;

#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "OPA SET PORT_INFO MAD TID %llx amod %x\n",
		     be64_to_cpu(smp->tid), be32_to_cpu(smp->attr_mod));
#endif
	/* Note: what we have here is actually a GET_RESP, in spite of
	 * appearing to be a SET.
	 */
	ret = hfi2_update_from_opa_portinfo(ppd, smp, pi);
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd,
		     "PORTINFO %llx: %s %04x active speed=%04x width=%04x\n",
		     be64_to_cpu(smp->tid), ps_state_name(&pi->port_states),
		     be32_to_cpu(pi->lid), be16_to_cpu(pi->link_speed.active),
		     be16_to_cpu(pi->link_width.active));

#endif
	return ret;
}

static int cport_set_opa_pkeytbl(struct hfi2_pportdata *ppd,
				 struct opa_smp *smp, u8 *data)
{
	struct hfi2_devdata *dd = ppd->dd;
	u32 am = be32_to_cpu(smp->attr_mod);
	u32 n_blocks_sent = OPA_AM_NBLK(am);
	int npk_sent = n_blocks_sent * OPA_PARTITION_TABLE_BLK_SIZE;
	u32 start_block = am & 0x7ff;
	u16 *p = (u16 *)data;
	__be16 *q = (__be16 *)data;
	int i;
	u32 n_blocks_avail;
	u32 npkeys = hfi2_get_npkeys(dd);
	u32 size = 0;
	u32 max_len = (u32)opa_get_smp_data_size(smp);
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	if (n_blocks_sent == 0)
		goto out;
	n_blocks_avail = DIV_ROUND_UP(npkeys, OPA_PARTITION_TABLE_BLK_SIZE);
	size = sizeof(u16) * npk_sent;
	if (smp_length_check(size, max_len))
		goto out;
	if (start_block + n_blocks_sent > n_blocks_avail ||
	    n_blocks_sent > OPA_NUM_PKEY_BLOCKS_PER_SMP)
		goto out;

	/* must convert to little-endian for set_pkeys() */
	for (i = 0; i < npk_sent; i++)
		p[i] = be16_to_cpu(q[i]);
	if (set_pkeys(dd, ppd->port, start_block, n_blocks_sent, p) == 0)
		sts = 0;
	/* must convert back to big-endian for response */
	for (i = 0; i < npk_sent; i++)
		q[i] = cpu_to_be16(p[i]);
out:
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "P_KEYTBL start_block %d, %d pkeys (%d)\n",
		     start_block, npk_sent, sts);
#endif
	return sts;
}

static int cport_set_opa_slsc(struct hfi2_pportdata *ppd, struct opa_smp *smp,
			      u8 *data)
{
	struct hfi2_ibport *ibp = &ppd->ibport_data;
	u8 *p = data;
	int i;

	/* always update driver tables with CPORT contents */
	for (i = 0; i < ARRAY_SIZE(ibp->sl_to_sc); i++)
		ibp->sl_to_sc[i] = *p++;

#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "SL_TO_SC MAPPING: copied\n");
#endif
	return 0;
}

static int cport_set_opa_vlarb(struct hfi2_pportdata *ppd, struct opa_smp *smp,
			       u8 *data)
{
	u32 am = be32_to_cpu(smp->attr_mod);
	u32 num_ports = OPA_AM_NPORT(am);
	u8 section = (am & 0x00ff0000) >> 16;
	u8 *p = data;
	u32 max_len = (u32)opa_get_smp_data_size(smp);
	int size = 256;
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	if (num_ports != 1 || smp_length_check(size, max_len))
		goto out;

	switch (section) {
	case OPA_VLARB_LOW_ELEMENTS:
		(void)hfi2_fm_set_table(ppd, FM_TBL_VL_LOW_ARB, p);
		sts = 0;
		break;
	case OPA_VLARB_HIGH_ELEMENTS:
		(void)hfi2_fm_set_table(ppd, FM_TBL_VL_HIGH_ARB, p);
		sts = 0;
		break;
	case OPA_VLARB_PREEMPT_ELEMENTS:
	case OPA_VLARB_PREEMPT_MATRIX:
	default:
		break;
	}
out:
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "VL_ARBIT: %d (%d)\n", section, sts);
#endif
	return sts;
}

static int cport_set_opa_scsl(struct hfi2_pportdata *ppd, struct opa_smp *smp,
			      u8 *data)
{
	struct hfi2_ibport *ibp = &ppd->ibport_data;
	u8 *p = data;
	int i;

	/* always update driver tables with CPORT contents */
	for (i = 0; i < ARRAY_SIZE(ibp->sc_to_sl); i++)
		ibp->sc_to_sl[i] = *p++;

#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "SC_TO_SL MAPPING: copied\n");
#endif
	return 0;
}

static int cport_set_opa_scvlr(struct hfi2_pportdata *ppd, struct opa_smp *smp,
			       u8 *data)
{
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	sts = 0;
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "SC_TO_VLR MAPPING (%d)\n", sts);
#endif
	return sts;
}

void hfi2_update_sc2vlt(struct hfi2_pportdata *ppd, void *data, bool filter)
{
	write_seqlock_irq(&ppd->sc2vl_lock);
	memcpy(ppd->sc2vl, data, sizeof(ppd->sc2vl));
	if (filter)
		filter_sc2vlt(ppd->sc2vl, true);
	write_sequnlock_irq(&ppd->sc2vl_lock);
}

static int cport_set_opa_scvlt(struct hfi2_pportdata *ppd, struct opa_smp *smp,
			       u8 *data)
{
	u32 am = be32_to_cpu(smp->attr_mod);
	u32 n_blocks = OPA_AM_NBLK(am);
	int async_update = OPA_AM_ASYNC(am);
	int lstate;
	size_t size = 4 * sizeof(u64); /* see __subn_set_opa_sc_to_vlt() */
	u32 max_len = (u32)opa_get_smp_data_size(smp);
	int sts = MSG_RSP_STATUS_INVALID_STATE;
	int res;

	if (n_blocks != 1 || async_update || smp_length_check(size, max_len))
		goto out;
	lstate = hfi2_driver_lstate(ppd);
	if (!async_update &&
	    (lstate == IB_PORT_ARMED || lstate == IB_PORT_ACTIVE))
		goto out;

	/*
	 * set_sc2vlt_tables() writes CSRs, and CPORT now maintains those.
	 * Also, need to avoid altering (filter_sc2vlt()) the MAD data since
	 * it needs to be sent back to requesting node.
	 */
	hfi2_update_sc2vlt(ppd, data, true);
	sts = 0;
	if (ppd->dd->is_sriov && !ppd->dd->is_vf) {
		/*
		 * That requires making a copy of 'smp' since caller
		 * may destroy that on return from here.
		 */
		res = hfi2_pf2vf_push_sc2vlt(ppd, VF2PF_SI_ALL);
		if (res)
			ppd_dev_err(ppd, "hfi2_pf2vf_push_sc2vlt failed %d\n",
				    res);
	}
out:
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "SC_TO_VLT MAPPING (%d)\n", sts);
#endif
	return sts;
}

static int cport_set_opa_scvlnt(struct hfi2_pportdata *ppd, struct opa_smp *smp,
				u8 *data)
{
	u32 am = be32_to_cpu(smp->attr_mod);
	u32 n_blocks = OPA_AM_NPORT(am);
	int lstate;
	int size = sizeof(struct sc2vlnt);
	u32 max_len = (u32)opa_get_smp_data_size(smp);
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	if (n_blocks != 1 || smp_length_check(size, max_len))
		goto out;
	lstate = hfi2_driver_lstate(ppd);
	if (lstate == IB_PORT_ARMED || lstate == IB_PORT_ACTIVE)
		goto out;

	/* CPORT controls sc2vlnt */
	sts = 0;

out:
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "SC_TO_VLNT MAPPING (%d)\n", sts);
#endif
	return sts;
}

static int cport_set_opa_bufctl(struct hfi2_pportdata *ppd, struct opa_smp *smp,
				u8 *data)
{
	u32 am = be32_to_cpu(smp->attr_mod);
	u32 num_ports = OPA_AM_NPORT(am);
	u32 max_len = (u32)opa_get_smp_data_size(smp);
	struct buffer_control *p = (struct buffer_control *)data;
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	if (num_ports != 1 || smp_length_check(sizeof(*p), max_len))
		goto out;

	sts = 0;
out:
#ifdef CPORT_MAD_TRACE
	ppd_dev_info(ppd, "BUFCTL %d/%d (%d)\n", ppd->port, num_ports, sts);
#endif
	return sts;
}

static int cport_set_opa_aggr(struct hfi2_pportdata *ppd, struct opa_smp *smp)
{
	return 0;
}

static int cport_subn_set_opa(struct hfi2_pportdata *ppd, struct opa_smp *smp)
{
	int sts = MSG_RSP_STATUS_INVALID_STATE;
	u8 *data = opa_get_smp_data(smp);

	switch (smp->attr_id) {
	case IB_SMP_ATTR_NODE_DESC:
		sts = cport_do_opa_nodedesc(ppd, smp, data);
		break;
	case IB_SMP_ATTR_NODE_INFO:
		sts = cport_set_opa_nodeinfo(ppd, smp, data);
		break;
	case OPA_ATTRIB_ID_PORT_INFO:
		sts = cport_set_opa_portinfo(ppd, smp, data);
		break;
	case IB_SMP_ATTR_PKEY_TABLE:
		sts = cport_set_opa_pkeytbl(ppd, smp, data);
		break;
	case OPA_ATTRIB_ID_SL_TO_SC_MAP:
		sts = cport_set_opa_slsc(ppd, smp, data);
		break;
	case IB_SMP_ATTR_VL_ARB_TABLE:
		sts = cport_set_opa_vlarb(ppd, smp, data);
		break;
	case OPA_ATTRIB_ID_SC_TO_SL_MAP:
		sts = cport_set_opa_scsl(ppd, smp, data);
		break;
	case OPA_ATTRIB_ID_SC_TO_VLR_MAP:
		sts = cport_set_opa_scvlr(ppd, smp, data);
		break;
	case OPA_ATTRIB_ID_SC_TO_VLT_MAP:
		sts = cport_set_opa_scvlt(ppd, smp, data);
		break;
	case OPA_ATTRIB_ID_SC_TO_VLNT_MAP:
		sts = cport_set_opa_scvlnt(ppd, smp, data);
		break;
	case OPA_ATTRIB_ID_BUFFER_CONTROL_TABLE:
		sts = cport_set_opa_bufctl(ppd, smp, data);
		break;
	case OPA_ATTRIB_ID_CONGESTION_INFO:
		sts = 0;
		break;
	case OPA_ATTRIB_ID_HFI_CONGESTION_LOG:
		sts = 0;
		break;
	case OPA_ATTRIB_ID_HFI_CONGESTION_SETTING:
		sts = 0; /* accept, nothing to do */
		break;
	case OPA_ATTRIB_ID_CONGESTION_CONTROL_TABLE:
		sts = 0;
		break;
	case IB_SMP_ATTR_NOTICE:
		sts = 0;
		break;
	case IB_SMP_ATTR_SM_INFO:
		sts = 0;
		break;
	case IB_SMP_ATTR_LED_INFO:
		sts = 0;
		break;
	case OPA_ATTRIB_ID_CABLE_INFO:
		sts = 0;
		break;
	case OPA_ATTRIB_ID_AGGREGATE:
		sts = cport_set_opa_aggr(ppd, smp);
		break;
	default:
		break;
	}
	return sts;
}

static int cport_subn_opa(struct hfi2_pportdata *ppd, struct opa_mad *mad)
{
	struct opa_smp *smp = (struct opa_smp *)mad;
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	if (smp->class_version != OPA_SM_CLASS_VERSION) {
		smp->status |= IB_SMP_UNSUP_VERSION;
		goto out;
	}
	switch (smp->method) {
	case IB_MGMT_METHOD_SET:
		sts = cport_subn_set_opa(ppd, smp);
		break;
	case IB_MGMT_METHOD_GET:
		if (smp->attr_id == IB_SMP_ATTR_NODE_DESC) {
			sts = cport_do_opa_nodedesc(ppd, smp,
						    opa_get_smp_data(smp));
			break;
		}
		if (smp->attr_id == IB_SMP_ATTR_NODE_INFO) {
			sts = cport_get_opa_nodeinfo(ppd, smp,
						     opa_get_smp_data(smp));
			break;
		}
#ifdef CPORT_MAD_TRACE
#ifndef GET_PORT_INFO_DEBUG
		if (smp->attr_id != IB_SMP_ATTR_PORT_INFO)
#endif
			ppd_dev_info(
				ppd,
				"%s: approving GET 0x%04x without doing anything\n",
				__func__, smp->attr_id);
#endif
		sts = 0;
		break;
	default:
		break;
	}
out:
	return sts;
}

static int hfi2_opa_mad_cport(struct hfi2_pportdata *ppd, struct opa_mad *mad)
{
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	switch (mad->mad_hdr.mgmt_class) {
	case IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE:
	case IB_MGMT_CLASS_SUBN_LID_ROUTED:
		sts = cport_subn_opa(ppd, mad);
		break;
	case IB_MGMT_CLASS_PERF_MGMT:
		sts = 0; /* accept, nothing to do */
		break;
	default:
		break;
	}
	return sts;
}

static int cport_get_ib_nodeinfo(struct hfi2_pportdata *ppd, struct ib_smp *smp)
{
	struct ib_node_info *nip = (struct ib_node_info *)&smp->data;

	/* This is already in network order */
	nip->sys_guid = hfi2_ib_hfi2_sys_image_guid;

	return 0;
}

static int cport_subn_get_ib(struct hfi2_pportdata *ppd, struct ib_smp *smp)
{
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	switch (smp->attr_id) {
	case IB_SMP_ATTR_NODE_INFO:
		sts = cport_get_ib_nodeinfo(ppd, smp);
		break;
	default:
		smp->status |= IB_SMP_UNSUP_METH_ATTR;
		break;
	}
	if (sts)
		ppd_dev_err(ppd, "%s: ret %d\n", __func__, sts);
	return sts;
}

static int cport_subn_ib(struct hfi2_pportdata *ppd, struct ib_mad *mad)
{
	struct ib_smp *smp = (struct ib_smp *)mad;
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	if (smp->class_version != 1) {
		smp->status |= IB_SMP_UNSUP_VERSION;
		goto out;
	}
	switch (smp->method) {
	case IB_MGMT_METHOD_GET:
		sts = cport_subn_get_ib(ppd, smp);
		break;
	default:
		smp->status |= IB_SMP_UNSUP_METHOD;
		break;
	}
out:
	if (sts)
		ppd_dev_err(ppd, "%s: ret %d\n", __func__, sts);
	return sts;
}

static int hfi2_ib_mad_cport(struct hfi2_pportdata *ppd, struct ib_mad *mad)
{
	int sts = MSG_RSP_STATUS_INVALID_STATE;

	switch (mad->mad_hdr.mgmt_class) {
	case IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE:
	case IB_MGMT_CLASS_SUBN_LID_ROUTED:
		sts = cport_subn_ib(ppd, mad);
		break;
	case IB_MGMT_CLASS_PERF_MGMT:
		sts = 0; /* accept, nothing to do */
		break;
	}
	if (sts)
		ppd_dev_err(ppd, "%s: ret %d\n", __func__, sts);
	return sts;
}

static inline int get_mad_offset(void *hdr, bool is16B)
{
	if (is16B) {
		/* 16B header - look at the L4 type */
		switch (hfi2_16B_get_l4(hdr)) {
		case OPA_16B_L4_FM:
			return MAD_16B_FM_OFFSET;
		case OPA_16B_L4_IB_LOCAL:
		case OPA_16B_L4_IB_GLOBAL:
			return MAD_16B_IB_OFFSET;
		case OPA_16B_L4_9B:
			return MAD_9B_OFFSET;
		default:
			/* can't determine MAD header offset */
			return -1;
		}

	} else {
		/* expect 9B */
		return MAD_9B_OFFSET;
	}
}

/*
 * CPORT is communicating with some user-space application using MADs.
 * We simply forward these MADs to IB verbs to handle as for normal
 * receives.
 */
static int cport_umad_handler(struct hfi2_devdata *dd, u8 op, u8 sideband,
			      void *payload, int len, void *handle)
{
	struct ib_smp *mad;
	struct hfi2_pportdata *ppd;
	struct rvt_qp *qp0;
	struct ib_wc wc;
	int mad_offset;
	u32 tlen;
	u32 slid, dlid;
	u16 pkey;
	int port;
	int ret = 0;
	int rc;

	mad_offset = get_mad_offset(payload, op == CH_OP_UMAD_16B);
	if (mad_offset < 0)
		return MSG_RSP_STATUS_INVALID_STATE;
	if (len < mad_offset + (int)sizeof(struct ib_mad))
		return MSG_RSP_STATUS_INVALID_STATE;
	mad = payload + mad_offset;

	/* This MAD really must be IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE,
	 * but this routine doesn't care.
	 */

	port = sideband & 0x7;
	if (!port)
		port = 1;
	if (port > dd->num_pports) {
		port -= dd->num_pports;
		if (port > dd->num_pports)
			port = 1;
	}
	ppd = &dd->pport[port - 1];
	rcu_read_lock();
	qp0 = rcu_dereference(ppd->ibport_data.rvp.qp[0]);
	if (!qp0) {
		ppd_dev_err(ppd, "QP0 is NULL\n");
		ret = MSG_RSP_STATUS_INVALID_STATE;
		goto out;
	}

	tlen = len - mad_offset;

	wc.ex.imm_data = 0;
	wc.wc_flags = IB_WC_IP_CSUM_OK;
	wc.byte_len = tlen + sizeof(struct ib_grh);
	/* Ignore RVT_R_REUSE_SGE here, as done by hfi2_rc_rcv and hfi2_uc_rcv */
	rc = rvt_get_rwqe(qp0, false);
	if (rc <= 0) {
		if (rc < 0) {
			rvt_rc_error(qp0, IB_WC_LOC_QP_OP_ERR);
			ret = MSG_RSP_STATUS_INVALID_STATE;
		} else {
			ppd->ibport_data.rvp.n_vl15_dropped++;
			ret = 0; /* Silently drop, don't spam console */
		}
		goto out;
	}
	rvt_skip_sge(&qp0->r_sge, sizeof(struct ib_grh), true);
	rvt_copy_sge(qp0, &qp0->r_sge, payload + mad_offset,
		     wc.byte_len - sizeof(struct ib_grh), true, false);
	rvt_put_ss(&qp0->r_sge);
	if (!test_and_clear_bit(RVT_R_WRID_VALID, &qp0->r_aflags)) {
		/* Something went wrong since rvt_get_rwqe, assume it was transient */
		ret = MSG_RSP_STATUS_RETRY;
		goto out;
	}

	wc.wr_id = qp0->r_wr_id;
	wc.status = IB_WC_SUCCESS;
	wc.opcode = IB_WC_RECV;
	/* tag MAD so we don't reflect it back to CPORT */
	wc.vendor_err = OPA_WC_MCTXT_UMAD;
	wc.qp = &qp0->ibqp;
	wc.src_qp = 0; /* MCTXT has no QP, so just use 0 */
	/*
	 * Check for firmware update MADs and always use pkey index 0.
	 */
	if (mad->base_version == OPA_MGMT_BASE_VERSION &&
	    mad->mgmt_class == IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE &&
	    mad->attr_id == OPA_ATTRIB_ID_MCTP_OVER_MAD) {
		wc.pkey_index = 0;
	} else {
		if (op == CH_OP_UMAD_16B) {
			pkey = hfi2_16B_get_pkey(payload);
		} else {
			struct ib_header *hdr = payload;
			struct ib_other_headers *ohdr;
			u8 lnh = ib_get_lnh(hdr);

			if (lnh == HFI2_LRH_GRH)
				ohdr = &hdr->u.l.oth;
			else
				ohdr = &hdr->u.oth;
			pkey = ib_bth_get_pkey(ohdr);
		}
		rc = hfi2_lookup_pkey_idx(&ppd->ibport_data,
					  pkey); /* should never fail */
		wc.pkey_index = rc < 0 ? 0 : rc;
	}
	if (mad->mgmt_class == IB_MGMT_CLASS_SUBN_DIRECTED_ROUTE) {
		if (mad->base_version == OPA_MGMT_BASE_VERSION) {
			struct opa_smp *omad = (struct opa_smp *)mad;

			slid = be32_to_cpu(omad->route.dr.dr_slid);
			dlid = be32_to_cpu(omad->route.dr.dr_dlid);
		} else {
			slid = be16_to_cpu(mad->dr_slid);
			dlid = be16_to_cpu(mad->dr_dlid);
		}
	} else {
		if (op == CH_OP_UMAD_16B) {
			slid = hfi2_16B_get_slid(payload);
			dlid = hfi2_16B_get_dlid(payload);
		} else {
			slid = ib_get_slid(payload);
			dlid = ib_get_dlid(payload);
		}
	}
	wc.slid = slid;
	wc.dlid_path_bits = dlid & ((1 << ppd->lmc) - 1);
	wc.sl = ppd->ibport_data.sc_to_sl[15];
	wc.port_num = qp0->port_num;

#ifdef CPORT_UMAD_TRACE
	print_hex_dump(KERN_INFO, "MCTXT UMAD ", DUMP_PREFIX_OFFSET, 16, 1, mad,
		       64, false);
#endif
	rvt_recv_cq(qp0, &wc, false);
out:
	rcu_read_unlock();
	return ret;
}

/*
 * CPORT sends us the MAD response, so our only actions are to
 * modify the response to add more data and/or take note of any
 * implications of the MAD.
 */
static int cport_mad_handler(struct hfi2_devdata *dd, u8 op, u8 sideband,
			     void *payload, int len, void *handle)
{
	struct ib_mad *mad = NULL;
	struct hfi2_pportdata *ppd = NULL;
	int sts = MSG_RSP_STATUS_INVALID_STATE;
	int mad_offset;
	int port;
#ifdef DEBUG_MAD_MCTXT
	static char buf[128];
	char *s;
	u8 *p = payload;
	int n = len, x;

	dd_dev_info(dd, "MAD MCTXT op %d %02x\n", op, sideband);
	while (n > 0) {
		s = buf;
		s += sprintf(s, "%04x:", (int)(p - (u8 *)payload));
		for (x = 0; x < 16 && n > 0; ++x) {
			s += sprintf(s, " %02x", *p++);
			--n;
		}
		dd_dev_info(dd, "%s\n", buf);
	}
#endif
	mad_offset = get_mad_offset(payload, op == CH_OP_MAD_16B);
	if (mad_offset < 0)
		goto done;
	if (len < mad_offset + (int)sizeof(struct ib_mad_hdr))
		goto done;
	mad = (struct ib_mad *)(payload + mad_offset);

	port = sideband & 0x7;
	if (!port) {
		dd_dev_info(dd, "Fudging zero port to 1\n");
		port = 1;
	}
	if (port > 2) {
		dd_dev_info(dd, "Fudging hfi2_loopback port %d -> %d\n", port,
			    port - 2);
		port = port - 2;
	}
	if (!port || port > dd->num_pports)
		goto done;
	ppd = &dd->pport[port - 1];

	switch (mad->mad_hdr.base_version) {
	case OPA_MGMT_BASE_VERSION:
		sts = hfi2_opa_mad_cport(ppd, (struct opa_mad *)mad);
		break;
	case IB_MGMT_BASE_VERSION:
		sts = hfi2_ib_mad_cport(ppd, mad);
		break;
	default:
		break;
	}

done:
	/* print mad and error before mad_hdr.method is overwritten */
	if (mad && (sts || (mad->mad_hdr.status & ~IB_SMP_DIRECTION))) {
		print_mad(dd, port, mad, __func__, true);
		if (ppd)
			ppd_dev_info(
				ppd,
				"CPORT MAD error: [%02x] %02x %02x %02x %02x - %04x %04x %08x (%d)\n",
				sideband, mad->mad_hdr.base_version,
				mad->mad_hdr.mgmt_class,
				mad->mad_hdr.class_version, mad->mad_hdr.method,
				be16_to_cpu(mad->mad_hdr.status),
				be16_to_cpu(mad->mad_hdr.attr_id),
				be32_to_cpu(mad->mad_hdr.attr_mod), sts);
		else
			dd_dev_info(
				dd,
				"CPORT MAD error: [%02x] %02x %02x %02x %02x - %04x %04x %08x (%d)\n",
				sideband, mad->mad_hdr.base_version,
				mad->mad_hdr.mgmt_class,
				mad->mad_hdr.class_version, mad->mad_hdr.method,
				be16_to_cpu(mad->mad_hdr.status),
				be16_to_cpu(mad->mad_hdr.attr_id),
				be32_to_cpu(mad->mad_hdr.attr_mod), sts);
	} else if (sts) {
		if (ppd)
			ppd_dev_info(ppd, "CPORT MAD error: [%02x] (%d)\n",
				     sideband, sts);
		else
			dd_dev_info(dd, "CPORT MAD error: [%02x] (%d)\n",
				    sideband, sts);
	}
	/*
	 * Last step before returning MAD to cport: If this was a Get or Set,
	 * change the MgmtMethod value to a Get Response.
	 */
	if (mad && (mad->mad_hdr.method == IB_MGMT_METHOD_GET ||
		    mad->mad_hdr.method == IB_MGMT_METHOD_SET))
		mad->mad_hdr.method = IB_MGMT_METHOD_GET_RESP;
	/*
	 * Copy (MAD response) payload back into MCTXT response.
	 * This is done even if sts != 0.
	 */
	hfi2_cport_resp_set(handle, payload, len);
	return sts;
}

int hfi2_mad_init(struct hfi2_devdata *dd)
{
	int ret;

	if (!dd->cport)
		return 0;
	ret = hfi2_cport_register_cb(dd, CH_OP_MAD_9B, CH_OP_MAD_16B,
				     cport_mad_handler);
	if (ret)
		dd_dev_warn(dd, "Failed to register for MCTXT MADs (%d)\n",
			    ret);
	ret = hfi2_cport_register_cb(dd, CH_OP_UMAD_9B, CH_OP_UMAD_16B,
				     cport_umad_handler);
	if (ret)
		dd_dev_warn(dd, "Failed to register for MCTXT UMADs (%d)\n",
			    ret);
	return ret;
}

int hfi2_mad_deinit(struct hfi2_devdata *dd)
{
	if (!dd->cport)
		return 0;
	hfi2_deinit_cport_trap128(dd);
	hfi2_cport_register_cb(dd, CH_OP_MAD_9B, CH_OP_MAD_16B, NULL);
	hfi2_cport_register_cb(dd, CH_OP_UMAD_9B, CH_OP_UMAD_16B, NULL);
	return 0;
}
