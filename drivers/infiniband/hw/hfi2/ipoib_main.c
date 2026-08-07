// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Copyright(c) 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 */

/*
 * This file contains HFI2 support for ipoib functionality
 */

#include "ipoib.h"
#include "hfi2.h"

static u32 qpn_from_mac(const u8 *mac_arr)
{
	return (u32)mac_arr[1] << 16 | mac_arr[2] << 8 | mac_arr[3];
}

static int hfi2_ipoib_dev_init(struct net_device *dev)
{
	struct hfi2_ipoib_dev_priv *priv = hfi2_ipoib_priv(dev);
	int ret;

	ret = priv->netdev_ops->ndo_init(dev);
	if (ret)
		return ret;

	ret = hfi2_netdev_add_data(priv->ppd,
				   qpn_from_mac(priv->netdev->dev_addr), dev);
	if (ret < 0) {
		priv->netdev_ops->ndo_uninit(dev);
		return ret;
	}

	return 0;
}

static void hfi2_ipoib_dev_uninit(struct net_device *dev)
{
	struct hfi2_ipoib_dev_priv *priv = hfi2_ipoib_priv(dev);

	hfi2_netdev_remove_data(priv->ppd,
				qpn_from_mac(priv->netdev->dev_addr));

	priv->netdev_ops->ndo_uninit(dev);
}

static int hfi2_ipoib_dev_open(struct net_device *dev)
{
	struct hfi2_ipoib_dev_priv *priv = hfi2_ipoib_priv(dev);
	int ret;

	ret = priv->netdev_ops->ndo_open(dev);
	if (!ret) {
		struct hfi2_ibport *ibp =
			to_iport(priv->device, priv->port_num);
		struct rvt_qp *qp;
		u32 qpn = qpn_from_mac(priv->netdev->dev_addr);

		rcu_read_lock();
		qp = rvt_lookup_qpn(ib_to_rvt(priv->device), &ibp->rvp,
				    qpn | RVT_AIP_QP_BASE);
		if (!qp) {
			rcu_read_unlock();
			priv->netdev_ops->ndo_stop(dev);
			return -EINVAL;
		}
		rvt_get_qp(qp);
		priv->qp = qp;
		rcu_read_unlock();

		hfi2_netdev_enable_queues(priv->ppd);
		hfi2_ipoib_napi_tx_enable(dev);
	}

	return ret;
}

static int hfi2_ipoib_dev_stop(struct net_device *dev)
{
	struct hfi2_ipoib_dev_priv *priv = hfi2_ipoib_priv(dev);

	if (!priv->qp)
		return 0;

	hfi2_ipoib_napi_tx_disable(dev);
	hfi2_netdev_disable_queues(priv->ppd);

	rvt_put_qp(priv->qp);
	priv->qp = NULL;

	return priv->netdev_ops->ndo_stop(dev);
}

static const struct net_device_ops hfi2_ipoib_netdev_ops = {
	.ndo_init = hfi2_ipoib_dev_init,
	.ndo_uninit = hfi2_ipoib_dev_uninit,
	.ndo_open = hfi2_ipoib_dev_open,
	.ndo_stop = hfi2_ipoib_dev_stop,
};

static int hfi2_ipoib_mcast_attach(struct net_device *dev,
				   struct ib_device *device, union ib_gid *mgid,
				   u16 mlid, int set_qkey, u32 qkey)
{
	struct hfi2_ipoib_dev_priv *priv = hfi2_ipoib_priv(dev);
	u32 qpn = (u32)qpn_from_mac(priv->netdev->dev_addr);
	struct hfi2_ibport *ibp = to_iport(priv->device, priv->port_num);
	struct rvt_qp *qp;
	int ret = -EINVAL;

	rcu_read_lock();

	qp = rvt_lookup_qpn(ib_to_rvt(priv->device), &ibp->rvp,
			    qpn | RVT_AIP_QP_BASE);
	if (qp) {
		rvt_get_qp(qp);
		rcu_read_unlock();
		if (set_qkey)
			priv->qkey = qkey;

		/* attach QP to multicast group */
		ret = qp->ibqp.device->ops.attach_mcast(&qp->ibqp, mgid, mlid);
		rvt_put_qp(qp);
	} else {
		rcu_read_unlock();
	}

	return ret;
}

