// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.

#include <linux/netdevice.h>
#include <linux/idr.h>
#include <linux/nvme-tcp.h>
#include "en_accel/nvmeotcp.h"
#include "en_accel/nvmeotcp_utils.h"
#include "en_accel/fs_tcp.h"
#include "en/txrx.h"

#define MAX_NUM_NVMEOTCP_QUEUES	(4000)
#define MIN_NUM_NVMEOTCP_QUEUES	(1)

/* Max PDU data will be 512K */
#define MLX5E_NVMEOTCP_MAX_SEGMENTS (128)
#define MLX5E_NVMEOTCP_IO_THRESHOLD (32 * 1024)
#define MLX5E_NVMEOTCP_FULL_CCID_RANGE (0)

static const struct rhashtable_params rhash_queues = {
	.key_len = sizeof(int),
	.key_offset = offsetof(struct mlx5e_nvmeotcp_queue, id),
	.head_offset = offsetof(struct mlx5e_nvmeotcp_queue, hash),
	.automatic_shrinking = true,
	.min_size = MIN_NUM_NVMEOTCP_QUEUES,
	.max_size = MAX_NUM_NVMEOTCP_QUEUES,
};

static u32 mlx5e_get_max_sgl(struct mlx5_core_dev *mdev)
{
	return min_t(u32,
		     MLX5E_NVMEOTCP_MAX_SEGMENTS,
		     1 << MLX5_CAP_GEN(mdev, log_max_klm_list_size));
}

static u32
mlx5e_get_channel_ix_from_io_cpu(struct mlx5e_params *params, u32 io_cpu)
{
	int num_channels = params->num_channels;
	u32 channel_ix = io_cpu;

	if (channel_ix >= num_channels)
		channel_ix = channel_ix % num_channels;

	return channel_ix;
}

static
int mlx5e_create_nvmeotcp_tag_buf_table(struct mlx5_core_dev *mdev,
					struct mlx5e_nvmeotcp_queue *queue,
					u8 log_table_size)
{
	u32 in[MLX5_ST_SZ_DW(create_nvmeotcp_tag_buf_table_in)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];
	u64 general_obj_types;
	void *obj;
	int err;

	obj = MLX5_ADDR_OF(create_nvmeotcp_tag_buf_table_in, in,
			   nvmeotcp_tag_buf_table_obj);

	general_obj_types = MLX5_CAP_GEN_64(mdev, general_obj_types);
	if (!(general_obj_types &
	      MLX5_HCA_CAP_GENERAL_OBJECT_TYPES_NVMEOTCP_TAG_BUFFER_TABLE))
		return -EINVAL;

	MLX5_SET(general_obj_in_cmd_hdr, in, opcode,
		 MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type,
		 MLX5_GENERAL_OBJECT_TYPES_NVMEOTCP_TAG_BUFFER_TABLE);
	MLX5_SET(nvmeotcp_tag_buf_table_obj, obj,
		 log_tag_buffer_table_size, log_table_size);

	err = mlx5_cmd_exec(mdev, in, sizeof(in), out, sizeof(out));
	if (!err)
		queue->tag_buf_table_id = MLX5_GET(general_obj_out_cmd_hdr,
						   out, obj_id);
	return err;
}

static
void mlx5_destroy_nvmeotcp_tag_buf_table(struct mlx5_core_dev *mdev, u32 uid)
{
	u32 in[MLX5_ST_SZ_DW(general_obj_in_cmd_hdr)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];

	MLX5_SET(general_obj_in_cmd_hdr, in, opcode,
		 MLX5_CMD_OP_DESTROY_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type,
		 MLX5_GENERAL_OBJECT_TYPES_NVMEOTCP_TAG_BUFFER_TABLE);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_id, uid);

	mlx5_cmd_exec(mdev, in, sizeof(in), out, sizeof(out));
}

