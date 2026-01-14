// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016 HGST, a Western Digital Company.
 */
#include <linux/memremap.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/pci-p2pdma.h>
#include <rdma/mr_pool.h>
#include <rdma/rw.h>

enum {
	RDMA_RW_SINGLE_WR,
	RDMA_RW_MULTI_WR,
	RDMA_RW_MR,
	RDMA_RW_SIG_MR,
	RDMA_RW_IOVA,
};

static bool rdma_rw_force_mr;
module_param_named(force_mr, rdma_rw_force_mr, bool, 0);
MODULE_PARM_DESC(force_mr, "Force usage of MRs for RDMA READ/WRITE operations");

/*
 * Report whether memory registration should be used. Memory registration must
 * be used for iWarp devices because of iWARP-specific limitations. Memory
 * registration is also enabled if registering memory might yield better
 * performance than using multiple SGE entries, see rdma_rw_io_needs_mr()
 */
static inline bool rdma_rw_can_use_mr(struct ib_device *dev, u32 port_num)
{
	if (rdma_protocol_iwarp(dev, port_num))
		return true;
	if (dev->attrs.max_sgl_rd)
		return true;
	if (unlikely(rdma_rw_force_mr))
		return true;
	return false;
}

/*
 * Check if the device will use memory registration for this RW operation.
 * For RDMA READs we must use MRs on iWarp and can optionally use them as an
 * optimization otherwise.  Additionally we have a debug option to force usage
 * of MRs to help testing this code path.
 */
static inline bool rdma_rw_io_needs_mr(struct ib_device *dev, u32 port_num,
		enum dma_data_direction dir, int dma_nents)
{
	if (dir == DMA_FROM_DEVICE) {
		if (rdma_protocol_iwarp(dev, port_num))
			return true;
		if (dev->attrs.max_sgl_rd && dma_nents > dev->attrs.max_sgl_rd)
			return true;
	}
	if (unlikely(rdma_rw_force_mr))
		return true;
	return false;
}

static inline u32 rdma_rw_fr_page_list_len(struct ib_device *dev,
					   bool pi_support)
{
	u32 max_pages;

	if (pi_support)
		max_pages = dev->attrs.max_pi_fast_reg_page_list_len;
	else
		max_pages = dev->attrs.max_fast_reg_page_list_len;

	/* arbitrary limit to avoid allocating gigantic resources */
	return min_t(u32, max_pages, 256);
}

static inline int rdma_rw_inv_key(struct rdma_rw_reg_ctx *reg)
{
	int count = 0;

	if (reg->mr->need_inval) {
		reg->inv_wr.opcode = IB_WR_LOCAL_INV;
		reg->inv_wr.ex.invalidate_rkey = reg->mr->lkey;
		reg->inv_wr.next = &reg->reg_wr.wr;
		count++;
	} else {
		reg->inv_wr.next = NULL;
	}

	return count;
}

/* Caller must have zero-initialized *reg. */
static int rdma_rw_init_one_mr(struct ib_qp *qp, u32 port_num,
		struct rdma_rw_reg_ctx *reg, struct scatterlist *sg,
		u32 sg_cnt, u32 offset)
{
	u32 pages_per_mr = rdma_rw_fr_page_list_len(qp->pd->device,
						    qp->integrity_en);
	u32 nents = min(sg_cnt, pages_per_mr);
	int count = 0, ret;

	reg->mr = ib_mr_pool_get(qp, &qp->rdma_mrs);
	if (!reg->mr)
		return -EAGAIN;

	count += rdma_rw_inv_key(reg);

	ret = ib_map_mr_sg(reg->mr, sg, nents, &offset, PAGE_SIZE);
	if (ret < 0 || ret < nents) {
		ib_mr_pool_put(qp, &qp->rdma_mrs, reg->mr);
		return -EINVAL;
	}

	reg->reg_wr.wr.opcode = IB_WR_REG_MR;
	reg->reg_wr.mr = reg->mr;
	reg->reg_wr.access = IB_ACCESS_LOCAL_WRITE;
	if (rdma_protocol_iwarp(qp->device, port_num))
		reg->reg_wr.access |= IB_ACCESS_REMOTE_WRITE;
	count++;

	reg->sge.addr = reg->mr->iova;
	reg->sge.length = reg->mr->length;
	return count;
}

static int rdma_rw_init_mr_wrs(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		u32 port_num, struct scatterlist *sg, u32 sg_cnt, u32 offset,
		u64 remote_addr, u32 rkey, enum dma_data_direction dir)
{
	struct rdma_rw_reg_ctx *prev = NULL;
	u32 pages_per_mr = rdma_rw_fr_page_list_len(qp->pd->device,
						    qp->integrity_en);
	int i, j, ret = 0, count = 0;

	ctx->nr_ops = DIV_ROUND_UP(sg_cnt, pages_per_mr);
	ctx->reg = kcalloc(ctx->nr_ops, sizeof(*ctx->reg), GFP_KERNEL);
	if (!ctx->reg) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < ctx->nr_ops; i++) {
		struct rdma_rw_reg_ctx *reg = &ctx->reg[i];
		u32 nents = min(sg_cnt, pages_per_mr);

		ret = rdma_rw_init_one_mr(qp, port_num, reg, sg, sg_cnt,
				offset);
		if (ret < 0)
			goto out_free;
		count += ret;

		if (prev) {
			if (reg->mr->need_inval)
				prev->wr.wr.next = &reg->inv_wr;
			else
				prev->wr.wr.next = &reg->reg_wr.wr;
		}

		reg->reg_wr.wr.next = &reg->wr.wr;

		reg->wr.wr.sg_list = &reg->sge;
		reg->wr.wr.num_sge = 1;
		reg->wr.remote_addr = remote_addr;
		reg->wr.rkey = rkey;
		if (dir == DMA_TO_DEVICE) {
			reg->wr.wr.opcode = IB_WR_RDMA_WRITE;
		} else if (!rdma_cap_read_inv(qp->device, port_num)) {
			reg->wr.wr.opcode = IB_WR_RDMA_READ;
		} else {
			reg->wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
			reg->wr.wr.ex.invalidate_rkey = reg->mr->lkey;
		}
		count++;

		remote_addr += reg->sge.length;
		sg_cnt -= nents;
		for (j = 0; j < nents; j++)
			sg = sg_next(sg);
		prev = reg;
		offset = 0;
	}

	if (prev)
		prev->wr.wr.next = NULL;

	ctx->type = RDMA_RW_MR;
	return count;