static int hfi2_ipoib_mcast_detach(struct net_device *dev,
				   struct ib_device *device, union ib_gid *mgid,
				   u16 mlid)
{
	struct hfi2_ipoib_dev_priv *priv = hfi2_ipoib_priv(dev);
	u32 qpn = (u32)qpn_from_mac(priv->netdev->dev_addr);
	struct hfi2_ibport *ibp = to_iport(priv->device, priv->port_num);
	struct rvt_qp *qp;
	int ret = -EINVAL;

	rcu_read_lock();

	qp = rvt_lookup_qpn(ib_to_rvt(priv->device), &ibp->rvp,
			    qpn | RVT_AIP_QP_BASE);
	if (qp) {
		rvt_get_qp(qp);
		rcu_read_unlock();
		ret = qp->ibqp.device->ops.detach_mcast(&qp->ibqp, mgid, mlid);
		rvt_put_qp(qp);
	} else {
		rcu_read_unlock();
	}
	return ret;
}

static void hfi2_ipoib_netdev_dtor(struct net_device *dev)
{
	struct hfi2_ipoib_dev_priv *priv = hfi2_ipoib_priv(dev);

	hfi2_ipoib_txreq_deinit(priv);
	hfi2_ipoib_rxq_deinit(priv->netdev);
}

static void hfi2_ipoib_set_id(struct net_device *dev, int id)
{
	struct hfi2_ipoib_dev_priv *priv = hfi2_ipoib_priv(dev);

	priv->pkey_index = (u16)id;
	ib_query_pkey(priv->device, priv->port_num, priv->pkey_index,
		      &priv->pkey);
}

static int hfi2_ipoib_setup_rn(struct ib_device *device, u32 port_num,
			       struct net_device *netdev, void *param)
{
	struct hfi2_devdata *dd = dd_from_ibdev(device);
	struct rdma_netdev *rn = netdev_priv(netdev);
	struct hfi2_ipoib_dev_priv *priv;
	int rc;

	rn->send = hfi2_ipoib_send;
	rn->tx_timeout = hfi2_ipoib_tx_timeout;
	rn->attach_mcast = hfi2_ipoib_mcast_attach;
	rn->detach_mcast = hfi2_ipoib_mcast_detach;
	rn->set_id = hfi2_ipoib_set_id;
	rn->hca = device;
	rn->port_num = port_num;
	rn->mtu = netdev->mtu;

	priv = hfi2_ipoib_priv(netdev);
	priv->dd = dd;
	priv->ppd = &dd->pport[port_num - 1];
	priv->netdev = netdev;
	priv->device = device;
	priv->port_num = port_num;
	priv->netdev_ops = netdev->netdev_ops;

	ib_query_pkey(device, port_num, priv->pkey_index, &priv->pkey);

	rc = hfi2_ipoib_txreq_init(priv);
	if (rc) {
		dd_dev_err(dd, "IPoIB netdev TX init - failed(%d)\n", rc);
		return rc;
	}

	rc = hfi2_ipoib_rxq_init(netdev);
	if (rc) {
		dd_dev_err(dd, "IPoIB netdev RX init - failed(%d)\n", rc);
		hfi2_ipoib_txreq_deinit(priv);
		return rc;
	}

	netdev->netdev_ops = &hfi2_ipoib_netdev_ops;

	netdev->priv_destructor = hfi2_ipoib_netdev_dtor;
	netdev->needs_free_netdev = true;
	netdev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;

	return 0;
}

int hfi2_ipoib_rn_get_params(struct ib_device *device, u32 port_num,
			     enum rdma_netdev_t type,
			     struct rdma_netdev_alloc_params *params)
{
	struct hfi2_devdata *dd = dd_from_ibdev(device);
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	struct hfi2_portrsrcs *pr;

	if (type != RDMA_NETDEV_IPOIB)
		return -EOPNOTSUPP;

	if (!port_num || port_num > dd->num_pports)
		return -EINVAL;
	pr = &dr->ppr[port_num - 1];

	if (!HFI2_CAP_IS_KSET(AIP) || !pr->num_netdev_contexts)
		return -EOPNOTSUPP;

	params->sizeof_priv = sizeof(struct hfi2_ipoib_rdma_netdev);
	params->txqs = dr->last_sdma_engine - dr->first_sdma_engine;
	params->rxqs = pr->num_netdev_contexts;
	params->param = NULL;
	params->initialize_rdma_netdev = hfi2_ipoib_setup_rn;

	return 0;
}