static void
fill_nvmeotcp_bsf_klm_wqe(struct mlx5e_nvmeotcp_queue *queue, struct mlx5e_umr_wqe *wqe,
			  u16 ccid, u32 klm_entries, u16 klm_offset)
{
	u32 i;

	/* BSF_KLM_UMR is used to update the tag_buffer. To spare the
	 * need to update both mkc.length and tag_buffer[i].len in two
	 * different UMRs we initialize the tag_buffer[*].len to the
	 * maximum size of an entry so the HW check will pass and the
	 * validity of the MKEY len will be checked against the
	 * updated mkey context field.
	 */
	for (i = 0; i < klm_entries; i++) {
		u32 lkey = queue->ccid_table[i + klm_offset].klm_mkey;

		wqe->inline_klms[i].bcount = cpu_to_be32(U32_MAX);
		wqe->inline_klms[i].key = cpu_to_be32(lkey);
		wqe->inline_klms[i].va = 0;
	}
}

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
	u32 id = (klm_type == BSF_KLM_UMR) ?
		 (mlx5e_tir_get_tirn(&queue->tir) << MLX5_WQE_CTRL_TIR_TIS_INDEX_SHIFT) :
		 queue->ccid_table[ccid].klm_mkey;
	u8 opc_mod = (klm_type == BSF_KLM_UMR) ? MLX5_OPC_MOD_TRANSPORT_TIR_STATIC_PARAMS :
		     MLX5_CTRL_SEGMENT_OPC_MOD_UMR_UMR;
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

	if (!klm_entries) { /* this is invalidate */
		ucseg->mkey_mask = cpu_to_be64(MLX5_MKEY_MASK_FREE);
		ucseg->flags = MLX5_UMR_INLINE;
		mkc->status = MLX5_MKEY_STATUS_FREE;
		return;
	}

	if (klm_type == KLM_UMR && !klm_offset) {
		ucseg->mkey_mask = cpu_to_be64(MLX5_MKEY_MASK_XLT_OCT_SIZE |
					       MLX5_MKEY_MASK_LEN | MLX5_MKEY_MASK_FREE);
		mkc->xlt_oct_size = cpu_to_be32(ALIGN(len, MLX5_UMR_KLM_NUM_ENTRIES_ALIGNMENT));
		mkc->len = cpu_to_be64(queue->ccid_table[ccid].size);
	}

	ucseg->flags = MLX5_UMR_INLINE | MLX5_UMR_TRANSLATION_OFFSET_EN;
	ucseg->xlt_octowords = cpu_to_be16(ALIGN(klm_entries, MLX5_UMR_KLM_NUM_ENTRIES_ALIGNMENT));
	ucseg->xlt_offset = cpu_to_be16(klm_offset);
	if (klm_type == BSF_KLM_UMR)
		fill_nvmeotcp_bsf_klm_wqe(queue, wqe, ccid, klm_entries, klm_offset);
	else
		fill_nvmeotcp_klm_wqe(queue, wqe, ccid, klm_entries, klm_offset);
}

static void
fill_nvmeotcp_progress_params(struct mlx5e_nvmeotcp_queue *queue,
			      struct mlx5_seg_nvmeotcp_progress_params *params,
			      u32 seq)
{
	void *ctx = params->ctx;

	params->tir_num = cpu_to_be32(mlx5e_tir_get_tirn(&queue->tir));

	MLX5_SET(nvmeotcp_progress_params, ctx, next_pdu_tcp_sn, seq);
	MLX5_SET(nvmeotcp_progress_params, ctx, pdu_tracker_state,
		 MLX5E_NVMEOTCP_PROGRESS_PARAMS_PDU_TRACKER_STATE_START);
}

void
build_nvmeotcp_progress_params(struct mlx5e_nvmeotcp_queue *queue,
			       struct mlx5e_set_nvmeotcp_progress_params_wqe *wqe,
			       u32 seq)
{
	struct mlx5_wqe_ctrl_seg *cseg = &wqe->ctrl;
	u32 sqn = queue->sq.sqn;
	u16 pc = queue->sq.pc;
	u8 opc_mod;

	memset(wqe, 0, MLX5E_NVMEOTCP_PROGRESS_PARAMS_WQE_SZ);
	opc_mod = MLX5_CTRL_SEGMENT_OPC_MOD_UMR_NVMEOTCP_TIR_PROGRESS_PARAMS;
	cseg->opmod_idx_opcode = cpu_to_be32((pc << MLX5_WQE_CTRL_WQE_INDEX_SHIFT) |
					     MLX5_OPCODE_SET_PSV | (opc_mod << 24));
	cseg->qpn_ds = cpu_to_be32((sqn << MLX5_WQE_CTRL_QPN_SHIFT) |
				   PROGRESS_PARAMS_DS_CNT);
	fill_nvmeotcp_progress_params(queue, &wqe->params, seq);
}