out_free:
	while (--i >= 0)
		ib_mr_pool_put(qp, &qp->rdma_mrs, ctx->reg[i].mr);
	kfree(ctx->reg);
out:
	return ret;
}

static int rdma_rw_init_mr_wrs_bvec(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		u32 port_num, const struct bio_vec *bvec, u32 nr_bvec,
		u32 offset, u64 remote_addr, u32 rkey,
		enum dma_data_direction dir)
{
	struct ib_device *dev = qp->pd->device;
	struct rdma_rw_reg_ctx *prev = NULL;
	u32 pages_per_mr = rdma_rw_fr_page_list_len(dev, qp->integrity_en);
	struct scatterlist *sgl;
	int i, j, ret = 0, count = 0;
	u32 sg_idx = 0;

	ctx->nr_ops = DIV_ROUND_UP(nr_bvec, pages_per_mr);
	ctx->reg = kcalloc(ctx->nr_ops, sizeof(*ctx->reg), GFP_KERNEL);
	if (!ctx->reg)
		return -ENOMEM;

	/*
	 * Allocate synthetic scatterlist to hold DMA addresses.
	 * ib_map_mr_sg() extracts sg_dma_address/len, so the page
	 * pointer is unused.
	 */
	sgl = kmalloc_array(nr_bvec, sizeof(*sgl), GFP_KERNEL);
	if (!sgl) {
		ret = -ENOMEM;
		goto out_free_reg;
	}
	sg_init_table(sgl, nr_bvec);

	/*
	 * DMA map all bvecs and populate the synthetic scatterlist.
	 */
	for (i = 0; i < nr_bvec; i++) {
		const struct bio_vec *bv = &bvec[i];
		struct bio_vec adjusted;
		u64 dma_addr;
		u32 len;

		if (i == 0 && offset) {
			adjusted = *bv;
			adjusted.bv_offset += offset;
			adjusted.bv_len -= offset;
			bv = &adjusted;
		}
		len = bv->bv_len;

		dma_addr = ib_dma_map_bvec(dev, bv, dir);
		if (ib_dma_mapping_error(dev, dma_addr)) {
			ret = -ENOMEM;
			goto out_unmap;
		}

		/*
		 * Populate sg entry with DMA address. sg_set_page() is
		 * called to initialize the entry, but the page pointer
		 * is unused by ib_map_mr_sg().
		 */
		sg_set_page(&sgl[i], bv->bv_page, len, bv->bv_offset);
		sg_dma_address(&sgl[i]) = dma_addr;
		sg_dma_len(&sgl[i]) = len;
	}

	/*
	 * Build MR chain using the synthetic scatterlist.
	 */
	for (i = 0; i < ctx->nr_ops; i++) {
		struct rdma_rw_reg_ctx *reg = &ctx->reg[i];
		u32 nents = min(nr_bvec - sg_idx, pages_per_mr);

		ret = rdma_rw_init_one_mr(qp, port_num, reg, &sgl[sg_idx],
					  nents, 0);
		if (ret < 0)
			goto out_free_mrs;
		count += ret;

		if (prev) {
			if (reg->mr->need_inval)
				prev->wr.wr.next = &reg->inv_wr;
			else
				prev->wr.wr.next = &reg->reg_wr.wr;
		}

		reg->reg_wr.wr.next = &reg->wr.wr;

		reg->wr.wr.sg_list = &reg->sge;
		reg->wr.wr.num_sge = 1;
		reg->wr.remote_addr = remote_addr;
		reg->wr.rkey = rkey;

		if (dir == DMA_TO_DEVICE) {
			reg->wr.wr.opcode = IB_WR_RDMA_WRITE;
		} else if (!rdma_cap_read_inv(qp->device, port_num)) {
			reg->wr.wr.opcode = IB_WR_RDMA_READ;
		} else {
			reg->wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
			reg->wr.wr.ex.invalidate_rkey = reg->mr->lkey;
		}
		count++;

		remote_addr += reg->sge.length;
		sg_idx += nents;
		prev = reg;
	}

	if (prev)
		prev->wr.wr.next = NULL;

	ctx->type = RDMA_RW_MR;
	ctx->mr_sgl = sgl;
	ctx->mr_sg_cnt = nr_bvec;
	return count;

out_free_mrs:
	while (--i >= 0)
		ib_mr_pool_put(qp, &qp->rdma_mrs, ctx->reg[i].mr);
	/* All bvecs were mapped successfully, unmap them all */
	for (j = 0; j < nr_bvec; j++)
		ib_dma_unmap_bvec(dev, sg_dma_address(&sgl[j]),
				  sg_dma_len(&sgl[j]), dir);
	kfree(sgl);
	kfree(ctx->reg);
	return ret;

out_unmap:
	/* Unmap bvecs that were successfully mapped (0 through i-1) */
	for (j = 0; j < i; j++)
		ib_dma_unmap_bvec(dev, sg_dma_address(&sgl[j]),
				  sg_dma_len(&sgl[j]), dir);
	kfree(sgl);
out_free_reg:
	kfree(ctx->reg);
	return ret;
}

