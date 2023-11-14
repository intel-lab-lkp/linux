// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.

#include <linux/netdevice.h>
#include <linux/idr.h>
#include "en_accel/nvmeotcp.h"
#include "en_accel/fs_tcp.h"
#include "en/txrx.h"

#define MAX_NUM_NVMEOTCP_QUEUES	(4000)
#define MIN_NUM_NVMEOTCP_QUEUES	(1)

static const struct rhashtable_params rhash_queues = {
	.key_len = sizeof(int),
	.key_offset = offsetof(struct mlx5e_nvmeotcp_queue, id),
	.head_offset = offsetof(struct mlx5e_nvmeotcp_queue, hash),
	.automatic_shrinking = true,
	.min_size = MIN_NUM_NVMEOTCP_QUEUES,
	.max_size = MAX_NUM_NVMEOTCP_QUEUES,
};

static int
mlx5e_nvmeotcp_offload_limits(struct net_device *netdev,
			      struct ulp_ddp_limits *limits)
{
	return 0;
}

static int
mlx5e_nvmeotcp_queue_init(struct net_device *netdev,
			  struct sock *sk,
			  struct ulp_ddp_config *tconfig)
{
	return 0;
}

static void
mlx5e_nvmeotcp_queue_teardown(struct net_device *netdev,
			      struct sock *sk)
{
}

static int
mlx5e_nvmeotcp_ddp_setup(struct net_device *netdev,
			 struct sock *sk,
			 struct ulp_ddp_io *ddp)
{
	return 0;
}

static void
mlx5e_nvmeotcp_ddp_teardown(struct net_device *netdev,
			    struct sock *sk,
			    struct ulp_ddp_io *ddp,
			    void *ddp_ctx)
{
}

static void
mlx5e_nvmeotcp_ddp_resync(struct net_device *netdev,
			  struct sock *sk, u32 seq)
{
}

int set_ulp_ddp_nvme_tcp(struct net_device *netdev, bool enable)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_params new_params;
	int err = 0;

	/* There may be offloaded queues when an netlink callback to disable the feature is made.
	 * Hence, we can't destroy the tcp flow-table since it may be referenced by the offload
	 * related flows and we'll keep the 128B CQEs on the channel RQs. Also, since we don't
	 * deref/destroy the fs tcp table when the feature is disabled, we don't ref it again
	 * if the feature is enabled multiple times.
	 */
	if (!enable || priv->nvmeotcp->enabled)
		return 0;

	err = mlx5e_accel_fs_tcp_create(priv->fs);
	if (err)
		return err;

	new_params = priv->channels.params;
	new_params.nvmeotcp = enable;
	err = mlx5e_safe_switch_params(priv, &new_params, NULL, NULL, true);
	if (err)
		goto fs_tcp_destroy;

	priv->nvmeotcp->enabled = true;
	return 0;

fs_tcp_destroy:
	mlx5e_accel_fs_tcp_destroy(priv->fs);
	return err;
}

static int mlx5e_ulp_ddp_set_caps(struct net_device *netdev, unsigned long *new_caps,
				  struct netlink_ext_ack *extack)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	DECLARE_BITMAP(old_caps, ULP_DDP_CAP_COUNT);
	struct mlx5e_params *params;
	int ret = 0;
	int nvme = -1;

	mutex_lock(&priv->state_lock);
	params = &priv->channels.params;
	bitmap_copy(old_caps, priv->nvmeotcp->ddp_caps.active, ULP_DDP_CAP_COUNT);

	/* always handle nvme-tcp-ddp and nvme-tcp-ddgst-rx together (all or nothing) */

	if (ulp_ddp_cap_turned_on(old_caps, new_caps, ULP_DDP_CAP_NVME_TCP) &&
	    ulp_ddp_cap_turned_on(old_caps, new_caps, ULP_DDP_CAP_NVME_TCP_DDGST_RX)) {
		if (MLX5E_GET_PFLAG(params, MLX5E_PFLAG_RX_CQE_COMPRESS)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "NVMe-TCP offload not supported when CQE compress is active. Disable rx_cqe_compress ethtool private flag first\n");
			goto out;
		}

		if (netdev->features & (NETIF_F_LRO | NETIF_F_GRO_HW)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "NVMe-TCP offload not supported when HW_GRO/LRO is active. Disable rx-gro-hw ethtool feature first\n");
			goto out;
		}
		nvme = 1;
	} else if (ulp_ddp_cap_turned_off(old_caps, new_caps, ULP_DDP_CAP_NVME_TCP) &&
		   ulp_ddp_cap_turned_off(old_caps, new_caps, ULP_DDP_CAP_NVME_TCP_DDGST_RX)) {
		nvme = 0;
	}

	if (nvme >= 0) {
		ret = set_ulp_ddp_nvme_tcp(netdev, nvme);
		if (ret)
			goto out;
		change_bit(ULP_DDP_CAP_NVME_TCP, priv->nvmeotcp->ddp_caps.active);
		change_bit(ULP_DDP_CAP_NVME_TCP_DDGST_RX, priv->nvmeotcp->ddp_caps.active);
	}