static void
fill_nvmeotcp_static_params(struct mlx5e_nvmeotcp_queue *queue,
			    struct mlx5_wqe_transport_static_params_seg *params,
			    u32 resync_seq, bool ddgst_offload_en)
{
	void *ctx = params->ctx;

	MLX5_SET(transport_static_params, ctx, const_1, 1);
	MLX5_SET(transport_static_params, ctx, const_2, 2);
	MLX5_SET(transport_static_params, ctx, acc_type,
		 MLX5_TRANSPORT_STATIC_PARAMS_ACC_TYPE_NVMETCP);
	MLX5_SET(transport_static_params, ctx, nvme_resync_tcp_sn, resync_seq);
	MLX5_SET(transport_static_params, ctx, pda, queue->pda);
	MLX5_SET(transport_static_params, ctx, ddgst_en,
		 !!(queue->dgst & NVME_TCP_DATA_DIGEST_ENABLE));
	MLX5_SET(transport_static_params, ctx, ddgst_offload_en, ddgst_offload_en);
	MLX5_SET(transport_static_params, ctx, hddgst_en,
		 !!(queue->dgst & NVME_TCP_HDR_DIGEST_ENABLE));
	MLX5_SET(transport_static_params, ctx, hdgst_offload_en, 0);
	MLX5_SET(transport_static_params, ctx, ti,
		 MLX5_TRANSPORT_STATIC_PARAMS_TI_INITIATOR);
	MLX5_SET(transport_static_params, ctx, cccid_ttag, 1);
	MLX5_SET(transport_static_params, ctx, zero_copy_en, 1);
}

void
build_nvmeotcp_static_params(struct mlx5e_nvmeotcp_queue *queue,
			     struct mlx5e_set_transport_static_params_wqe *wqe,
			     u32 resync_seq, bool crc_rx)
{
	u8 opc_mod = MLX5_OPC_MOD_TRANSPORT_TIR_STATIC_PARAMS;
	struct mlx5_wqe_umr_ctrl_seg *ucseg = &wqe->uctrl;
	struct mlx5_wqe_ctrl_seg      *cseg = &wqe->ctrl;
	u32 sqn = queue->sq.sqn;
	u16 pc = queue->sq.pc;

	memset(wqe, 0, MLX5E_TRANSPORT_STATIC_PARAMS_WQE_SZ);

	cseg->opmod_idx_opcode = cpu_to_be32((pc << MLX5_WQE_CTRL_WQE_INDEX_SHIFT) |
					     MLX5_OPCODE_UMR | (opc_mod) << 24);
	cseg->qpn_ds = cpu_to_be32((sqn << MLX5_WQE_CTRL_QPN_SHIFT) |
				   MLX5E_TRANSPORT_STATIC_PARAMS_DS_CNT);
	cseg->imm = cpu_to_be32(mlx5e_tir_get_tirn(&queue->tir)
				<< MLX5_WQE_CTRL_TIR_TIS_INDEX_SHIFT);

	ucseg->flags = MLX5_UMR_INLINE;
	ucseg->bsf_octowords = cpu_to_be16(MLX5E_TRANSPORT_STATIC_PARAMS_OCTWORD_SIZE);
	fill_nvmeotcp_static_params(queue, &wqe->params, resync_seq, crc_rx);
}

static void
mlx5e_nvmeotcp_fill_wi(struct mlx5e_nvmeotcp_queue *nvmeotcp_queue,
		       struct mlx5e_icosq *sq, u32 wqebbs,
		       u16 pi, u16 ccid, enum wqe_type type)
{
	struct mlx5e_icosq_wqe_info *wi = &sq->db.wqe_info[pi];

	memset(wi, 0, sizeof(*wi));

	wi->num_wqebbs = wqebbs;
	switch (type) {
	case SET_PSV_UMR:
		wi->wqe_type = MLX5E_ICOSQ_WQE_SET_PSV_NVMEOTCP;
		wi->nvmeotcp_q.queue = nvmeotcp_queue;
		break;
	case KLM_INV_UMR:
		wi->wqe_type = MLX5E_ICOSQ_WQE_UMR_NVMEOTCP_INVALIDATE;
		wi->nvmeotcp_qe.entry = &nvmeotcp_queue->ccid_table[ccid];
		break;
	default:
		/* cases where no further action is required upon completion, such as ddp setup */
		wi->wqe_type = MLX5E_ICOSQ_WQE_UMR_NVMEOTCP;
		break;
	}
}

static void
mlx5e_nvmeotcp_rx_post_static_params_wqe(struct mlx5e_nvmeotcp_queue *queue, u32 resync_seq)
{
	struct mlx5e_set_transport_static_params_wqe *wqe;
	struct mlx5e_icosq *sq = &queue->sq;
	u16 pi, wqebbs;

	spin_lock_bh(&queue->sq_lock);
	wqebbs = MLX5E_TRANSPORT_SET_STATIC_PARAMS_WQEBBS;
	pi = mlx5e_icosq_get_next_pi(sq, wqebbs);
	wqe = MLX5E_TRANSPORT_FETCH_SET_STATIC_PARAMS_WQE(sq, pi);
	mlx5e_nvmeotcp_fill_wi(NULL, sq, wqebbs, pi, 0, BSF_UMR);
	build_nvmeotcp_static_params(queue, wqe, resync_seq, queue->crc_rx);
	sq->pc += wqebbs;
	mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, &wqe->ctrl);
	spin_unlock_bh(&queue->sq_lock);
}