static int rdma_rw_init_map_wrs(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		struct scatterlist *sg, u32 sg_cnt, u32 offset,
		u64 remote_addr, u32 rkey, enum dma_data_direction dir)
{
	u32 max_sge = dir == DMA_TO_DEVICE ? qp->max_write_sge :
		      qp->max_read_sge;
	struct ib_sge *sge;
	u32 total_len = 0, i, j;

	ctx->nr_ops = DIV_ROUND_UP(sg_cnt, max_sge);

	ctx->map.sges = sge = kcalloc(sg_cnt, sizeof(*sge), GFP_KERNEL);
	if (!ctx->map.sges)
		goto out;

	ctx->map.wrs = kcalloc(ctx->nr_ops, sizeof(*ctx->map.wrs), GFP_KERNEL);
	if (!ctx->map.wrs)
		goto out_free_sges;

	for (i = 0; i < ctx->nr_ops; i++) {
		struct ib_rdma_wr *rdma_wr = &ctx->map.wrs[i];
		u32 nr_sge = min(sg_cnt, max_sge);

		if (dir == DMA_TO_DEVICE)
			rdma_wr->wr.opcode = IB_WR_RDMA_WRITE;
		else
			rdma_wr->wr.opcode = IB_WR_RDMA_READ;
		rdma_wr->remote_addr = remote_addr + total_len;
		rdma_wr->rkey = rkey;
		rdma_wr->wr.num_sge = nr_sge;
		rdma_wr->wr.sg_list = sge;

		for (j = 0; j < nr_sge; j++, sg = sg_next(sg)) {
			sge->addr = sg_dma_address(sg) + offset;
			sge->length = sg_dma_len(sg) - offset;
			sge->lkey = qp->pd->local_dma_lkey;

			total_len += sge->length;
			sge++;
			sg_cnt--;
			offset = 0;
		}

		rdma_wr->wr.next = i + 1 < ctx->nr_ops ?
			&ctx->map.wrs[i + 1].wr : NULL;
	}

	ctx->type = RDMA_RW_MULTI_WR;
	return ctx->nr_ops;

out_free_sges:
	kfree(ctx->map.sges);
out:
	return -ENOMEM;
}

static int rdma_rw_init_single_wr(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		struct scatterlist *sg, u32 offset, u64 remote_addr, u32 rkey,
		enum dma_data_direction dir)
{
	struct ib_rdma_wr *rdma_wr = &ctx->single.wr;

	ctx->nr_ops = 1;

	ctx->single.sge.lkey = qp->pd->local_dma_lkey;
	ctx->single.sge.addr = sg_dma_address(sg) + offset;
	ctx->single.sge.length = sg_dma_len(sg) - offset;

	memset(rdma_wr, 0, sizeof(*rdma_wr));
	if (dir == DMA_TO_DEVICE)
		rdma_wr->wr.opcode = IB_WR_RDMA_WRITE;
	else
		rdma_wr->wr.opcode = IB_WR_RDMA_READ;
	rdma_wr->wr.sg_list = &ctx->single.sge;
	rdma_wr->wr.num_sge = 1;
	rdma_wr->remote_addr = remote_addr;
	rdma_wr->rkey = rkey;

	ctx->type = RDMA_RW_SINGLE_WR;
	return 1;
}

static int rdma_rw_init_single_wr_bvec(struct rdma_rw_ctx *ctx,
		struct ib_qp *qp, const struct bio_vec *bvec, u32 offset,
		u64 remote_addr, u32 rkey, enum dma_data_direction dir)
{
	struct ib_device *dev = qp->pd->device;
	struct ib_rdma_wr *rdma_wr = &ctx->single.wr;
	struct bio_vec adjusted = *bvec;
	u64 dma_addr;

	ctx->nr_ops = 1;

	if (offset) {
		adjusted.bv_offset += offset;
		adjusted.bv_len -= offset;
	}

	dma_addr = ib_dma_map_bvec(dev, &adjusted, dir);
	if (ib_dma_mapping_error(dev, dma_addr))
		return -ENOMEM;

	ctx->single.sge.lkey = qp->pd->local_dma_lkey;
	ctx->single.sge.addr = dma_addr;
	ctx->single.sge.length = adjusted.bv_len;

	memset(rdma_wr, 0, sizeof(*rdma_wr));
	if (dir == DMA_TO_DEVICE)
		rdma_wr->wr.opcode = IB_WR_RDMA_WRITE;
	else
		rdma_wr->wr.opcode = IB_WR_RDMA_READ;
	rdma_wr->wr.sg_list = &ctx->single.sge;
	rdma_wr->wr.num_sge = 1;
	rdma_wr->remote_addr = remote_addr;
	rdma_wr->rkey = rkey;

	ctx->type = RDMA_RW_SINGLE_WR;
	return 1;
}

static int rdma_rw_init_map_wrs_bvec(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		const struct bio_vec *bvec, u32 nr_bvec, u32 offset,
		u64 remote_addr, u32 rkey, enum dma_data_direction dir)
{
	struct ib_device *dev = qp->pd->device;
	u32 max_sge = dir == DMA_TO_DEVICE ? qp->max_write_sge :
		      qp->max_read_sge;
	struct ib_sge *sge;
	u32 total_len = 0, i, j, bvec_idx = 0;
	u32 mapped_bvecs = 0;
	u64 dma_addr;

	ctx->nr_ops = DIV_ROUND_UP(nr_bvec, max_sge);

	ctx->map.sges = sge = kcalloc(nr_bvec, sizeof(*sge), GFP_KERNEL);
	if (!ctx->map.sges)
		return -ENOMEM;

	ctx->map.wrs = kcalloc(ctx->nr_ops, sizeof(*ctx->map.wrs), GFP_KERNEL);
	if (!ctx->map.wrs)
		goto out_free_sges;

	for (i = 0; i < ctx->nr_ops; i++) {
		struct ib_rdma_wr *rdma_wr = &ctx->map.wrs[i];
		u32 nr_sge = min(nr_bvec - bvec_idx, max_sge);

		if (dir == DMA_TO_DEVICE)
			rdma_wr->wr.opcode = IB_WR_RDMA_WRITE;
		else
			rdma_wr->wr.opcode = IB_WR_RDMA_READ;
		rdma_wr->remote_addr = remote_addr + total_len;
		rdma_wr->rkey = rkey;
		rdma_wr->wr.num_sge = nr_sge;
		rdma_wr->wr.sg_list = sge;

		for (j = 0; j < nr_sge; j++, bvec_idx++) {
			const struct bio_vec *bv = &bvec[bvec_idx];
			u32 len = bv->bv_len;

			/* Handle offset into first bvec */
			if (bvec_idx == 0 && offset) {
				struct bio_vec adjusted = *bv;

				adjusted.bv_offset += offset;
				adjusted.bv_len -= offset;
				dma_addr = ib_dma_map_bvec(dev, &adjusted, dir);
				len = adjusted.bv_len;
			} else {
				dma_addr = ib_dma_map_bvec(dev, bv, dir);
			}

			if (ib_dma_mapping_error(dev, dma_addr))
				goto out_unmap;

			mapped_bvecs++;
			sge->addr = dma_addr;
			sge->length = len;
			sge->lkey = qp->pd->local_dma_lkey;

			total_len += len;
			sge++;
		}

		rdma_wr->wr.next = i + 1 < ctx->nr_ops ?
			&ctx->map.wrs[i + 1].wr : NULL;
	}

	ctx->type = RDMA_RW_MULTI_WR;
	return ctx->nr_ops;

out_unmap:
	for (i = 0; i < mapped_bvecs; i++)
		ib_dma_unmap_bvec(dev, ctx->map.sges[i].addr,
				  ctx->map.sges[i].length, dir);
	kfree(ctx->map.wrs);
out_free_sges:
	kfree(ctx->map.sges);
	return -ENOMEM;
}

