// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2022 Hewlett Packard Enterprise, Inc. All rights reserved.
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

/*
 * rxe_mcast.c implements driver support for multicast transport.
 * It is based on two data structures struct rxe_mcg ('mcg') and
 * struct rxe_mca ('mca'). An mcg is allocated each time a qp is
 * attached to a new mgid for the first time. These are indexed by
 * a red-black tree using the mgid. This data structure is searched
 * for the mcg when a multicast packet is received and when another
 * qp is attached to the same mgid. It is cleaned up when the last qp
 * is detached from the mcg. Each time a qp is attached to an mcg an
 * mca is created. It holds a pointer to the qp and is added to a list
 * of qp's that are attached to the mcg. The qp_list is used to replicate
 * mcast packets in the rxe receive path.
 */

#include <linux/igmp.h>

#include "rxe.h"

static int rxe_mcast_add6(struct rxe_dev *rxe, union ib_gid *mgid)
{
	struct in6_addr *addr6 = (struct in6_addr *)mgid;
	unsigned char ll_addr[ETH_ALEN];
	int err;

	rtnl_lock();
	err = ipv6_sock_mc_join(recv_sockets.sk6->sk, rxe->ndev->ifindex,
				addr6);
	rtnl_unlock();
	if (err && err != -EADDRINUSE)
		goto err_out;

	ipv6_eth_mc_map((struct in6_addr *)mgid->raw, ll_addr);
	err = dev_mc_add(rxe->ndev, ll_addr);
	if (err)
		goto err_drop;

	return 0;

err_drop:
	rtnl_lock();
	ipv6_sock_mc_drop(recv_sockets.sk6->sk, rxe->ndev->ifindex, addr6);
	rtnl_unlock();
err_out:
	return err;
}

static int rxe_mcast_add(struct rxe_mcg *mcg)
{
	struct rxe_dev *rxe = mcg->rxe;
	union ib_gid *mgid = &mcg->mgid;
	unsigned char ll_addr[ETH_ALEN];
	struct ip_mreqn imr = {};
	int err;

	if (mcg->is_ipv6)
		return rxe_mcast_add6(rxe, mgid);

	imr.imr_multiaddr = *(struct in_addr *)(mgid->raw + 12);
	imr.imr_ifindex = rxe->ndev->ifindex;
	rtnl_lock();
	err = ip_mc_join_group(recv_sockets.sk4->sk, &imr);
	rtnl_unlock();
	if (err && err != -EADDRINUSE)
		goto err_out;

	ip_eth_mc_map(imr.imr_multiaddr.s_addr, ll_addr);
	err = dev_mc_add(rxe->ndev, ll_addr);
	if (err)
		goto err_leave;

	return 0;

err_leave:
	rtnl_lock();
	ip_mc_leave_group(recv_sockets.sk4->sk, &imr);
	rtnl_unlock();
err_out:
	return err;
}

static int rxe_mcast_del6(struct rxe_dev *rxe, union ib_gid *mgid)
{
	unsigned char ll_addr[ETH_ALEN];
	int err, err2;

	ipv6_eth_mc_map((struct in6_addr *)mgid->raw, ll_addr);
	err = dev_mc_del(rxe->ndev, ll_addr);

	rtnl_lock();
	err2 = ipv6_sock_mc_drop(recv_sockets.sk6->sk,
			rxe->ndev->ifindex, (struct in6_addr *)mgid);
	rtnl_unlock();

	return err ?: err2;
}

static int rxe_mcast_del(struct rxe_mcg *mcg)
{
	struct rxe_dev *rxe = mcg->rxe;
	union ib_gid *mgid = &mcg->mgid;
	unsigned char ll_addr[ETH_ALEN];
	struct ip_mreqn imr = {};
	int err, err2;

	if (mcg->is_ipv6)
		return rxe_mcast_del6(rxe, mgid);

	imr.imr_multiaddr = *(struct in_addr *)(mgid->raw + 12);
	imr.imr_ifindex = rxe->ndev->ifindex;
	ip_eth_mc_map(imr.imr_multiaddr.s_addr, ll_addr);
	err = dev_mc_del(rxe->ndev, ll_addr);

	rtnl_lock();
	err2 = ip_mc_leave_group(recv_sockets.sk4->sk, &imr);
	rtnl_unlock();

	return err ?: err2;
}

static void __rxe_remove_mcg(struct rxe_mcg *mcg)
{
	rb_erase(&mcg->node, &mcg->rxe->mcg_tree);
}