static void
mlx5e_nvmeotcp_rx_post_progress_params_wqe(struct mlx5e_nvmeotcp_queue *queue, u32 seq)
{
	struct mlx5e_set_nvmeotcp_progress_params_wqe *wqe;
	struct mlx5e_icosq *sq = &queue->sq;
	u16 pi, wqebbs;

	wqebbs = MLX5E_NVMEOTCP_PROGRESS_PARAMS_WQEBBS;
	pi = mlx5e_icosq_get_next_pi(sq, wqebbs);
	wqe = MLX5E_NVMEOTCP_FETCH_PROGRESS_PARAMS_WQE(sq, pi);
	mlx5e_nvmeotcp_fill_wi(queue, sq, wqebbs, pi, 0, SET_PSV_UMR);
	build_nvmeotcp_progress_params(queue, wqe, seq);
	sq->pc += wqebbs;
	mlx5e_notify_hw(&sq->wq, sq->pc, sq->uar_map, &wqe->ctrl);
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
	mlx5e_nvmeotcp_fill_wi(queue, sq, wqebbs, pi, ccid, wqe_type);
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

	if (wqe_type == KLM_INV_UMR)
		wqes = 1;
	else
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

static int mlx5e_create_nvmeotcp_mkey(struct mlx5_core_dev *mdev, u8 access_mode,
				      u32 translation_octword_size, u32 *mkey)
{
	int inlen = MLX5_ST_SZ_BYTES(create_mkey_in);
	void *mkc;
	u32 *in;
	int err;

	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	MLX5_SET(mkc, mkc, free, 1);
	MLX5_SET(mkc, mkc, translations_octword_size, translation_octword_size);
	MLX5_SET(mkc, mkc, umr_en, 1);
	MLX5_SET(mkc, mkc, lw, 1);
	MLX5_SET(mkc, mkc, lr, 1);
	MLX5_SET(mkc, mkc, access_mode_1_0, access_mode);

	MLX5_SET(mkc, mkc, qpn, 0xffffff);
	MLX5_SET(mkc, mkc, pd, mdev->mlx5e_res.hw_objs.pdn);

	err = mlx5_core_create_mkey(mdev, mkey, in, inlen);

	kvfree(in);
	return err;
}

static int
mlx5e_nvmeotcp_offload_limits(struct net_device *netdev,
			      struct ulp_ddp_limits *limits)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;

	if (limits->type != ULP_DDP_NVME)
		return -EOPNOTSUPP;

	limits->max_ddp_sgl_len = mlx5e_get_max_sgl(mdev);
	limits->io_threshold = MLX5E_NVMEOTCP_IO_THRESHOLD;
	limits->tls = false;
	limits->nvmeotcp.full_ccid_range = MLX5E_NVMEOTCP_FULL_CCID_RANGE;
	return 0;
}

static int mlx5e_nvmeotcp_queue_handler_poll(struct napi_struct *napi, int budget)
{
	struct mlx5e_nvmeotcp_queue_handler *qh;
	int work_done;

	qh = container_of(napi, struct mlx5e_nvmeotcp_queue_handler, napi);

	work_done = mlx5e_poll_ico_cq(qh->cq, budget);

	if (work_done == budget || !napi_complete_done(napi, work_done))
		goto out;

	mlx5e_cq_arm(qh->cq);

out:
	return work_done;
}

static void
mlx5e_nvmeotcp_destroy_icosq(struct mlx5e_icosq *sq)
{
	mlx5e_close_icosq(sq);
	mlx5e_close_cq(&sq->cq);
}

static void mlx5e_nvmeotcp_icosq_err_cqe_work(struct work_struct *recover_work)
{
	struct mlx5e_icosq *sq = container_of(recover_work, struct mlx5e_icosq, recover_work);

	/* Not implemented yet. */

	netdev_warn(sq->channel->netdev, "nvmeotcp icosq recovery is not implemented\n");
}