/*
 * Try to use the two-step IOVA API to map bvecs into a contiguous DMA range.
 * This reduces IOTLB sync overhead by doing one sync at the end instead of
 * one per bvec, and produces a contiguous DMA address range.
 *
 * Returns the number of WQEs on success, -EOPNOTSUPP if IOVA mapping is not
 * available, or another negative error code on failure.
 */
static int rdma_rw_init_iova_wrs_bvec(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		const struct bio_vec *bvec, u32 nr_bvec, u32 offset,
		u64 remote_addr, u32 rkey, enum dma_data_direction dir)
{
	struct ib_device *dev = qp->pd->device;
	struct device *dma_dev = dev->dma_device;
	u32 max_sge = dir == DMA_TO_DEVICE ? qp->max_write_sge :
		      qp->max_read_sge;
	struct ib_sge *sge;
	size_t total_len = 0, mapped_len = 0;
	u32 i, j, bvec_idx = 0;
	int ret;

	/* Virtual DMA devices don't support IOVA mapping */
	if (ib_uses_virt_dma(dev))
		return -EOPNOTSUPP;

	if (!max_sge)
		return -EINVAL;

	/* Calculate total transfer length */
	for (i = 0; i < nr_bvec; i++) {
		size_t len = (i == 0 && offset) ?
			     bvec[i].bv_len - offset : bvec[i].bv_len;

		if (check_add_overflow(total_len, len, &total_len))
			return -EINVAL;
	}

	/* Try to allocate contiguous IOVA space */
	if (!dma_iova_try_alloc(dma_dev, &ctx->iova.state,
				bvec_phys(&bvec[0]) + offset, total_len))
		return -EOPNOTSUPP;

	ctx->nr_ops = DIV_ROUND_UP(nr_bvec, max_sge);

	ctx->iova.sges = sge = kcalloc(nr_bvec, sizeof(*sge), GFP_KERNEL);
	if (!ctx->iova.sges) {
		ret = -ENOMEM;
		goto out_free_iova;
	}

	ctx->iova.wrs = kcalloc(ctx->nr_ops, sizeof(*ctx->iova.wrs), GFP_KERNEL);
	if (!ctx->iova.wrs) {
		ret = -ENOMEM;
		goto out_free_sges;
	}

	/* Link all bvecs into the IOVA space */
	for (i = 0; i < nr_bvec; i++) {
		const struct bio_vec *bv = &bvec[i];
		phys_addr_t phys = bvec_phys(bv);
		size_t len = bv->bv_len;

		if (i == 0 && offset) {
			phys += offset;
			len -= offset;
		}

		ret = dma_iova_link(dma_dev, &ctx->iova.state, phys,
				    mapped_len, len, dir, 0);
		if (ret)
			goto out_destroy;

		mapped_len += len;
	}

	/* Sync the IOTLB once for all linked pages */
	ret = dma_iova_sync(dma_dev, &ctx->iova.state, 0, mapped_len);
	if (ret)
		goto out_destroy;

	ctx->iova.mapped_len = mapped_len;

	/* Build SGEs using offsets into the contiguous IOVA range */
	mapped_len = 0;
	for (i = 0; i < ctx->nr_ops; i++) {
		struct ib_rdma_wr *rdma_wr = &ctx->iova.wrs[i];
		u32 nr_sge = min(nr_bvec - bvec_idx, max_sge);

		if (dir == DMA_TO_DEVICE)
			rdma_wr->wr.opcode = IB_WR_RDMA_WRITE;
		else
			rdma_wr->wr.opcode = IB_WR_RDMA_READ;
		rdma_wr->remote_addr = remote_addr + mapped_len;
		rdma_wr->rkey = rkey;
		rdma_wr->wr.num_sge = nr_sge;
		rdma_wr->wr.sg_list = sge;

		for (j = 0; j < nr_sge; j++, bvec_idx++) {
			const struct bio_vec *bv = &bvec[bvec_idx];
			size_t len = bv->bv_len;

			if (bvec_idx == 0 && offset)
				len -= offset;

			sge->addr = ctx->iova.state.addr + mapped_len;
			sge->length = len;
			sge->lkey = qp->pd->local_dma_lkey;

			mapped_len += len;
			sge++;
		}

		rdma_wr->wr.next = i + 1 < ctx->nr_ops ?
			&ctx->iova.wrs[i + 1].wr : NULL;
	}

	ctx->type = RDMA_RW_IOVA;
	return ctx->nr_ops;

out_destroy:
	dma_iova_destroy(dma_dev, &ctx->iova.state, mapped_len, dir, 0);
	kfree(ctx->iova.wrs);
	kfree(ctx->iova.sges);
	return ret;
out_free_sges:
	kfree(ctx->iova.sges);
out_free_iova:
	dma_iova_free(dma_dev, &ctx->iova.state);
	return ret;
}

