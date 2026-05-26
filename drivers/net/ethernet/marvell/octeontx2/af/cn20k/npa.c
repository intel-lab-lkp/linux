// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU Admin Function driver
 *
 * Copyright (C) 2024 Marvell.
 *
 */

#include <linux/module.h>
#include <linux/pci.h>

#include "cn20k/api.h"
#include "cn20k/reg.h"
#include "struct.h"
#include "../rvu.h"

int rvu_mbox_handler_npa_cn20k_aq_enq(struct rvu *rvu,
				      struct npa_cn20k_aq_enq_req *req,
				      struct npa_cn20k_aq_enq_rsp *rsp)
{
	return rvu_npa_aq_enq_inst(rvu, (struct npa_aq_enq_req *)req,
				   (struct npa_aq_enq_rsp *)rsp);
}
EXPORT_SYMBOL(rvu_mbox_handler_npa_cn20k_aq_enq);

static int npa_cn20k_dpc_alloc(struct rvu *rvu,
			       struct npa_cn20k_dpc_alloc_req *req,
			       struct npa_cn20k_dpc_alloc_rsp *rsp)
{
	struct rvu_hwinfo *hw = rvu->hw;
	u16 pcifunc = req->hdr.pcifunc;
	int cntr, lf, blkaddr, ridx;
	struct rvu_block *block;
	struct rvu_pfvf *pfvf;
	u64 val, lfmask;

	pfvf = rvu_get_pfvf(rvu, pcifunc);

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPA, 0);
	if (!pfvf->npalf || blkaddr < 0)
		return NPA_AF_ERR_AF_LF_INVALID;

	mutex_lock(&rvu->rsrc_lock);

	block = &hw->block[blkaddr];
	lf = rvu_get_lf(rvu, block, pcifunc, 0);
	if (lf < 0) {
		mutex_unlock(&rvu->rsrc_lock);
		return NPA_AF_ERR_AF_LF_INVALID;
	}

	/* allocate a new counter */
	cntr = rvu_alloc_rsrc(&rvu->npa_dpc);
	if (cntr < 0) {
		mutex_unlock(&rvu->rsrc_lock);
		return cntr;
	}

	rsp->cntr_id = cntr;

	/* Consumers can configure to count ALLOC/FREE requests only */
	rvu_write64(rvu, blkaddr, NPA_AF_DPCX_CFG(cntr), req->dpc_conf);

	/* 0 to 63 lfs -> idx 0, 64 - 127 lfs -> idx 1 */
	ridx = lf >> 6;
	lfmask = BIT_ULL(ridx ? lf - NPA_DPC_LFS_PER_REG : lf);

	ridx = 2 * cntr + ridx;
	/* Give permission for LF access */
	val = rvu_read64(rvu, blkaddr, NPA_AF_DPC_PERMITX(ridx));
	val |= lfmask;
	rvu_write64(rvu, blkaddr, NPA_AF_DPC_PERMITX(ridx), val);

	mutex_unlock(&rvu->rsrc_lock);

	return 0;
}

int rvu_mbox_handler_npa_cn20k_dpc_alloc(struct rvu *rvu,
					 struct npa_cn20k_dpc_alloc_req *req,
					 struct npa_cn20k_dpc_alloc_rsp *rsp)
{
	if (!is_cn20k(rvu->pdev))
		return -EOPNOTSUPP;

	return npa_cn20k_dpc_alloc(rvu, req, rsp);
}

static int npa_cn20k_dpc_free(struct rvu *rvu,
			      struct npa_cn20k_dpc_free_req *req)
{
	struct rvu_hwinfo *hw = rvu->hw;
	u16 pcifunc = req->hdr.pcifunc;
	int cntr, lf, blkaddr, ridx;
	struct rvu_block *block;
	struct rvu_pfvf *pfvf;
	u64 val, lfmask;

	pfvf = rvu_get_pfvf(rvu, pcifunc);

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPA, 0);
	if (!pfvf->npalf || blkaddr < 0)
		return NPA_AF_ERR_AF_LF_INVALID;

	block = &hw->block[blkaddr];
	lf = rvu_get_lf(rvu, block, pcifunc, 0);
	if (lf < 0)
		return NPA_AF_ERR_AF_LF_INVALID;

	if (req->cntr_id >= NPA_DPC_MAX)
		return NPA_AF_ERR_PARAM;

	mutex_lock(&rvu->rsrc_lock);

	/* 0 to 63 lfs -> idx 0, 64 - 127 lfs -> idx 1 */
	ridx = lf >> 6;
	lfmask = BIT_ULL(ridx ? lf - NPA_DPC_LFS_PER_REG : lf);
	cntr = req->cntr_id;

	ridx = 2 * cntr + ridx;

	val = rvu_read64(rvu, blkaddr, NPA_AF_DPC_PERMITX(ridx));
	/* Check if the counter is allotted to this LF */
	if (!(val & lfmask)) {
		mutex_unlock(&rvu->rsrc_lock);
		return 0;
	}

	/* Revert permission */
	val &= ~lfmask;
	rvu_write64(rvu, blkaddr, NPA_AF_DPC_PERMITX(ridx), val);

	/* Free this counter */
	rvu_free_rsrc(&rvu->npa_dpc, req->cntr_id);

	mutex_unlock(&rvu->rsrc_lock);

	return 0;
}

void npa_cn20k_dpc_free_all(struct rvu *rvu, u16 pcifunc)
{
	struct npa_cn20k_dpc_free_req req;
	int i;

	req.hdr.pcifunc = pcifunc;
	for (i = 0; i < NPA_DPC_MAX; i++) {
		req.cntr_id = i;
		npa_cn20k_dpc_free(rvu, &req);
	}
}

int rvu_mbox_handler_npa_cn20k_dpc_free(struct rvu *rvu,
					struct npa_cn20k_dpc_free_req *req,
					struct msg_rsp *rsp)
{
	if (!is_cn20k(rvu->pdev))
		return -EOPNOTSUPP;

	return npa_cn20k_dpc_free(rvu, req);
}