static int
mlx5e_nvmeotcp_build_icosq(struct mlx5e_nvmeotcp_queue *queue, struct mlx5e_priv *priv, int io_cpu)
{
	u16 max_sgl, max_klm_per_wqe, max_umr_per_ccid, sgl_rest, wqebbs_rest;
	struct mlx5e_channel *c = priv->channels.c[queue->channel_ix];
	struct mlx5e_sq_param icosq_param = {};
	struct mlx5e_create_cq_param ccp = {};
	struct dim_cq_moder icocq_moder = {};
	struct mlx5e_icosq *icosq;
	int err = -ENOMEM;
	u16 log_icosq_sz;
	u32 max_wqebbs;

	icosq = &queue->sq;
	max_sgl = mlx5e_get_max_sgl(priv->mdev);
	max_klm_per_wqe = queue->max_klms_per_wqe;
	max_umr_per_ccid = max_sgl / max_klm_per_wqe;
	sgl_rest = max_sgl % max_klm_per_wqe;
	wqebbs_rest = sgl_rest ? MLX5E_KLM_UMR_WQEBBS(sgl_rest) : 0;
	max_wqebbs = (MLX5E_KLM_UMR_WQEBBS(max_klm_per_wqe) *
		     max_umr_per_ccid + wqebbs_rest) * queue->size;
	log_icosq_sz = order_base_2(max_wqebbs);

	mlx5e_build_icosq_param(priv->mdev, log_icosq_sz, &icosq_param);
	ccp.napi = &queue->qh.napi;
	ccp.ch_stats = &priv->channel_stats[queue->channel_ix]->ch;
	ccp.node = cpu_to_node(io_cpu);
	ccp.ix = queue->channel_ix;

	err = mlx5e_open_cq(priv, icocq_moder, &icosq_param.cqp, &ccp, &icosq->cq);
	if (err)
		goto err_nvmeotcp_sq;
	err = mlx5e_open_icosq(c, &priv->channels.params, &icosq_param, icosq,
			       mlx5e_nvmeotcp_icosq_err_cqe_work);
	if (err)
		goto close_cq;

	spin_lock_init(&queue->sq_lock);
	return 0;

close_cq:
	mlx5e_close_cq(&icosq->cq);
err_nvmeotcp_sq:
	return err;
}

static void
mlx5e_nvmeotcp_destroy_rx(struct mlx5e_priv *priv, struct mlx5e_nvmeotcp_queue *queue,
			  struct mlx5_core_dev *mdev)
{
	int i;

	mlx5e_accel_fs_del_sk(queue->fh);

	for (i = 0; i < queue->size; i++)
		mlx5_core_destroy_mkey(mdev, queue->ccid_table[i].klm_mkey);

	mlx5e_tir_destroy(&queue->tir);
	mlx5_destroy_nvmeotcp_tag_buf_table(mdev, queue->tag_buf_table_id);

	mlx5e_deactivate_icosq(&queue->sq);
	napi_disable(&queue->qh.napi);
	mlx5e_nvmeotcp_destroy_icosq(&queue->sq);
	netif_napi_del(&queue->qh.napi);
}

static int
mlx5e_nvmeotcp_queue_rx_init(struct mlx5e_nvmeotcp_queue *queue,
			     struct ulp_ddp_config *config,
			     struct net_device *netdev)
{
	struct nvme_tcp_ddp_config *nvme_config = &config->nvmeotcp;
	u8 log_queue_size = order_base_2(nvme_config->queue_size);
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	struct sock *sk = queue->sk;
	int err, max_sgls, i;

	if (nvme_config->queue_size >
	    BIT(MLX5_CAP_DEV_NVMEOTCP(mdev, log_max_nvmeotcp_tag_buffer_size)))
		return -EINVAL;

	err = mlx5e_create_nvmeotcp_tag_buf_table(mdev, queue, log_queue_size);
	if (err)
		return err;

	queue->qh.cq = &queue->sq.cq;
	netif_napi_add(priv->netdev, &queue->qh.napi, mlx5e_nvmeotcp_queue_handler_poll);

	mutex_lock(&priv->state_lock);
	err = mlx5e_nvmeotcp_build_icosq(queue, priv, config->io_cpu);
	mutex_unlock(&priv->state_lock);
	if (err)
		goto del_napi;

	napi_enable(&queue->qh.napi);
	mlx5e_activate_icosq(&queue->sq);

	/* initializes queue->tir */
	err = mlx5e_rx_res_nvmeotcp_tir_create(priv->rx_res, queue->channel_ix, queue->crc_rx,
					       queue->tag_buf_table_id, &queue->tir);
	if (err)
		goto destroy_icosq;

	mlx5e_nvmeotcp_rx_post_static_params_wqe(queue, 0);
	mlx5e_nvmeotcp_rx_post_progress_params_wqe(queue, tcp_sk(sk)->copied_seq);

	queue->ccid_table = kcalloc(queue->size, sizeof(struct mlx5e_nvmeotcp_queue_entry),
				    GFP_KERNEL);
	if (!queue->ccid_table) {
		err = -ENOMEM;
		goto destroy_tir;
	}