/**
 * rdma_rw_ctx_init - initialize a RDMA READ/WRITE context
 * @ctx:	context to initialize
 * @qp:		queue pair to operate on
 * @port_num:	port num to which the connection is bound
 * @sg:		scatterlist to READ/WRITE from/to
 * @sg_cnt:	number of entries in @sg
 * @sg_offset:	current byte offset into @sg
 * @remote_addr:remote address to read/write (relative to @rkey)
 * @rkey:	remote key to operate on
 * @dir:	%DMA_TO_DEVICE for RDMA WRITE, %DMA_FROM_DEVICE for RDMA READ
 *
 * Returns the number of WQEs that will be needed on the workqueue if
 * successful, or a negative error code.
 */
int rdma_rw_ctx_init(struct rdma_rw_ctx *ctx, struct ib_qp *qp, u32 port_num,
		struct scatterlist *sg, u32 sg_cnt, u32 sg_offset,
		u64 remote_addr, u32 rkey, enum dma_data_direction dir)
{
	struct ib_device *dev = qp->pd->device;
	struct sg_table sgt = {
		.sgl = sg,
		.orig_nents = sg_cnt,
	};
	int ret;

	ret = ib_dma_map_sgtable_attrs(dev, &sgt, dir, 0);
	if (ret)
		return ret;
	sg_cnt = sgt.nents;

	/*
	 * Skip to the S/G entry that sg_offset falls into:
	 */
	for (;;) {
		u32 len = sg_dma_len(sg);

		if (sg_offset < len)
			break;

		sg = sg_next(sg);
		sg_offset -= len;
		sg_cnt--;
	}

	ret = -EIO;
	if (WARN_ON_ONCE(sg_cnt == 0))
		goto out_unmap_sg;

	if (rdma_rw_io_needs_mr(qp->device, port_num, dir, sg_cnt)) {
		ret = rdma_rw_init_mr_wrs(ctx, qp, port_num, sg, sg_cnt,
				sg_offset, remote_addr, rkey, dir);
	} else if (sg_cnt > 1) {
		ret = rdma_rw_init_map_wrs(ctx, qp, sg, sg_cnt, sg_offset,
				remote_addr, rkey, dir);
	} else {
		ret = rdma_rw_init_single_wr(ctx, qp, sg, sg_offset,
				remote_addr, rkey, dir);
	}

	if (ret < 0)
		goto out_unmap_sg;
	return ret;

out_unmap_sg:
	ib_dma_unmap_sgtable_attrs(dev, &sgt, dir, 0);
	return ret;
}
EXPORT_SYMBOL(rdma_rw_ctx_init);

/**
 * rdma_rw_ctx_init_bvec - initialize a RDMA READ/WRITE context from bio_vec
 * @ctx:	context to initialize
 * @qp:		queue pair to operate on
 * @port_num:	port num to which the connection is bound
 * @bvec:	bio_vec array to READ/WRITE from/to
 * @nr_bvec:	number of entries in @bvec
 * @offset:	byte offset into first bvec
 * @remote_addr:remote address to read/write (relative to @rkey)
 * @rkey:	remote key to operate on
 * @dir:	%DMA_TO_DEVICE for RDMA WRITE, %DMA_FROM_DEVICE for RDMA READ
 *
 * Maps the bio_vec array directly, avoiding intermediate scatterlist
 * conversion. Supports MR registration for iWARP devices and force_mr mode.
 *
 * Returns the number of WQEs that will be needed on the workqueue if
 * successful, or a negative error code.
 */
int rdma_rw_ctx_init_bvec(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		u32 port_num, const struct bio_vec *bvec, u32 nr_bvec,
		u32 offset, u64 remote_addr, u32 rkey,
		enum dma_data_direction dir)
{
	struct ib_device *dev = qp->pd->device;
	int ret;

	if (nr_bvec == 0 || offset > bvec[0].bv_len)
		return -EINVAL;

	if (rdma_rw_io_needs_mr(dev, port_num, dir, nr_bvec))
		return rdma_rw_init_mr_wrs_bvec(ctx, qp, port_num, bvec,
						nr_bvec, offset, remote_addr,
						rkey, dir);

	if (nr_bvec == 1)
		return rdma_rw_init_single_wr_bvec(ctx, qp, bvec, offset,
				remote_addr, rkey, dir);

	/*
	 * Try IOVA-based mapping first for multi-bvec transfers.
	 * This reduces IOTLB sync overhead by batching all mappings.
	 */
	ret = rdma_rw_init_iova_wrs_bvec(ctx, qp, bvec, nr_bvec, offset,
			remote_addr, rkey, dir);
	if (ret != -EOPNOTSUPP)
		return ret;

	return rdma_rw_init_map_wrs_bvec(ctx, qp, bvec, nr_bvec, offset,
			remote_addr, rkey, dir);
}
EXPORT_SYMBOL(rdma_rw_ctx_init_bvec);

/**
 * rdma_rw_ctx_signature_init - initialize a RW context with signature offload
 * @ctx:	context to initialize
 * @qp:		queue pair to operate on
 * @port_num:	port num to which the connection is bound
 * @sg:		scatterlist to READ/WRITE from/to
 * @sg_cnt:	number of entries in @sg
 * @prot_sg:	scatterlist to READ/WRITE protection information from/to
 * @prot_sg_cnt: number of entries in @prot_sg
 * @sig_attrs:	signature offloading algorithms
 * @remote_addr:remote address to read/write (relative to @rkey)
 * @rkey:	remote key to operate on
 * @dir:	%DMA_TO_DEVICE for RDMA WRITE, %DMA_FROM_DEVICE for RDMA READ
 *
 * Returns the number of WQEs that will be needed on the workqueue if
 * successful, or a negative error code.
 */
