// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.

#include <linux/netdevice.h>
#include <linux/idr.h>
#include "en_accel/nvmeotcp.h"
#include "en_accel/nvmeotcp_utils.h"
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

static void
fill_nvmeotcp_klm_wqe(struct mlx5e_nvmeotcp_queue *queue, struct mlx5e_umr_wqe *wqe, u16 ccid,
		      u32 klm_entries, u16 klm_offset)
{
	struct scatterlist *sgl_mkey;
	u32 lkey, i;

	lkey = queue->priv->mdev->mlx5e_res.hw_objs.mkey;
	for (i = 0; i < klm_entries; i++) {
		sgl_mkey = &queue->ccid_table[ccid].sgl[i + klm_offset];
		wqe->inline_klms[i].bcount = cpu_to_be32(sg_dma_len(sgl_mkey));
		wqe->inline_klms[i].key = cpu_to_be32(lkey);
		wqe->inline_klms[i].va = cpu_to_be64(sgl_mkey->dma_address);
	}

	for (; i < ALIGN(klm_entries, MLX5_UMR_KLM_NUM_ENTRIES_ALIGNMENT); i++) {
		wqe->inline_klms[i].bcount = 0;
		wqe->inline_klms[i].key = 0;
		wqe->inline_klms[i].va = 0;
	}
}

static void
build_nvmeotcp_klm_umr(struct mlx5e_nvmeotcp_queue *queue, struct mlx5e_umr_wqe *wqe,
		       u16 ccid, int klm_entries, u32 klm_offset, u32 len,
		       enum wqe_type klm_type)
{
	u32 id = (klm_type == KLM_UMR) ? queue->ccid_table[ccid].klm_mkey :
		 (mlx5e_tir_get_tirn(&queue->tir) << MLX5_WQE_CTRL_TIR_TIS_INDEX_SHIFT);
	u8 opc_mod = (klm_type == KLM_UMR) ? MLX5_CTRL_SEGMENT_OPC_MOD_UMR_UMR :
		MLX5_OPC_MOD_TRANSPORT_TIR_STATIC_PARAMS;
	u32 ds_cnt = MLX5E_KLM_UMR_DS_CNT(ALIGN(klm_entries, MLX5_UMR_KLM_NUM_ENTRIES_ALIGNMENT));
	struct mlx5_wqe_umr_ctrl_seg *ucseg = &wqe->uctrl;
	struct mlx5_wqe_ctrl_seg *cseg = &wqe->ctrl;
	struct mlx5_mkey_seg *mkc = &wqe->mkc;
	u32 sqn = queue->sq.sqn;
	u16 pc = queue->sq.pc;

	cseg->opmod_idx_opcode = cpu_to_be32((pc << MLX5_WQE_CTRL_WQE_INDEX_SHIFT) |
					     MLX5_OPCODE_UMR | (opc_mod) << 24);
	cseg->qpn_ds = cpu_to_be32((sqn << MLX5_WQE_CTRL_QPN_SHIFT) | ds_cnt);
	cseg->general_id = cpu_to_be32(id);

	if (klm_type == KLM_UMR && !klm_offset) {
		ucseg->mkey_mask = cpu_to_be64(MLX5_MKEY_MASK_XLT_OCT_SIZE |
					       MLX5_MKEY_MASK_LEN | MLX5_MKEY_MASK_FREE);
		mkc->xlt_oct_size = cpu_to_be32(ALIGN(len, MLX5_UMR_KLM_NUM_ENTRIES_ALIGNMENT));
		mkc->len = cpu_to_be64(queue->ccid_table[ccid].size);
	}

	ucseg->flags = MLX5_UMR_INLINE | MLX5_UMR_TRANSLATION_OFFSET_EN;
	ucseg->xlt_octowords = cpu_to_be16(ALIGN(klm_entries, MLX5_UMR_KLM_NUM_ENTRIES_ALIGNMENT));
	ucseg->xlt_offset = cpu_to_be16(klm_offset);
	fill_nvmeotcp_klm_wqe(queue, wqe, ccid, klm_entries, klm_offset);
}

static void
mlx5e_nvmeotcp_fill_wi(struct mlx5e_icosq *sq, u32 wqebbs, u16 pi)
{
	struct mlx5e_icosq_wqe_info *wi = &sq->db.wqe_info[pi];

	memset(wi, 0, sizeof(*wi));

	wi->num_wqebbs = wqebbs;
	wi->wqe_type = MLX5E_ICOSQ_WQE_UMR_NVMEOTCP;
}

static u32
post_klm_wqe(struct mlx5e_nvmeotcp_queue *queue,
	     enum wqe_type wqe_type,
	     u16 ccid,
	     u32 klm_length,
	     u32 klm_offset)
{
	struct mlx5e_icosq *sq = &queue->sq;
	u32 wqebbs, cur_klm_entries;
	struct mlx5e_umr_wqe *wqe;
	u16 pi, wqe_sz;

	cur_klm_entries = min_t(int, queue->max_klms_per_wqe, klm_length - klm_offset);
	wqe_sz = MLX5E_KLM_UMR_WQE_SZ(ALIGN(cur_klm_entries, MLX5_UMR_KLM_NUM_ENTRIES_ALIGNMENT));
	wqebbs = DIV_ROUND_UP(wqe_sz, MLX5_SEND_WQE_BB);
	pi = mlx5e_icosq_get_next_pi(sq, wqebbs);
	wqe = MLX5E_NVMEOTCP_FETCH_KLM_WQE(sq, pi);
	mlx5e_nvmeotcp_fill_wi(sq, wqebbs, pi);
	build_nvmeotcp_klm_umr(queue, wqe, ccid, cur_klm_entries, klm_offset,
			       klm_length, wqe_type);
	sq->pc += wqebbs;
	sq->doorbell_cseg = &wqe->ctrl;
	return cur_klm_entries;
}

static void
mlx5e_nvmeotcp_post_klm_wqe(struct mlx5e_nvmeotcp_queue *queue, enum wqe_type wqe_type,
			    u16 ccid, u32 klm_length)
{
	struct mlx5e_icosq *sq = &queue->sq;
	u32 klm_offset = 0, wqes, i;

	wqes = DIV_ROUND_UP(klm_length, queue->max_klms_per_wqe);

	spin_lock_bh(&queue->sq_lock);

	for (i = 0; i < wqes; i++)
		klm_offset += post_klm_wqe(queue, wqe_type, ccid, klm_length, klm_offset);

	if (wqe_type == KLM_UMR) /* not asking for completion on ddp_setup UMRs */
		__mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, sq->doorbell_cseg, 0);
	else
		mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, sq->doorbell_cseg);

	spin_unlock_bh(&queue->sq_lock);
}

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
	struct mlx5e_nvmeotcp_queue *queue;

	queue = container_of(ulp_ddp_get_ctx(sk),
			     struct mlx5e_nvmeotcp_queue, ulp_ddp_ctx);

	/* Placeholder - map_sg and initializing the count */

	mlx5e_nvmeotcp_post_klm_wqe(queue, KLM_UMR, ddp->command_id, 0);
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