	max_sgls = mlx5e_get_max_sgl(mdev);
	for (i = 0; i < queue->size; i++) {
		err = mlx5e_create_nvmeotcp_mkey(mdev, MLX5_MKC_ACCESS_MODE_KLMS, max_sgls,
						 &queue->ccid_table[i].klm_mkey);
		if (err)
			goto free_ccid_table;
	}

	mlx5e_nvmeotcp_post_klm_wqe(queue, BSF_KLM_UMR, 0, queue->size);

	if (!(WARN_ON(!wait_for_completion_timeout(&queue->static_params_done,
						   msecs_to_jiffies(3000)))))
		queue->fh = mlx5e_accel_fs_add_sk(priv->fs, sk, mlx5e_tir_get_tirn(&queue->tir),
						  queue->id);

	if (IS_ERR_OR_NULL(queue->fh)) {
		err = -EINVAL;
		goto destroy_mkeys;
	}

	return 0;

destroy_mkeys:
	while ((i--))
		mlx5_core_destroy_mkey(mdev, queue->ccid_table[i].klm_mkey);
free_ccid_table:
	kfree(queue->ccid_table);
destroy_tir:
	mlx5e_tir_destroy(&queue->tir);
destroy_icosq:
	mlx5e_deactivate_icosq(&queue->sq);
	napi_disable(&queue->qh.napi);
	mlx5e_nvmeotcp_destroy_icosq(&queue->sq);
del_napi:
	netif_napi_del(&queue->qh.napi);
	mlx5_destroy_nvmeotcp_tag_buf_table(mdev, queue->tag_buf_table_id);

	return err;
}

static int
mlx5e_nvmeotcp_queue_init(struct net_device *netdev,
			  struct sock *sk,
			  struct ulp_ddp_config *config)
{
	struct nvme_tcp_ddp_config *nvme_config = &config->nvmeotcp;
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_nvmeotcp_queue *queue;
	int queue_id, err;

	if (config->type != ULP_DDP_NVME) {
		err = -EOPNOTSUPP;
		goto out;
	}

	queue = kzalloc(sizeof(*queue), GFP_KERNEL);
	if (!queue) {
		err = -ENOMEM;
		goto out;
	}

	queue_id = ida_simple_get(&priv->nvmeotcp->queue_ids,
				  MIN_NUM_NVMEOTCP_QUEUES, MAX_NUM_NVMEOTCP_QUEUES,
				  GFP_KERNEL);
	if (queue_id < 0) {
		err = -ENOSPC;
		goto free_queue;
	}

	queue->crc_rx = !!(nvme_config->dgst & NVME_TCP_DATA_DIGEST_ENABLE);
	queue->ulp_ddp_ctx.type = ULP_DDP_NVME;
	queue->sk = sk;
	queue->id = queue_id;
	queue->dgst = nvme_config->dgst;
	queue->pda = nvme_config->cpda;
	queue->channel_ix = mlx5e_get_channel_ix_from_io_cpu(&priv->channels.params,
							     config->io_cpu);
	queue->size = nvme_config->queue_size;
	queue->max_klms_per_wqe = MLX5E_MAX_KLM_PER_WQE(mdev);
	queue->priv = priv;
	init_completion(&queue->static_params_done);

	err = mlx5e_nvmeotcp_queue_rx_init(queue, config, netdev);
	if (err)
		goto remove_queue_id;

	err = rhashtable_insert_fast(&priv->nvmeotcp->queue_hash, &queue->hash,
				     rhash_queues);
	if (err)
		goto destroy_rx;

	write_lock_bh(&sk->sk_callback_lock);
	ulp_ddp_set_ctx(sk, queue);
	write_unlock_bh(&sk->sk_callback_lock);
	refcount_set(&queue->ref_count, 1);
	return 0;

destroy_rx:
	mlx5e_nvmeotcp_destroy_rx(priv, queue, mdev);
remove_queue_id:
	ida_simple_remove(&priv->nvmeotcp->queue_ids, queue_id);
free_queue:
	kfree(queue);
out:
	return err;
}

static void
mlx5e_nvmeotcp_queue_teardown(struct net_device *netdev,
			      struct sock *sk)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_nvmeotcp_queue *queue;

	queue = container_of(ulp_ddp_get_ctx(sk), struct mlx5e_nvmeotcp_queue, ulp_ddp_ctx);

	WARN_ON(refcount_read(&queue->ref_count) != 1);
	mlx5e_nvmeotcp_destroy_rx(priv, queue, mdev);

	rhashtable_remove_fast(&priv->nvmeotcp->queue_hash, &queue->hash,
			       rhash_queues);
	ida_simple_remove(&priv->nvmeotcp->queue_ids, queue->id);
	write_lock_bh(&sk->sk_callback_lock);
	ulp_ddp_set_ctx(sk, NULL);
	write_unlock_bh(&sk->sk_callback_lock);
	mlx5e_nvmeotcp_put_queue(queue);
}