static void rxe_cleanup_mcg(struct kref *kref)
{
	struct rxe_mcg *mcg = container_of(kref, typeof(*mcg), ref_cnt);

	__rxe_remove_mcg(mcg);
	rxe_mcast_del(mcg);
	atomic_dec(&mcg->rxe->mcg_num);
	kfree_rcu(mcg, rcu);
}

static int rxe_get_mcg(struct rxe_mcg *mcg)
{
	return kref_get_unless_zero(&mcg->ref_cnt);
}

int rxe_put_mcg(struct rxe_mcg *mcg)
{
	return kref_put(&mcg->ref_cnt, rxe_cleanup_mcg);
}

static void __rxe_insert_mcg(struct rxe_mcg *mcg)
{
	struct rb_root *tree = &mcg->rxe->mcg_tree;
	struct rb_node **link = &tree->rb_node;
	struct rb_node *node = NULL;
	struct rxe_mcg *tmp;
	int cmp;

	while (*link) {
		node = *link;
		tmp = rb_entry(node, struct rxe_mcg, node);

		cmp = memcmp(&tmp->mgid, &mcg->mgid, sizeof(mcg->mgid));
		if (cmp > 0) {
			link = &(*link)->rb_left;
		} else if (cmp < 0) {
			link = &(*link)->rb_right;
		} else {
			/* we must delete the old mcg before adding one */
			WARN_ON_ONCE(1);
			return;
		}
	}

	rb_link_node_rcu(&mcg->node, node, link);
	rb_insert_color(&mcg->node, tree);
}

/*
 * Lookup mgid in the multicast group red-black tree and try to
 * get a ref on it. Return mcg on success else NULL.
 */
struct rxe_mcg *rxe_lookup_mcg(struct rxe_dev *rxe, union ib_gid *mgid)
{
	struct rb_root *tree = &rxe->mcg_tree;
	struct rxe_mcg *mcg;
	struct rb_node *node;
	int cmp;

	rcu_read_lock();
	node = rcu_dereference_raw(tree->rb_node);

	while (node) {
		mcg = rb_entry(node, struct rxe_mcg, node);

		cmp = memcmp(&mcg->mgid, mgid, sizeof(*mgid));

		if (cmp > 0)
			node = rcu_dereference_raw(node->rb_left);
		else if (cmp < 0)
			node = rcu_dereference_raw(node->rb_right);
		else
			break;
	}
	mcg = (node && rxe_get_mcg(mcg)) ? mcg : NULL;
	rcu_read_unlock();

	return mcg;
}

/* find an existing mcg or allocate a new one */
static struct rxe_mcg *rxe_alloc_mcg(struct rxe_dev *rxe, union ib_gid *mgid)
{
	struct rxe_mcg *mcg;
	int err;

	mutex_lock(&rxe->mcg_mutex);
	mcg = rxe_lookup_mcg(rxe, mgid);
	if (mcg)
		goto out;	/* nothing to do */

	if (atomic_inc_return(&rxe->mcg_num) > rxe->attr.max_mcast_grp) {
		err = -EINVAL;
		goto err_dec;
	}

	mcg = kzalloc(sizeof(*mcg), GFP_KERNEL);
	if (!mcg) {
		err = -ENOMEM;
		goto err_dec;
	}

	memcpy(&mcg->mgid, mgid, sizeof(mcg->mgid));
	mcg->is_ipv6 = !ipv6_addr_v4mapped((struct in6_addr *)mgid);
	mcg->rxe = rxe;
	kref_init(&mcg->ref_cnt);
	INIT_LIST_HEAD(&mcg->qp_list);
	spin_lock_init(&mcg->lock);

	err = rxe_mcast_add(mcg);
	if (err)
		goto err_free;

	__rxe_insert_mcg(mcg);
out:
	mutex_unlock(&rxe->mcg_mutex);
	return mcg;

err_free:
	kfree(mcg);
err_dec:
	atomic_dec(&rxe->mcg_num);
	mutex_unlock(&rxe->mcg_mutex);
	return ERR_PTR(err);
}

