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

/**
 * __rxe_insert_mcg - insert an mcg into red-black tree (rxe->mcg_tree)
 * @mcg: mcg object with an embedded red-black tree node
 *
 * Context: caller must hold a reference to mcg and rxe->mcg_mutex and
 * is responsible to avoid adding the same mcg twice to the tree.
 */
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

/**
 * __rxe_remove_mcg - remove an mcg from red-black tree holding lock
 * @mcg: mcast group object with an embedded red-black tree node
 *
 * Context: caller must hold a reference to mcg and rxe->mcg_mutex
 */
static void __rxe_remove_mcg(struct rxe_mcg *mcg)
{
	rb_erase(&mcg->node, &mcg->rxe->mcg_tree);
}

/*
 * Lookup mgid in the multicast group red-black tree and try to
 * get a ref on it. Return mcg on success else NULL.
 */
struct rxe_mcg *rxe_lookup_mcg(struct rxe_dev *rxe,
					union ib_gid *mgid)
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
	mcg = (node && kref_get_unless_zero(&mcg->ref_cnt)) ? mcg : NULL;
	rcu_read_unlock();

	return mcg;
}

/**
 * rxe_get_mcg - lookup or allocate a mcg
 * @rxe: rxe device object
 * @mgid: multicast IP address as a gid
 *
 * Returns: mcg on success else ERR_PTR(error)
 */
static struct rxe_mcg *rxe_get_mcg(struct rxe_dev *rxe, union ib_gid *mgid)
{
	struct rxe_mcg *mcg;
	int err;

	mutex_lock(&rxe->mcg_mutex);
	mcg = rxe_lookup_mcg(rxe, mgid);
	if (mcg)
		goto out;	/* nothing to do */

	if (atomic_inc_return(&rxe->mcg_num) > rxe->attr.max_mcast_grp) {
		err = -ENOMEM;
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
	kref_get(&mcg->ref_cnt);
	__rxe_insert_mcg(mcg);

	err = rxe_mcast_add(mcg);
	if (err)
		goto err_free;

out:
	mutex_unlock(&rxe->mcg_mutex);
	return mcg;

err_free:
	__rxe_remove_mcg(mcg);
	kfree(mcg);
err_dec:
	atomic_dec(&rxe->mcg_num);
	mutex_unlock(&rxe->mcg_mutex);
	return ERR_PTR(err);
}

/**
 * rxe_cleanup_mcg - cleanup mcg for kref_put
 * @kref: struct kref embnedded in mcg
 */
void rxe_cleanup_mcg(struct kref *kref)
{
	struct rxe_mcg *mcg = container_of(kref, typeof(*mcg), ref_cnt);

	kfree_rcu(mcg, rcu);
}

/**
 * __rxe_destroy_mcg - destroy mcg object holding rxe->mcg_mutex
 * @mcg: the mcg object
 *
 * Context: caller is holding rxe->mcg_mutex
 * no qp's are attached to mcg
 */
static void __rxe_destroy_mcg(struct rxe_mcg *mcg)
{
	struct rxe_dev *rxe = mcg->rxe;

	/* remove mcg from red-black tree then drop ref */
	__rxe_remove_mcg(mcg);
	kref_put(&mcg->ref_cnt, rxe_cleanup_mcg);

	atomic_dec(&rxe->mcg_num);
}

/**
 * rxe_destroy_mcg - destroy mcg object
 * @mcg: the mcg object
 *
 * Context: no qp's are attached to mcg
 */
static void rxe_destroy_mcg(struct rxe_mcg *mcg)
{
	/* delete mcast address outside of lock */
	rxe_mcast_del(mcg);

	mutex_lock(&mcg->rxe->mcg_mutex);
	__rxe_destroy_mcg(mcg);
	mutex_unlock(&mcg->rxe->mcg_mutex);
}

/**
 * rxe_attach_mcg - attach qp to mcg if not already attached
 * @qp: qp object
 * @mcg: mcg object
 *
 * Returns: 0 on success else an error
 */
static int rxe_attach_mcg(struct rxe_mcg *mcg, struct rxe_qp *qp)
{
	struct rxe_dev *rxe = mcg->rxe;
	struct rxe_mca *mca;
	unsigned long flags;
	int err;

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

	spin_lock_irqsave(&mcg->lock, flags);
	list_add_tail(&mca->qp_list, &mcg->qp_list);
	spin_unlock_irqrestore(&mcg->lock, flags);
out:
	mutex_unlock(&rxe->mcg_mutex);
	return 0;

err_dec_qp_num:
	atomic_dec(&mcg->qp_num);
err_dec_attach:
	atomic_dec(&rxe->mcg_attach);
	mutex_unlock(&rxe->mcg_mutex);
	return err;
}

/**
 * rxe_detach_mcg - detach qp from mcg
 * @mcg: mcg object
 * @qp: qp object
 *
 * Returns: 0 on success else an error if qp is not attached.
 */
static int rxe_detach_mcg(struct rxe_mcg *mcg, struct rxe_qp *qp)
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

	/* we didn't find the qp on the list */
	err = -EINVAL;
	goto err_out;

found:
	spin_lock_irqsave(&mcg->lock, flags);
	list_del(&mca->qp_list);
	spin_unlock_irqrestore(&mcg->lock, flags);

	atomic_dec(&mcg->qp_num);
	atomic_dec(&mcg->rxe->mcg_attach);
	atomic_dec(&mca->qp->mcg_num);
	rxe_put(mca->qp);
	kfree(mca);

	/* if the number of qp's attached to the
	 * mcast group falls to zero go ahead and
	 * tear it down. This will not free the
	 * object since we are still holding a ref
	 * from the caller
	 */
	if (atomic_read(&mcg->qp_num) <= 0)
		__rxe_destroy_mcg(mcg);

err_out:
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
	int err;
	struct rxe_dev *rxe = to_rdev(ibqp->device);
	struct rxe_qp *qp = to_rqp(ibqp);
	struct rxe_mcg *mcg;

	if (rxe->attr.max_mcast_grp == 0)
		return -EINVAL;

	/* takes a ref on mcg if successful */
	mcg = rxe_get_mcg(rxe, mgid);
	if (IS_ERR(mcg))
		return PTR_ERR(mcg);

	err = rxe_attach_mcg(mcg, qp);

	/* if we failed to attach the first qp to mcg tear it down */
	if (atomic_read(&mcg->qp_num) == 0)
		rxe_destroy_mcg(mcg);

	kref_put(&mcg->ref_cnt, rxe_cleanup_mcg);

	return err;
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
	int err;

	mcg = rxe_lookup_mcg(rxe, mgid);
	if (!mcg)
		return -EINVAL;

	err = rxe_detach_mcg(mcg, qp);
	kref_put(&mcg->ref_cnt, rxe_cleanup_mcg);

	return err;
}