int rdma_rw_ctx_signature_init(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		u32 port_num, struct scatterlist *sg, u32 sg_cnt,
		struct scatterlist *prot_sg, u32 prot_sg_cnt,
		struct ib_sig_attrs *sig_attrs,
		u64 remote_addr, u32 rkey, enum dma_data_direction dir)
{
	struct ib_device *dev = qp->pd->device;
	u32 pages_per_mr = rdma_rw_fr_page_list_len(qp->pd->device,
						    qp->integrity_en);
	struct sg_table sgt = {
		.sgl = sg,
		.orig_nents = sg_cnt,
	};
	struct sg_table prot_sgt = {
		.sgl = prot_sg,
		.orig_nents = prot_sg_cnt,
	};
	struct ib_rdma_wr *rdma_wr;
	int count = 0, ret;

	if (sg_cnt > pages_per_mr || prot_sg_cnt > pages_per_mr) {
		pr_err("SG count too large: sg_cnt=%u, prot_sg_cnt=%u, pages_per_mr=%u\n",
		       sg_cnt, prot_sg_cnt, pages_per_mr);
		return -EINVAL;
	}

	ret = ib_dma_map_sgtable_attrs(dev, &sgt, dir, 0);
	if (ret)
		return ret;

	if (prot_sg_cnt) {
		ret = ib_dma_map_sgtable_attrs(dev, &prot_sgt, dir, 0);
		if (ret)
			goto out_unmap_sg;
	}

	ctx->type = RDMA_RW_SIG_MR;
	ctx->nr_ops = 1;
	ctx->reg = kzalloc(sizeof(*ctx->reg), GFP_KERNEL);
	if (!ctx->reg) {
		ret = -ENOMEM;
		goto out_unmap_prot_sg;
	}

	ctx->reg->mr = ib_mr_pool_get(qp, &qp->sig_mrs);
	if (!ctx->reg->mr) {
		ret = -EAGAIN;
		goto out_free_ctx;
	}

	count += rdma_rw_inv_key(ctx->reg);

	memcpy(ctx->reg->mr->sig_attrs, sig_attrs, sizeof(struct ib_sig_attrs));

	ret = ib_map_mr_sg_pi(ctx->reg->mr, sg, sgt.nents, NULL, prot_sg,
			      prot_sgt.nents, NULL, SZ_4K);
	if (unlikely(ret)) {
		pr_err("failed to map PI sg (%u)\n",
		       sgt.nents + prot_sgt.nents);
		goto out_destroy_sig_mr;
	}

	ctx->reg->reg_wr.wr.opcode = IB_WR_REG_MR_INTEGRITY;
	ctx->reg->reg_wr.wr.wr_cqe = NULL;
	ctx->reg->reg_wr.wr.num_sge = 0;
	ctx->reg->reg_wr.wr.send_flags = 0;
	ctx->reg->reg_wr.access = IB_ACCESS_LOCAL_WRITE;
	if (rdma_protocol_iwarp(qp->device, port_num))
		ctx->reg->reg_wr.access |= IB_ACCESS_REMOTE_WRITE;
	ctx->reg->reg_wr.mr = ctx->reg->mr;
	ctx->reg->reg_wr.key = ctx->reg->mr->lkey;
	count++;

	ctx->reg->sge.addr = ctx->reg->mr->iova;
	ctx->reg->sge.length = ctx->reg->mr->length;
	if (sig_attrs->wire.sig_type == IB_SIG_TYPE_NONE)
		ctx->reg->sge.length -= ctx->reg->mr->sig_attrs->meta_length;

	rdma_wr = &ctx->reg->wr;
	rdma_wr->wr.sg_list = &ctx->reg->sge;
	rdma_wr->wr.num_sge = 1;
	rdma_wr->remote_addr = remote_addr;
	rdma_wr->rkey = rkey;
	if (dir == DMA_TO_DEVICE)
		rdma_wr->wr.opcode = IB_WR_RDMA_WRITE;
	else
		rdma_wr->wr.opcode = IB_WR_RDMA_READ;
	ctx->reg->reg_wr.wr.next = &rdma_wr->wr;
	count++;

	return count;

out_destroy_sig_mr:
	ib_mr_pool_put(qp, &qp->sig_mrs, ctx->reg->mr);
out_free_ctx:
	kfree(ctx->reg);
out_unmap_prot_sg:
	if (prot_sgt.nents)
		ib_dma_unmap_sgtable_attrs(dev, &prot_sgt, dir, 0);
out_unmap_sg:
	ib_dma_unmap_sgtable_attrs(dev, &sgt, dir, 0);
	return ret;
}
EXPORT_SYMBOL(rdma_rw_ctx_signature_init);

/*
 * Now that we are going to post the WRs we can update the lkey and need_inval
 * state on the MRs.  If we were doing this at init time, we would get double
 * or missing invalidations if a context was initialized but not actually
 * posted.
 */
static void rdma_rw_update_lkey(struct rdma_rw_reg_ctx *reg, bool need_inval)
{
	reg->mr->need_inval = need_inval;
	ib_update_fast_reg_key(reg->mr, ib_inc_rkey(reg->mr->lkey));
	reg->reg_wr.key = reg->mr->lkey;
	reg->sge.lkey = reg->mr->lkey;
}

/**
 * rdma_rw_ctx_wrs - return chain of WRs for a RDMA READ or WRITE operation
 * @ctx:	context to operate on
 * @qp:		queue pair to operate on
 * @port_num:	port num to which the connection is bound
 * @cqe:	completion queue entry for the last WR
 * @chain_wr:	WR to append to the posted chain
 *
 * Return the WR chain for the set of RDMA READ/WRITE operations described by
 * @ctx, as well as any memory registration operations needed.  If @chain_wr
 * is non-NULL the WR it points to will be appended to the chain of WRs posted.
 * If @chain_wr is not set @cqe must be set so that the caller gets a
 * completion notification.
 */
