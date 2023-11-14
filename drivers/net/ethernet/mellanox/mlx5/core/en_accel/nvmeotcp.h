/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. */
#ifndef __MLX5E_NVMEOTCP_H__
#define __MLX5E_NVMEOTCP_H__

#ifdef CONFIG_MLX5_EN_NVMEOTCP

#include <net/ulp_ddp.h>
#include "en.h"
#include "en/params.h"

struct mlx5e_nvmeotcp_queue_entry {
	struct mlx5e_nvmeotcp_queue *queue;
	u32 sgl_length;
	u32 klm_mkey;
	struct scatterlist *sgl;
	u32 ccid_gen;
	u64 size;

	/* for the ddp invalidate done callback */
	void *ddp_ctx;
	struct ulp_ddp_io *ddp;
};

struct mlx5e_nvmeotcp_queue_handler {
	struct napi_struct napi;
	struct mlx5e_cq *cq;
};

/**
 *	struct mlx5e_nvmeotcp_queue - mlx5 metadata for NVMEoTCP queue
 *	@ulp_ddp_ctx: Generic ulp ddp context
 *	@tir: Destination TIR created for NVMEoTCP offload
 *	@fh: Flow handle representing the 5-tuple steering for this flow
 *	@id: Flow tag ID used to identify this queue
 *	@size: NVMEoTCP queue depth
 *	@ccid_gen: Generation ID for the CCID, used to avoid conflicts in DDP
 *	@max_klms_per_wqe: Number of KLMs per DDP operation
 *	@hash: Hash table of queues mapped by @id
 *	@pda: Padding alignment
 *	@tag_buf_table_id: Tag buffer table for CCIDs
 *	@dgst: Digest supported (header and/or data)
 *	@sq: Send queue used for posting umrs
 *	@ref_count: Reference count for this structure
 *	@after_resync_cqe: Indicate if resync occurred
 *	@ccid_table: Table holding metadata for each CC (Command Capsule)
 *	@ccid: ID of the current CC
 *	@ccsglidx: Index within the scatter-gather list (SGL) of the current CC
 *	@ccoff: Offset within the current CC
 *	@ccoff_inner: Current offset within the @ccsglidx element
 *	@channel_ix: Channel IX for this nvmeotcp_queue
 *	@sk: The socket used by the NVMe-TCP queue
 *	@crc_rx: CRC Rx offload indication for this queue
 *	@priv: mlx5e netdev priv
 *	@static_params_done: Async completion structure for the initial umr mapping
 *	synchronization
 *	@sq_lock: Spin lock for the icosq
 *	@qh: Completion queue handler for processing umr completions
 */
struct mlx5e_nvmeotcp_queue {
	struct ulp_ddp_ctx ulp_ddp_ctx;
	struct mlx5e_tir tir;
	struct mlx5_flow_handle *fh;
	int id;
	u32 size;
	/* needed when the upper layer immediately reuses CCID + some packet loss happens */
	u32 ccid_gen;
	u32 max_klms_per_wqe;
	struct rhash_head hash;
	int pda;
	u32 tag_buf_table_id;
	u8 dgst;
	struct mlx5e_icosq sq;

	/* data-path section cache aligned */
	refcount_t ref_count;
	/* for MASK HW resync cqe */
	bool after_resync_cqe;
	struct mlx5e_nvmeotcp_queue_entry *ccid_table;
	/* current ccid fields */
	int ccid;
	int ccsglidx;
	off_t ccoff;
	int ccoff_inner;

	u32 channel_ix;
	struct sock *sk;
	u8 crc_rx:1;
	/* for ddp invalidate flow */
	struct mlx5e_priv *priv;
	/* end of data-path section */

	struct completion static_params_done;
	/* spin lock for the ico sq, ULP can issue requests from multiple contexts */
	spinlock_t sq_lock;
	struct mlx5e_nvmeotcp_queue_handler qh;
};

struct mlx5e_nvmeotcp {
	struct ida queue_ids;
	struct rhashtable queue_hash;
	struct ulp_ddp_dev_caps ddp_caps;
	bool enabled;
};

int mlx5e_nvmeotcp_init(struct mlx5e_priv *priv);
int set_ulp_ddp_nvme_tcp(struct net_device *netdev, bool enable);
void mlx5e_nvmeotcp_cleanup(struct mlx5e_priv *priv);
struct mlx5e_nvmeotcp_queue *
mlx5e_nvmeotcp_get_queue(struct mlx5e_nvmeotcp *nvmeotcp, int id);
void mlx5e_nvmeotcp_put_queue(struct mlx5e_nvmeotcp_queue *queue);
void mlx5e_nvmeotcp_ddp_inv_done(struct mlx5e_icosq_wqe_info *wi);
void mlx5e_nvmeotcp_ctx_complete(struct mlx5e_icosq_wqe_info *wi);
static inline void mlx5e_nvmeotcp_init_rx(struct mlx5e_priv *priv) {}
void mlx5e_nvmeotcp_cleanup_rx(struct mlx5e_priv *priv);
extern const struct ulp_ddp_dev_ops mlx5e_nvmeotcp_ops;
#else

static inline int mlx5e_nvmeotcp_init(struct mlx5e_priv *priv) { return 0; }
static inline void mlx5e_nvmeotcp_cleanup(struct mlx5e_priv *priv) {}
static inline int set_ulp_ddp_nvme_tcp(struct net_device *dev, bool en) { return -EOPNOTSUPP; }
static inline void mlx5e_nvmeotcp_init_rx(struct mlx5e_priv *priv) {}
static inline void mlx5e_nvmeotcp_cleanup_rx(struct mlx5e_priv *priv) {}
#endif
#endif /* __MLX5E_NVMEOTCP_H__ */