out:
	mutex_unlock(&priv->state_lock);
	return ret;
}

static void mlx5e_ulp_ddp_get_caps(struct net_device *dev,
				   struct ulp_ddp_dev_caps *caps)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	mutex_lock(&priv->state_lock);
	memcpy(caps, &priv->nvmeotcp->ddp_caps, sizeof(*caps));
	mutex_unlock(&priv->state_lock);
}

const struct ulp_ddp_dev_ops mlx5e_nvmeotcp_ops = {
	.limits = mlx5e_nvmeotcp_offload_limits,
	.sk_add = mlx5e_nvmeotcp_queue_init,
	.sk_del = mlx5e_nvmeotcp_queue_teardown,
	.setup = mlx5e_nvmeotcp_ddp_setup,
	.teardown = mlx5e_nvmeotcp_ddp_teardown,
	.resync = mlx5e_nvmeotcp_ddp_resync,
	.set_caps = mlx5e_ulp_ddp_set_caps,
	.get_caps = mlx5e_ulp_ddp_get_caps,
};

void mlx5e_nvmeotcp_cleanup_rx(struct mlx5e_priv *priv)
{
	if (priv->nvmeotcp && priv->nvmeotcp->enabled)
		mlx5e_accel_fs_tcp_destroy(priv->fs);
}

int mlx5e_nvmeotcp_init(struct mlx5e_priv *priv)
{
	struct mlx5e_nvmeotcp *nvmeotcp = NULL;
	int ret = 0;

	if (!(MLX5_CAP_GEN(priv->mdev, nvmeotcp) &&
	      MLX5_CAP_DEV_NVMEOTCP(priv->mdev, zerocopy) &&
	      MLX5_CAP_DEV_NVMEOTCP(priv->mdev, crc_rx) &&
	      MLX5_CAP_GEN(priv->mdev, cqe_128_always)))
		return 0;

	nvmeotcp = kzalloc(sizeof(*nvmeotcp), GFP_KERNEL);

	if (!nvmeotcp)
		return -ENOMEM;

	ida_init(&nvmeotcp->queue_ids);
	ret = rhashtable_init(&nvmeotcp->queue_hash, &rhash_queues);
	if (ret)
		goto err_ida;

	/* report ULP DPP as supported, but don't enable it by default */
	set_bit(ULP_DDP_CAP_NVME_TCP, nvmeotcp->ddp_caps.hw);
	set_bit(ULP_DDP_CAP_NVME_TCP_DDGST_RX, nvmeotcp->ddp_caps.hw);
	nvmeotcp->enabled = false;
	priv->nvmeotcp = nvmeotcp;
	return 0;

err_ida:
	ida_destroy(&nvmeotcp->queue_ids);
	kfree(nvmeotcp);
	return ret;
}

void mlx5e_nvmeotcp_cleanup(struct mlx5e_priv *priv)
{
	struct mlx5e_nvmeotcp *nvmeotcp = priv->nvmeotcp;

	if (!nvmeotcp)
		return;

	rhashtable_destroy(&nvmeotcp->queue_hash);
	ida_destroy(&nvmeotcp->queue_ids);
	kfree(nvmeotcp);
	priv->nvmeotcp = NULL;
}