static bool
mlx5e_nvmeotcp_validate_small_sgl_suffix(struct scatterlist *sg, int sg_len, int mtu)
{
	int i, hole_size, hole_len, chunk_size = 0;

	for (i = 1; i < sg_len; i++)
		chunk_size += sg_dma_len(&sg[i]);

	if (chunk_size >= mtu)
		return true;

	hole_size = mtu - chunk_size - 1;
	hole_len = DIV_ROUND_UP(hole_size, PAGE_SIZE);

	if (sg_len + hole_len > MAX_SKB_FRAGS)
		return false;

	return true;
}

static bool
mlx5e_nvmeotcp_validate_big_sgl_suffix(struct scatterlist *sg, int sg_len, int mtu)
{
	int i, j, last_elem, window_idx, window_size = MAX_SKB_FRAGS - 1;
	int chunk_size = 0;

	last_elem = sg_len - window_size;
	window_idx = window_size;

	for (j = 1; j < window_size; j++)
		chunk_size += sg_dma_len(&sg[j]);

	for (i = 1; i <= last_elem; i++, window_idx++) {
		chunk_size += sg_dma_len(&sg[window_idx]);
		if (chunk_size < mtu - 1)
			return false;

		chunk_size -= sg_dma_len(&sg[i]);
	}

	return true;
}

/* This function makes sure that the middle/suffix of a PDU SGL meets the
 * restriction of MAX_SKB_FRAGS. There are two cases here:
 * 1. sg_len < MAX_SKB_FRAGS - the extreme case here is a packet that consists
 * of one byte from the first SG element + the rest of the SGL and the remaining
 * space of the packet will be scattered to the WQE and will be pointed by
 * SKB frags.
 * 2. sg_len => MAX_SKB_FRAGS - the extreme case here is a packet that consists
 * of one byte from middle SG element + 15 continuous SG elements + one byte
 * from a sequential SG element or the rest of the packet.
 */
static bool
mlx5e_nvmeotcp_validate_sgl_suffix(struct scatterlist *sg, int sg_len, int mtu)
{
	int ret;

	if (sg_len < MAX_SKB_FRAGS)
		ret = mlx5e_nvmeotcp_validate_small_sgl_suffix(sg, sg_len, mtu);
	else
		ret = mlx5e_nvmeotcp_validate_big_sgl_suffix(sg, sg_len, mtu);

	return ret;
}

static bool
mlx5e_nvmeotcp_validate_sgl_prefix(struct scatterlist *sg, int sg_len, int mtu)
{
	int i, hole_size, hole_len, tmp_len, chunk_size = 0;

	tmp_len = min_t(int, sg_len, MAX_SKB_FRAGS);

	for (i = 0; i < tmp_len; i++)
		chunk_size += sg_dma_len(&sg[i]);

	if (chunk_size >= mtu)
		return true;

	hole_size = mtu - chunk_size;
	hole_len = DIV_ROUND_UP(hole_size, PAGE_SIZE);

	if (tmp_len + hole_len > MAX_SKB_FRAGS)
		return false;

	return true;
}

/* This function is responsible to ensure that a PDU could be offloaded.
 * PDU is offloaded by building a non-linear SKB such that each SGL element is
 * placed in frag, thus this function should ensure that all packets that
 * represent part of the PDU won't exaggerate from MAX_SKB_FRAGS SGL.
 * In addition NVMEoTCP offload has one PDU offload for packet restriction.
 * Packet could start with a new PDU and then we should check that the prefix
 * of the PDU meets the requirement or a packet can start in the middle of SG
 * element and then we should check that the suffix of PDU meets the requirement.
 */
static bool
mlx5e_nvmeotcp_validate_sgl(struct scatterlist *sg, int sg_len, int mtu)
{
	int max_hole_frags;

	max_hole_frags = DIV_ROUND_UP(mtu, PAGE_SIZE);
	if (sg_len + max_hole_frags <= MAX_SKB_FRAGS)
		return true;

	if (!mlx5e_nvmeotcp_validate_sgl_prefix(sg, sg_len, mtu) ||
	    !mlx5e_nvmeotcp_validate_sgl_suffix(sg, sg_len, mtu))
		return false;

	return true;
}

static int
mlx5e_nvmeotcp_ddp_setup(struct net_device *netdev,
			 struct sock *sk,
			 struct ulp_ddp_io *ddp)
{
	struct scatterlist *sg = ddp->sg_table.sgl;
	struct mlx5e_nvmeotcp_queue_entry *nvqt;
	struct mlx5e_nvmeotcp_queue *queue;
	struct mlx5_core_dev *mdev;
	int i, size = 0, count = 0;