struct ib_send_wr *rdma_rw_ctx_wrs(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		u32 port_num, struct ib_cqe *cqe, struct ib_send_wr *chain_wr)
{
	struct ib_send_wr *first_wr, *last_wr;
	int i;

	switch (ctx->type) {
	case RDMA_RW_SIG_MR:
	case RDMA_RW_MR:
		for (i = 0; i < ctx->nr_ops; i++) {
			rdma_rw_update_lkey(&ctx->reg[i],
				ctx->reg[i].wr.wr.opcode !=
					IB_WR_RDMA_READ_WITH_INV);
		}

		if (ctx->reg[0].inv_wr.next)
			first_wr = &ctx->reg[0].inv_wr;
		else
			first_wr = &ctx->reg[0].reg_wr.wr;
		last_wr = &ctx->reg[ctx->nr_ops - 1].wr.wr;
		break;
	case RDMA_RW_IOVA:
		first_wr = &ctx->iova.wrs[0].wr;
		last_wr = &ctx->iova.wrs[ctx->nr_ops - 1].wr;
		break;
	case RDMA_RW_MULTI_WR:
		first_wr = &ctx->map.wrs[0].wr;
		last_wr = &ctx->map.wrs[ctx->nr_ops - 1].wr;
		break;
	case RDMA_RW_SINGLE_WR:
		first_wr = &ctx->single.wr.wr;
		last_wr = &ctx->single.wr.wr;
		break;
	default:
		BUG();
	}

	if (chain_wr) {
		last_wr->next = chain_wr;
	} else {
		last_wr->wr_cqe = cqe;
		last_wr->send_flags |= IB_SEND_SIGNALED;
	}

	return first_wr;
}
EXPORT_SYMBOL(rdma_rw_ctx_wrs);

/**
 * rdma_rw_ctx_post - post a RDMA READ or RDMA WRITE operation
 * @ctx:	context to operate on
 * @qp:		queue pair to operate on
 * @port_num:	port num to which the connection is bound
 * @cqe:	completion queue entry for the last WR
 * @chain_wr:	WR to append to the posted chain
 *
 * Post the set of RDMA READ/WRITE operations described by @ctx, as well as
 * any memory registration operations needed.  If @chain_wr is non-NULL the
 * WR it points to will be appended to the chain of WRs posted.  If @chain_wr
 * is not set @cqe must be set so that the caller gets a completion
 * notification.
 */
int rdma_rw_ctx_post(struct rdma_rw_ctx *ctx, struct ib_qp *qp, u32 port_num,
		struct ib_cqe *cqe, struct ib_send_wr *chain_wr)
{
	struct ib_send_wr *first_wr;

	first_wr = rdma_rw_ctx_wrs(ctx, qp, port_num, cqe, chain_wr);
	return ib_post_send(qp, first_wr, NULL);
}
EXPORT_SYMBOL(rdma_rw_ctx_post);

/**
 * rdma_rw_ctx_destroy - release all resources allocated by rdma_rw_ctx_init
 * @ctx:	context to release
 * @qp:		queue pair to operate on
 * @port_num:	port num to which the connection is bound
 * @sg:		scatterlist that was used for the READ/WRITE
 * @sg_cnt:	number of entries in @sg
 * @dir:	%DMA_TO_DEVICE for RDMA WRITE, %DMA_FROM_DEVICE for RDMA READ
 */
void rdma_rw_ctx_destroy(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
			 u32 port_num, struct scatterlist *sg, u32 sg_cnt,
			 enum dma_data_direction dir)
{
	int i;

	switch (ctx->type) {
	case RDMA_RW_MR:
		for (i = 0; i < ctx->nr_ops; i++)
			ib_mr_pool_put(qp, &qp->rdma_mrs, ctx->reg[i].mr);
		kfree(ctx->reg);
		break;
	case RDMA_RW_MULTI_WR:
		kfree(ctx->map.wrs);
		kfree(ctx->map.sges);
		break;
	case RDMA_RW_SINGLE_WR:
		break;
	default:
		BUG();
		break;
	}

	ib_dma_unmap_sg(qp->pd->device, sg, sg_cnt, dir);
}
EXPORT_SYMBOL(rdma_rw_ctx_destroy);

/**
 * rdma_rw_ctx_destroy_bvec - release resources from rdma_rw_ctx_init_bvec
 * @ctx:	context to release
 * @qp:		queue pair to operate on
 * @port_num:	port num to which the connection is bound
 * @bvec:	bio_vec array that was used for the READ/WRITE
 * @nr_bvec:	number of entries in @bvec
 * @dir:	%DMA_TO_DEVICE for RDMA WRITE, %DMA_FROM_DEVICE for RDMA READ
 */
void rdma_rw_ctx_destroy_bvec(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		u32 __maybe_unused port_num,
		const struct bio_vec __maybe_unused *bvec,
		u32 nr_bvec, enum dma_data_direction dir)
{
	struct ib_device *dev = qp->pd->device;
	u32 i;

	switch (ctx->type) {
	case RDMA_RW_MR:
		for (i = 0; i < ctx->nr_ops; i++)
			ib_mr_pool_put(qp, &qp->rdma_mrs, ctx->reg[i].mr);
		kfree(ctx->reg);
		/* Unmap bvecs using stored DMA addresses */
		for (i = 0; i < ctx->mr_sg_cnt; i++)
			ib_dma_unmap_bvec(dev, sg_dma_address(&ctx->mr_sgl[i]),
					  sg_dma_len(&ctx->mr_sgl[i]), dir);
		kfree(ctx->mr_sgl);
		break;
	case RDMA_RW_IOVA:
		dma_iova_destroy(dev->dma_device, &ctx->iova.state,
				 ctx->iova.mapped_len, dir, 0);
		kfree(ctx->iova.wrs);
		kfree(ctx->iova.sges);
		break;
	case RDMA_RW_MULTI_WR:
		for (i = 0; i < nr_bvec; i++)
			ib_dma_unmap_bvec(dev, ctx->map.sges[i].addr,
					  ctx->map.sges[i].length, dir);
		kfree(ctx->map.wrs);
		kfree(ctx->map.sges);
		break;
	case RDMA_RW_SINGLE_WR:
		ib_dma_unmap_bvec(dev, ctx->single.sge.addr,
				  ctx->single.sge.length, dir);
		break;
	default:
		WARN_ON_ONCE(1);
		return;
	}
}
EXPORT_SYMBOL(rdma_rw_ctx_destroy_bvec);