static int rxe_attach_mcg(struct rxe_qp *qp, struct rxe_mcg *mcg)
{
	struct rxe_dev *rxe = mcg->rxe;
	struct rxe_mca *mca;
	unsigned long flags;
	int err = 0;

	mutex_lock(&rxe->mcg_mutex);
	spin_lock_irqsave(&mcg->lock, flags);
	list_for_each_entry(mca, &mcg->qp_list, qp_list) {
		if (mca->qp == qp) {
			spin_unlock_irqrestore(&mcg->lock, flags);
			goto out;	/* nothing to do */
		}
	}
	spin_unlock_irqrestore(&mcg->lock, flags);

	if (atomic_inc_return(&rxe->mcg_attach) >
	    rxe->attr.max_total_mcast_qp_attach) {
		err = -EINVAL;
		goto err_dec_attach;
	}

	if (atomic_inc_return(&mcg->qp_num) >
	    rxe->attr.max_mcast_qp_attach) {
		err = -EINVAL;
		goto err_dec_qp_num;
	}

	mca = kzalloc(sizeof(*mca), GFP_KERNEL);
	if (!mca) {
		err = -ENOMEM;
		goto err_dec_qp_num;
	}

	atomic_inc(&qp->mcg_num);
	rxe_get(qp);
	mca->qp = qp;

	rxe_get_mcg(mcg);

	spin_lock_irqsave(&mcg->lock, flags);
	list_add_tail(&mca->qp_list, &mcg->qp_list);
	spin_unlock_irqrestore(&mcg->lock, flags);
	goto out;

err_dec_qp_num:
	atomic_dec(&mcg->qp_num);
err_dec_attach:
	atomic_dec(&rxe->mcg_attach);
out:
	rxe_put_mcg(mcg);
	mutex_unlock(&rxe->mcg_mutex);
	return err;
}

static int rxe_detach_mcg(struct rxe_qp *qp, struct rxe_mcg *mcg)
{
	struct rxe_dev *rxe = mcg->rxe;
	struct rxe_mca *mca;
	unsigned long flags;
	int err = 0;

	mutex_lock(&rxe->mcg_mutex);
	spin_lock_irqsave(&mcg->lock, flags);
	list_for_each_entry(mca, &mcg->qp_list, qp_list) {
		if (mca->qp == qp) {
			spin_unlock_irqrestore(&mcg->lock, flags);
			goto found;
		}
	}
	spin_unlock_irqrestore(&mcg->lock, flags);

	err = -EINVAL;
	goto err_out;

found:
	spin_lock_irqsave(&mcg->lock, flags);
	list_del(&mca->qp_list);
	spin_unlock_irqrestore(&mcg->lock, flags);
	rxe_put_mcg(mcg);

	atomic_dec(&mcg->qp_num);
	atomic_dec(&mcg->rxe->mcg_attach);
	atomic_dec(&mca->qp->mcg_num);
	rxe_put(mca->qp);
	kfree(mca);
err_out:
	rxe_put_mcg(mcg);
	mutex_unlock(&rxe->mcg_mutex);
	return err;
}

/**
 * rxe_attach_mcast - attach qp to multicast group (see IBA-11.3.1)
 * @ibqp: (IB) qp object
 * @mgid: multicast IP address
 * @mlid: multicast LID, ignored for RoCEv2 (see IBA-A17.5.6)
 *
 * Returns: 0 on success else an errno
 */
int rxe_attach_mcast(struct ib_qp *ibqp, union ib_gid *mgid, u16 mlid)
{
	struct rxe_dev *rxe = to_rdev(ibqp->device);
	struct rxe_qp *qp = to_rqp(ibqp);
	struct rxe_mcg *mcg;

	if (rxe->attr.max_mcast_grp == 0)
		return -EINVAL;

	mcg = rxe_alloc_mcg(rxe, mgid);
	if (IS_ERR(mcg))
		return PTR_ERR(mcg);

	return rxe_attach_mcg(qp, mcg);
}

/**
 * rxe_detach_mcast - detach qp from multicast group (see IBA-11.3.2)
 * @ibqp: address of (IB) qp object
 * @mgid: multicast IP address
 * @mlid: multicast LID, ignored for RoCEv2 (see IBA-A17.5.6)
 *
 * Returns: 0 on success else an errno
 */
int rxe_detach_mcast(struct ib_qp *ibqp, union ib_gid *mgid, u16 mlid)
{
	struct rxe_dev *rxe = to_rdev(ibqp->device);
	struct rxe_qp *qp = to_rqp(ibqp);
	struct rxe_mcg *mcg;

	mcg = rxe_lookup_mcg(rxe, mgid);
	if (!mcg)
		return -EINVAL;

	return rxe_detach_mcg(qp, mcg);
}