	queue = container_of(ulp_ddp_get_ctx(sk),
			     struct mlx5e_nvmeotcp_queue, ulp_ddp_ctx);
	mdev = queue->priv->mdev;
	count = dma_map_sg(mdev->device, ddp->sg_table.sgl, ddp->nents,
			   DMA_FROM_DEVICE);

	if (count <= 0)
		return -EINVAL;

	if (WARN_ON(count > mlx5e_get_max_sgl(mdev)))
		return -ENOSPC;

	if (!mlx5e_nvmeotcp_validate_sgl(sg, count, READ_ONCE(netdev->mtu)))
		return -EOPNOTSUPP;

	for (i = 0; i < count; i++)
		size += sg_dma_len(&sg[i]);

	nvqt = &queue->ccid_table[ddp->command_id];
	nvqt->size = size;
	nvqt->ddp = ddp;
	nvqt->sgl = sg;
	nvqt->ccid_gen++;
	nvqt->sgl_length = count;
	mlx5e_nvmeotcp_post_klm_wqe(queue, KLM_UMR, ddp->command_id, count);

	return 0;
}

void mlx5e_nvmeotcp_ctx_complete(struct mlx5e_icosq_wqe_info *wi)
{
	struct mlx5e_nvmeotcp_queue *queue = wi->nvmeotcp_q.queue;

	complete(&queue->static_params_done);
}

void mlx5e_nvmeotcp_ddp_inv_done(struct mlx5e_icosq_wqe_info *wi)
{
	struct mlx5e_nvmeotcp_queue_entry *q_entry = wi->nvmeotcp_qe.entry;
	struct mlx5e_nvmeotcp_queue *queue = q_entry->queue;
	struct mlx5_core_dev *mdev = queue->priv->mdev;
	struct ulp_ddp_io *ddp = q_entry->ddp;
	const struct ulp_ddp_ulp_ops *ulp_ops;

	dma_unmap_sg(mdev->device, ddp->sg_table.sgl,
		     q_entry->sgl_length, DMA_FROM_DEVICE);

	q_entry->sgl_length = 0;

	ulp_ops = inet_csk(queue->sk)->icsk_ulp_ddp_ops;
	if (ulp_ops && ulp_ops->ddp_teardown_done)
		ulp_ops->ddp_teardown_done(q_entry->ddp_ctx);
}

static void
mlx5e_nvmeotcp_ddp_teardown(struct net_device *netdev,
			    struct sock *sk,
			    struct ulp_ddp_io *ddp,
			    void *ddp_ctx)
{
	struct mlx5e_nvmeotcp_queue_entry *q_entry;
	struct mlx5e_nvmeotcp_queue *queue;

	queue = container_of(ulp_ddp_get_ctx(sk), struct mlx5e_nvmeotcp_queue, ulp_ddp_ctx);
	q_entry  = &queue->ccid_table[ddp->command_id];
	WARN_ONCE(q_entry->sgl_length == 0,
		  "Invalidation of empty sgl (CID 0x%x, queue 0x%x)\n",
		  ddp->command_id, queue->id);

	q_entry->ddp_ctx = ddp_ctx;
	q_entry->queue = queue;

	mlx5e_nvmeotcp_post_klm_wqe(queue, KLM_INV_UMR, ddp->command_id, 0);
}

static void
mlx5e_nvmeotcp_ddp_resync(struct net_device *netdev,
			  struct sock *sk, u32 seq)
{
	struct mlx5e_nvmeotcp_queue *queue =
		container_of(ulp_ddp_get_ctx(sk), struct mlx5e_nvmeotcp_queue, ulp_ddp_ctx);

	queue->after_resync_cqe = 1;
	mlx5e_nvmeotcp_rx_post_static_params_wqe(queue, seq);
}

struct mlx5e_nvmeotcp_queue *
mlx5e_nvmeotcp_get_queue(struct mlx5e_nvmeotcp *nvmeotcp, int id)
{
	struct mlx5e_nvmeotcp_queue *queue;

	queue = rhashtable_lookup_fast(&nvmeotcp->queue_hash,
				       &id, rhash_queues);
	if (!IS_ERR_OR_NULL(queue))
		refcount_inc(&queue->ref_count);
	return queue;
}

void mlx5e_nvmeotcp_put_queue(struct mlx5e_nvmeotcp_queue *queue)
{
	if (refcount_dec_and_test(&queue->ref_count)) {
		kfree(queue->ccid_table);
		kfree(queue);
	}
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