/**
 * rdma_rw_ctx_destroy_signature - release all resources allocated by
 *	rdma_rw_ctx_signature_init
 * @ctx:	context to release
 * @qp:		queue pair to operate on
 * @port_num:	port num to which the connection is bound
 * @sg:		scatterlist that was used for the READ/WRITE
 * @sg_cnt:	number of entries in @sg
 * @prot_sg:	scatterlist that was used for the READ/WRITE of the PI
 * @prot_sg_cnt: number of entries in @prot_sg
 * @dir:	%DMA_TO_DEVICE for RDMA WRITE, %DMA_FROM_DEVICE for RDMA READ
 */
void rdma_rw_ctx_destroy_signature(struct rdma_rw_ctx *ctx, struct ib_qp *qp,
		u32 port_num, struct scatterlist *sg, u32 sg_cnt,
		struct scatterlist *prot_sg, u32 prot_sg_cnt,
		enum dma_data_direction dir)
{
	if (WARN_ON_ONCE(ctx->type != RDMA_RW_SIG_MR))
		return;

	ib_mr_pool_put(qp, &qp->sig_mrs, ctx->reg->mr);
	kfree(ctx->reg);

	if (prot_sg_cnt)
		ib_dma_unmap_sg(qp->pd->device, prot_sg, prot_sg_cnt, dir);
	ib_dma_unmap_sg(qp->pd->device, sg, sg_cnt, dir);
}
EXPORT_SYMBOL(rdma_rw_ctx_destroy_signature);

/**
 * rdma_rw_mr_factor - return number of MRs required for a payload
 * @device:	device handling the connection
 * @port_num:	port num to which the connection is bound
 * @maxpages:	maximum payload pages per rdma_rw_ctx
 *
 * Returns the number of MRs the device requires to move @maxpayload
 * bytes. The returned value is used during transport creation to
 * compute max_rdma_ctxts and the size of the transport's Send and
 * Send Completion Queues.
 */
unsigned int rdma_rw_mr_factor(struct ib_device *device, u32 port_num,
			       unsigned int maxpages)
{
	unsigned int mr_pages;

	if (rdma_rw_can_use_mr(device, port_num))
		mr_pages = rdma_rw_fr_page_list_len(device, false);
	else
		mr_pages = device->attrs.max_sge_rd;
	return DIV_ROUND_UP(maxpages, mr_pages);
}
EXPORT_SYMBOL(rdma_rw_mr_factor);

void rdma_rw_init_qp(struct ib_device *dev, struct ib_qp_init_attr *attr)
{
	u32 factor;

	WARN_ON_ONCE(attr->port_num == 0);

	/*
	 * Each context needs at least one RDMA READ or WRITE WR.
	 *
	 * For some hardware we might need more, eventually we should ask the
	 * HCA driver for a multiplier here.
	 */
	factor = 1;

	/*
	 * If the device needs MRs to perform RDMA READ or WRITE operations,
	 * we'll need two additional MRs for the registrations and the
	 * invalidation.
	 */
	if (attr->create_flags & IB_QP_CREATE_INTEGRITY_EN ||
	    rdma_rw_can_use_mr(dev, attr->port_num))
		factor += 2;	/* inv + reg */

	attr->cap.max_send_wr += factor * attr->cap.max_rdma_ctxs;

	/*
	 * But maybe we were just too high in the sky and the device doesn't
	 * even support all we need, and we'll have to live with what we get..
	 */
	attr->cap.max_send_wr =
		min_t(u32, attr->cap.max_send_wr, dev->attrs.max_qp_wr);
}

int rdma_rw_init_mrs(struct ib_qp *qp, struct ib_qp_init_attr *attr)
{
	struct ib_device *dev = qp->pd->device;
	u32 nr_mrs = 0, nr_sig_mrs = 0, max_num_sg = 0;
	int ret = 0;

	if (attr->create_flags & IB_QP_CREATE_INTEGRITY_EN) {
		nr_sig_mrs = attr->cap.max_rdma_ctxs;
		nr_mrs = attr->cap.max_rdma_ctxs;
		max_num_sg = rdma_rw_fr_page_list_len(dev, true);
	} else if (rdma_rw_can_use_mr(dev, attr->port_num)) {
		nr_mrs = attr->cap.max_rdma_ctxs;
		max_num_sg = rdma_rw_fr_page_list_len(dev, false);
	}

	if (nr_mrs) {
		ret = ib_mr_pool_init(qp, &qp->rdma_mrs, nr_mrs,
				IB_MR_TYPE_MEM_REG,
				max_num_sg, 0);
		if (ret) {
			pr_err("%s: failed to allocated %u MRs\n",
				__func__, nr_mrs);
			return ret;
		}
	}

	if (nr_sig_mrs) {
		ret = ib_mr_pool_init(qp, &qp->sig_mrs, nr_sig_mrs,
				IB_MR_TYPE_INTEGRITY, max_num_sg, max_num_sg);
		if (ret) {
			pr_err("%s: failed to allocated %u SIG MRs\n",
				__func__, nr_sig_mrs);
			goto out_free_rdma_mrs;
		}
	}

	return 0;

out_free_rdma_mrs:
	ib_mr_pool_destroy(qp, &qp->rdma_mrs);
	return ret;
}

void rdma_rw_cleanup_mrs(struct ib_qp *qp)
{
	ib_mr_pool_destroy(qp, &qp->sig_mrs);
	ib_mr_pool_destroy(qp, &qp->rdma_mrs);
}
