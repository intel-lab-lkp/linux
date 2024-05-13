// SPDX-License-Identifier: GPL-2.0
/* Marvell IPSEC offload driver
 *
 * Copyright (C) 2024 Marvell.
 */

#include <net/xfrm.h>
#include <linux/netdevice.h>
#include <linux/bitfield.h>

#include "otx2_common.h"
#include "cn10k_ipsec.h"

static bool is_dev_support_inline_ipsec(struct pci_dev *pdev)
{
	return is_dev_cn10ka_b0(pdev) || is_dev_cn10kb(pdev);
}

static int cn10k_outb_cptlf_attach(struct otx2_nic *pf)
{
	struct rsrc_attach *attach;
	int err;

	mutex_lock(&pf->mbox.lock);
	/* Get memory to put this msg */
	attach = otx2_mbox_alloc_msg_attach_resources(&pf->mbox);
	if (!attach) {
		mutex_unlock(&pf->mbox.lock);
		return -ENOMEM;
	}

	attach->cptlfs = true;
	attach->modify = true;

	/* Send attach request to AF */
	err = otx2_sync_mbox_msg(&pf->mbox);
	if (err) {
		mutex_unlock(&pf->mbox.lock);
		return err;
	}

	mutex_unlock(&pf->mbox.lock);
	return 0;
}

static int cn10k_outb_cptlf_detach(struct otx2_nic *pf)
{
	struct rsrc_detach *detach;

	mutex_lock(&pf->mbox.lock);
	detach = otx2_mbox_alloc_msg_detach_resources(&pf->mbox);
	if (!detach) {
		mutex_unlock(&pf->mbox.lock);
		return -ENOMEM;
	}

	detach->partial = true;
	detach->cptlfs = true;

	/* Send detach request to AF */
	otx2_sync_mbox_msg(&pf->mbox);
	mutex_unlock(&pf->mbox.lock);
	return 0;
}

static int cn10k_outb_cptlf_alloc(struct otx2_nic *pf)
{
	struct cpt_lf_alloc_req_msg *req;
	int ret = 0;

	mutex_lock(&pf->mbox.lock);
	req = otx2_mbox_alloc_msg_cpt_lf_alloc(&pf->mbox);
	if (!req) {
		ret = -ENOMEM;
		goto error;
	}

	/* PF function */
	req->nix_pf_func = pf->pcifunc;
	/* Enable SE-IE Engine Group */
	req->eng_grpmsk = 1 << CN10K_DEF_CPT_IPSEC_EGRP;

	ret = otx2_sync_mbox_msg(&pf->mbox);

error:
	mutex_unlock(&pf->mbox.lock);
	return ret;
}

static void cn10k_outb_cptlf_free(struct otx2_nic *pf)
{
	mutex_lock(&pf->mbox.lock);
	otx2_mbox_alloc_msg_cpt_lf_free(&pf->mbox);
	otx2_sync_mbox_msg(&pf->mbox);
	mutex_unlock(&pf->mbox.lock);
}

static int cn10k_outb_cptlf_config(struct otx2_nic *pf)
{
	struct cpt_inline_ipsec_cfg_msg *req;
	int ret = 0;

	mutex_lock(&pf->mbox.lock);
	req = otx2_mbox_alloc_msg_cpt_inline_ipsec_cfg(&pf->mbox);
	if (!req) {
		ret = -ENOMEM;
		goto error;
	}

	req->dir = CPT_INLINE_OUTBOUND;
	req->enable = 1;
	req->nix_pf_func = pf->pcifunc;
	ret = otx2_sync_mbox_msg(&pf->mbox);
error:
	mutex_unlock(&pf->mbox.lock);
	return ret;
}

static void cn10k_outb_cptlf_iq_enable(struct otx2_nic *pf)
{
	u64 reg_val;

	/* Set Execution Enable of instruction queue */
	reg_val = otx2_read64(pf, CN10K_CPT_LF_INPROG);
	reg_val |= BIT_ULL(16);
	otx2_write64(pf, CN10K_CPT_LF_INPROG, reg_val);

	/* Set iqueue's enqueuing */
	reg_val = otx2_read64(pf, CN10K_CPT_LF_CTL);
	reg_val |= BIT_ULL(0);
	otx2_write64(pf, CN10K_CPT_LF_CTL, reg_val);
}

static inline void cn10k_outb_cptlf_iq_disable(struct otx2_nic *pf)
{
	u32 inflight, grb_cnt, gwb_cnt;
	u32 nq_ptr, dq_ptr;
	int timeout = 20;
	u64 reg_val;
	int cnt;

	/* Disable instructions enqueuing */
	otx2_write64(pf, CN10K_CPT_LF_CTL, 0ull);

	/* Wait for instruction queue to become empty.
	 * CPT_LF_INPROG.INFLIGHT count is zero
	 */
	do {
		reg_val = otx2_read64(pf, CN10K_CPT_LF_INPROG);
		inflight = FIELD_GET(CPT_LF_INPROG_INFLIGHT, reg_val);
		if (!inflight)
			break;

		usleep_range(10000, 20000);
		if (timeout-- < 0) {
			netdev_err(pf->netdev, "Timeout to empty IQ\n");
			break;
		}
	} while (1);

	/* Disable executions in the LF's queue,
	 * the queue should be empty at this point
	 */
	reg_val &= ~BIT_ULL(16);
	otx2_write64(pf, CN10K_CPT_LF_INPROG, reg_val);

	/* Wait for instruction queue to become empty */
	cnt = 0;
	do {
		reg_val = otx2_read64(pf, CN10K_CPT_LF_INPROG);
		if (reg_val & BIT_ULL(31))
			cnt = 0;
		else
			cnt++;
		reg_val = otx2_read64(pf, CN10K_CPT_LF_Q_GRP_PTR);
		nq_ptr = FIELD_GET(CPT_LF_Q_GRP_PTR_DQ_PTR, reg_val);
		dq_ptr = FIELD_GET(CPT_LF_Q_GRP_PTR_DQ_PTR, reg_val);
	} while ((cnt < 10) && (nq_ptr != dq_ptr));

	cnt = 0;
	do {
		reg_val = otx2_read64(pf, CN10K_CPT_LF_INPROG);
		inflight = FIELD_GET(CPT_LF_INPROG_INFLIGHT, reg_val);
		grb_cnt = FIELD_GET(CPT_LF_INPROG_GRB_CNT, reg_val);
		gwb_cnt = FIELD_GET(CPT_LF_INPROG_GWB_CNT, reg_val);
		if (inflight == 0 && gwb_cnt < 40 &&
		    (grb_cnt == 0 || grb_cnt == 40))
			cnt++;
		else
			cnt = 0;
	} while (cnt < 10);
}

/* Allocate memory for CPT outbound Instruction queue.
 * Instruction queue memory format is:
 *      -----------------------------
 *     | Instruction Group memory    |
 *     |  (CPT_LF_Q_SIZE[SIZE_DIV40] |
 *     |   x 16 Bytes)               |
 *     |                             |
 *      ----------------------------- <-- CPT_LF_Q_BASE[ADDR]
 *     | Flow Control (128 Bytes)    |
 *     |                             |
 *      -----------------------------
 *     |  Instruction Memory         |
 *     |  (CPT_LF_Q_SIZE[SIZE_DIV40] |
 *     |   × 40 × 64 bytes)          |
 *     |                             |
 *      -----------------------------
 */
static int cn10k_outb_cptlf_iq_alloc(struct otx2_nic *pf)
{
	struct cn10k_cpt_inst_queue *iq = &pf->ipsec.iq;

	iq->size = CN10K_CPT_INST_QLEN_BYTES + CN10K_CPT_Q_FC_LEN +
		    CN10K_CPT_INST_GRP_QLEN_BYTES + OTX2_ALIGN;

	iq->real_vaddr = dma_alloc_coherent(pf->dev, iq->size,
					    &iq->real_dma_addr, GFP_KERNEL);
	if (!iq->real_vaddr)
		return -ENOMEM;

	/* iq->vaddr/dma_addr points to Flow Control location */
	iq->vaddr = iq->real_vaddr + CN10K_CPT_INST_GRP_QLEN_BYTES;
	iq->dma_addr = iq->real_dma_addr + CN10K_CPT_INST_GRP_QLEN_BYTES;

	/* Align pointers */
	iq->vaddr = PTR_ALIGN(iq->vaddr, OTX2_ALIGN);
	iq->dma_addr = PTR_ALIGN(iq->dma_addr, OTX2_ALIGN);
	return 0;
}

static void cn10k_outb_cptlf_iq_free(struct otx2_nic *pf)
{
	struct cn10k_cpt_inst_queue *iq = &pf->ipsec.iq;

	if (!iq->real_vaddr)
		dma_free_coherent(pf->dev, iq->size, iq->real_vaddr,
				  iq->real_dma_addr);

	iq->real_vaddr = NULL;
	iq->vaddr = NULL;
}

static int cn10k_outb_cptlf_iq_init(struct otx2_nic *pf)
{
	u64 reg_val;
	int ret;

	/* Allocate Memory for CPT IQ */
	ret = cn10k_outb_cptlf_iq_alloc(pf);
	if (ret)
		return ret;

	/* Disable IQ */
	cn10k_outb_cptlf_iq_disable(pf);

	/* Set IQ base address */
	otx2_write64(pf, CN10K_CPT_LF_Q_BASE, pf->ipsec.iq.dma_addr);

	/* Set IQ size */
	reg_val = FIELD_PREP(CPT_LF_Q_SIZE_DIV40, CN10K_CPT_SIZE_DIV40 +
			     CN10K_CPT_EXTRA_SIZE_DIV40);
	otx2_write64(pf, CN10K_CPT_LF_Q_SIZE, reg_val);

	return 0;
}

static int cn10k_outb_cptlf_init(struct otx2_nic *pf)
{
	int ret = 0;

	/* Initialize CPTLF Instruction Queue (IQ) */
	ret = cn10k_outb_cptlf_iq_init(pf);
	if (ret)
		return ret;

	/* Configure CPTLF for outbound inline ipsec */
	ret = cn10k_outb_cptlf_config(pf);
	if (ret)
		goto iq_clean;

	/* Enable CPTLF IQ */
	cn10k_outb_cptlf_iq_enable(pf);
	return 0;
iq_clean:
	cn10k_outb_cptlf_iq_free(pf);
	return ret;
}

static int cn10k_outb_cpt_init(struct net_device *netdev)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	int ret;

	mutex_lock(&pf->ipsec.lock);

	/* Attach a CPT LF for outbound inline ipsec */
	ret = cn10k_outb_cptlf_attach(pf);
	if (ret)
		goto unlock;

	/* Allocate a CPT LF for outbound inline ipsec */
	ret = cn10k_outb_cptlf_alloc(pf);
	if (ret)
		goto detach;

	/* Initialize the CPTLF for outbound inline ipsec */
	ret = cn10k_outb_cptlf_init(pf);
	if (ret)
		goto lf_free;

	pf->ipsec.io_addr = (__force u64)otx2_get_regaddr(pf,
						CN10K_CPT_LF_NQX(0));

	/* Set inline ipsec enabled for this device */
	pf->flags |= OTX2_FLAG_INLINE_IPSEC_ENABLED;

	goto unlock;

lf_free:
	cn10k_outb_cptlf_free(pf);
detach:
	cn10k_outb_cptlf_detach(pf);
unlock:
	mutex_unlock(&pf->ipsec.lock);
	return ret;
}

static int cn10k_outb_cpt_clean(struct otx2_nic *pf)
{
	int err;

	mutex_lock(&pf->ipsec.lock);

	/* Set inline ipsec disabled for this device */
	pf->flags &= ~OTX2_FLAG_INLINE_IPSEC_ENABLED;

	if (!bitmap_empty(pf->ipsec.sa_bitmap, CN10K_IPSEC_OUTB_MAX_SA)) {
		netdev_err(pf->netdev, "SA installed on this device\n");
		mutex_unlock(&pf->ipsec.lock);
		return -EBUSY;
	}

	/* Disable CPTLF Instruction Queue (IQ) */
	cn10k_outb_cptlf_iq_disable(pf);

	/* Set IQ base address and size to 0 */
	otx2_write64(pf, CN10K_CPT_LF_Q_BASE, 0);
	otx2_write64(pf, CN10K_CPT_LF_Q_SIZE, 0);

	/* Free CPTLF IQ */
	cn10k_outb_cptlf_iq_free(pf);

	/* Free and detach CPT LF */
	cn10k_outb_cptlf_free(pf);
	err = cn10k_outb_cptlf_detach(pf);
	if (err)
		netdev_err(pf->netdev, "Failed to detach CPT LF\n");

	mutex_unlock(&pf->ipsec.lock);
	return err;
}

static int cn10k_outb_get_sa_index(struct otx2_nic *pf,
				   struct cn10k_tx_sa_s *sa_entry)
{
	u32 sa_size = pf->ipsec.sa_size;
	u32 sa_index;

	if (!sa_entry || ((void *)sa_entry < pf->ipsec.outb_sa->base))
		return -EINVAL;

	sa_index = ((void *)sa_entry - pf->ipsec.outb_sa->base) / sa_size;
	if (sa_index >= CN10K_IPSEC_OUTB_MAX_SA)
		return -EINVAL;

	return sa_index;
}

static dma_addr_t cn10k_outb_get_sa_iova(struct otx2_nic *pf,
					 struct cn10k_tx_sa_s *sa_entry)
{
	u32 sa_index = cn10k_outb_get_sa_index(pf, sa_entry);

	if (sa_index < 0)
		return 0;
	return pf->ipsec.outb_sa->iova + sa_index * pf->ipsec.sa_size;
}

static struct cn10k_tx_sa_s *cn10k_outb_alloc_sa(struct otx2_nic *pf)
{
	u32 sa_size = pf->ipsec.sa_size;
	struct cn10k_tx_sa_s *sa_entry;
	u32 sa_index;

	sa_index = find_first_zero_bit(pf->ipsec.sa_bitmap,
				       CN10K_IPSEC_OUTB_MAX_SA);
	if (sa_index == CN10K_IPSEC_OUTB_MAX_SA)
		return NULL;

	set_bit(sa_index, pf->ipsec.sa_bitmap);

	sa_entry = pf->ipsec.outb_sa->base + sa_index * sa_size;
	return sa_entry;
}

static void cn10k_outb_free_sa(struct otx2_nic *pf,
			       struct cn10k_tx_sa_s *sa_entry)
{
	u32 sa_index = cn10k_outb_get_sa_index(pf, sa_entry);

	if (sa_index < 0)
		return;
	clear_bit(sa_index, pf->ipsec.sa_bitmap);
}

static void cn10k_cpt_inst_flush(struct otx2_nic *pf, struct cpt_inst_s *inst,
				 u64 size)
{
	struct otx2_lmt_info *lmt_info;
	u64 val = 0, tar_addr = 0;

	lmt_info = per_cpu_ptr(pf->hw.lmt_info, smp_processor_id());
	/* FIXME: val[0:10] LMT_ID.
	 * [12:15] no of LMTST - 1 in the burst.
	 * [19:63] data size of each LMTST in the burst except first.
	 */
	val = (lmt_info->lmt_id & 0x7FF);
	/* Target address for LMTST flush tells HW how many 128bit
	 * words are present.
	 * tar_addr[6:4] size of first LMTST - 1 in units of 128b.
	 */
	tar_addr |= pf->ipsec.io_addr | (((size / 16) - 1) & 0x7) << 4;
	dma_wmb();
	memcpy((u64 *)lmt_info->lmt_addr, inst, size);
	cn10k_lmt_flush(val, tar_addr);
}

static int cn10k_wait_for_cpt_respose(struct otx2_nic *pf,
				      struct cpt_res_s *res)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(10000);

	do {
		if (time_after(jiffies, timeout)) {
			netdev_err(pf->netdev, "CPT response timeout\n");
			return -EBUSY;
		}
	} while (res->compcode == CN10K_CPT_COMP_E_NOTDONE);

	if (!(res->compcode == CN10K_CPT_COMP_E_GOOD ||
	      res->compcode == CN10K_CPT_COMP_E_WARN) || res->uc_compcode) {
		netdev_err(pf->netdev, "compcode=%x doneint=%x\n",
			   res->compcode, res->doneint);
		netdev_err(pf->netdev, "uc_compcode=%x uc_info=%llx esn=%llx\n",
			   res->uc_compcode, (u64)res->uc_info, res->esn);
	}
	return 0;
}

static int cn10k_outb_write_sa(struct otx2_nic *pf, struct cn10k_tx_sa_s *sa_cptr)
{
	dma_addr_t res_iova, dptr_iova, sa_iova;
	struct cn10k_tx_sa_s *sa_dptr;
	struct cpt_inst_s inst;
	struct cpt_res_s *res;
	u32 sa_size, off;
	u64 reg_val;
	int ret;

	sa_iova = cn10k_outb_get_sa_iova(pf, sa_cptr);
	if (!sa_iova)
		return -EINVAL;

	res = dma_alloc_coherent(pf->dev, sizeof(struct cpt_res_s),
				 &res_iova, GFP_ATOMIC);
	if (!res)
		return -ENOMEM;

	sa_size = sizeof(struct cn10k_tx_sa_s);
	sa_dptr = dma_alloc_coherent(pf->dev, sa_size, &dptr_iova, GFP_ATOMIC);
	if (!sa_dptr) {
		dma_free_coherent(pf->dev, sizeof(struct cpt_res_s), res,
				  res_iova);
		return -ENOMEM;
	}

	for (off = 0; off < (sa_size / 8); off++)
		*((u64 *)sa_dptr + off) = cpu_to_be64(*((u64 *)sa_cptr + off));

	memset(&inst, 0, sizeof(struct cpt_inst_s));

	res->compcode = CN10K_CPT_COMP_E_NOTDONE;
	inst.res_addr = res_iova;
	inst.dptr = (u64)dptr_iova;
	inst.param2 = sa_size >> 3;
	inst.dlen = sa_size;
	inst.opcode_major = CN10K_IPSEC_MAJOR_OP_WRITE_SA;
	inst.opcode_minor = CN10K_IPSEC_MINOR_OP_WRITE_SA;
	inst.cptr = sa_iova;
	inst.ctx_val = 1;
	inst.egrp = CN10K_DEF_CPT_IPSEC_EGRP;

	cn10k_cpt_inst_flush(pf, &inst, sizeof(struct cpt_inst_s));
	dmb(sy);
	ret = cn10k_wait_for_cpt_respose(pf, res);
	if (ret)
		goto out;

	/* Trigger CTX flush to write dirty data back to DRAM */
	reg_val = FIELD_PREP(CPT_LF_CTX_FLUSH, sa_iova >> 7);
	otx2_write64(pf, CN10K_CPT_LF_CTX_FLUSH, reg_val);

out:
	dma_free_coherent(pf->dev, sa_size, sa_dptr, dptr_iova);
	dma_free_coherent(pf->dev, sizeof(struct cpt_res_s), res, res_iova);
	return ret;
}

static inline int cn10k_ipsec_get_hw_ctx_offset(void)
{
	/* Offset on Hardware-context offset in word */
	return (offsetof(struct cn10k_tx_sa_s, hw_ctx) / sizeof(u64)) & 0x7F;
}

static inline int cn10k_ipsec_get_ctx_push_size(void)
{
	/* Context push size is round up and in multiple of 8 Byte */
	return (roundup(offsetof(struct cn10k_tx_sa_s, hw_ctx), 8) / 8) & 0x7F;
}

static inline int cn10k_ipsec_get_aes_key_len(int key_len)
{
	if (key_len == 16)
		return CN10K_IPSEC_SA_AES_KEY_LEN_128;
	else if (key_len == 24)
		return CN10K_IPSEC_SA_AES_KEY_LEN_192;
	else
		return CN10K_IPSEC_SA_AES_KEY_LEN_256;
}

static void cn10k_outb_prepare_sa(struct xfrm_state *x,
				  struct cn10k_tx_sa_s *sa_entry)
{
	int key_len = (x->aead->alg_key_len + 7) / 8;
	struct net_device *netdev = x->xso.dev;
	u8 *key = x->aead->alg_key;
	struct otx2_nic *pf;
	u32 *tmp_salt;
	u64 *tmp_key;
	int idx;

	memset(sa_entry, 0, sizeof(struct cn10k_tx_sa_s));

	/* context size, 128 Byte aligned up */
	pf = netdev_priv(netdev);
	sa_entry->ctx_size = (pf->ipsec.sa_size / OTX2_ALIGN)  & 0xF;
	sa_entry->hw_ctx_off = cn10k_ipsec_get_hw_ctx_offset();
	sa_entry->ctx_push_size = cn10k_ipsec_get_ctx_push_size();

	/* Ucode to skip two words of CPT_CTX_HW_S */
	sa_entry->ctx_hdr_size = 1;

	/* Allow Atomic operation (AOP) */
	sa_entry->aop_valid = 1;

	/* Outbound, ESP TRANSPORT/TUNNEL Mode, AES-GCM with AES key length
	 * 128bit.
	 */
	sa_entry->sa_dir = CN10K_IPSEC_SA_DIR_OUTB;
	sa_entry->ipsec_protocol = CN10K_IPSEC_SA_IPSEC_PROTO_ESP;
	sa_entry->enc_type = CN10K_IPSEC_SA_ENCAP_TYPE_AES_GCM;
	if (x->props.mode == XFRM_MODE_TUNNEL)
		sa_entry->ipsec_mode = CN10K_IPSEC_SA_IPSEC_MODE_TUNNEL;
	else
		sa_entry->ipsec_mode = CN10K_IPSEC_SA_IPSEC_MODE_TRANSPORT;

	sa_entry->spi = cpu_to_be32(x->id.spi);

	/* Last 4 bytes are salt */
	key_len -= 4;
	sa_entry->aes_key_len = cn10k_ipsec_get_aes_key_len(key_len);
	memcpy(sa_entry->cipher_key, key, key_len);
	tmp_key = (u64 *)sa_entry->cipher_key;

	for (idx = 0; idx < key_len / 8; idx++)
		tmp_key[idx] = be64_to_cpu(tmp_key[idx]);

	memcpy(&sa_entry->iv_gcm_salt, key + key_len, 4);
	tmp_salt = (u32 *)&sa_entry->iv_gcm_salt;
	*tmp_salt = be32_to_cpu(*tmp_salt);

	/* Write SA context data to memory before enabling */
	wmb();

	/* Enable SA */
	sa_entry->sa_valid = 1;
}

static inline int cn10k_ipsec_validate_state(struct xfrm_state *x)
{
	struct net_device *netdev = x->xso.dev;

	if (x->props.aalgo != SADB_AALG_NONE) {
		netdev_err(netdev, "Cannot offload authenticated xfrm states\n");
		return -EINVAL;
	}
	if (x->props.ealgo != SADB_X_EALG_AES_GCM_ICV16) {
		netdev_err(netdev, "Only AES-GCM-ICV16 xfrm state may be offloaded\n");
		return -EINVAL;
	}
	if (x->props.calgo != SADB_X_CALG_NONE) {
		netdev_err(netdev, "Cannot offload compressed xfrm states\n");
		return -EINVAL;
	}
	if (x->props.flags & XFRM_STATE_ESN) {
		netdev_err(netdev, "Cannot offload ESN xfrm states\n");
		return -EINVAL;
	}
	if (x->props.family != AF_INET && x->props.family != AF_INET6) {
		netdev_err(netdev, "Only IPv4/v6 xfrm states may be offloaded\n");
		return -EINVAL;
	}
	if (x->props.mode != XFRM_MODE_TRANSPORT &&
	    x->props.mode != XFRM_MODE_TUNNEL) {
		dev_info(&netdev->dev, "Only tunnel/transport xfrm states may be offloaded\n");
		return -EINVAL;
	}
	if (x->id.proto != IPPROTO_ESP) {
		netdev_err(netdev, "Only ESP xfrm state may be offloaded\n");
		return -EINVAL;
	}
	if (x->encap) {
		netdev_err(netdev, "Encapsulated xfrm state may not be offloaded\n");
		return -EINVAL;
	}
	if (!x->aead) {
		netdev_err(netdev, "Cannot offload xfrm states without aead\n");
		return -EINVAL;
	}

	if (x->aead->alg_icv_len != 128) {
		netdev_err(netdev, "Cannot offload xfrm states with AEAD ICV length other than 128bit\n");
		return -EINVAL;
	}
	if (x->aead->alg_key_len != 128 + 32 &&
	    x->aead->alg_key_len != 192 + 32 &&
	    x->aead->alg_key_len != 256 + 32) {
		netdev_err(netdev, "Cannot offload xfrm states with AEAD key length other than 128/192/256bit\n");
		return -EINVAL;
	}
	if (x->tfcpad) {
		netdev_err(netdev, "Cannot offload xfrm states with tfc padding\n");
		return -EINVAL;
	}
	if (!x->geniv) {
		netdev_err(netdev, "Cannot offload xfrm states without geniv\n");
		return -EINVAL;
	}
	if (strcmp(x->geniv, "seqiv")) {
		netdev_err(netdev, "Cannot offload xfrm states with geniv other than seqiv\n");
		return -EINVAL;
	}
	return 0;
}

static int cn10k_ipsec_add_state(struct xfrm_state *x,
				 struct netlink_ext_ack *extack)
{
	struct net_device *netdev = x->xso.dev;
	struct cn10k_tx_sa_s *sa_entry;
	struct cpt_ctx_info_s *sa_info;
	struct otx2_nic *pf;
	int err;

	err = cn10k_ipsec_validate_state(x);
	if (err)
		return err;

	if (x->xso.dir == XFRM_DEV_OFFLOAD_IN) {
		netdev_err(netdev, "xfrm inbound offload not supported\n");
		err = -ENODEV;
	} else {
		pf = netdev_priv(netdev);
		if (!mutex_trylock(&pf->ipsec.lock)) {
			netdev_err(netdev, "IPSEC device is busy\n");
			return -EBUSY;
		}

		if (!(pf->flags & OTX2_FLAG_INLINE_IPSEC_ENABLED)) {
			netdev_err(netdev, "IPSEC not enabled/supported on device\n");
			err = -ENODEV;
			goto unlock;
		}

		sa_entry = cn10k_outb_alloc_sa(pf);
		if (!sa_entry) {
			netdev_err(netdev, "SA maximum limit %x reached\n",
				   CN10K_IPSEC_OUTB_MAX_SA);
			err = -EBUSY;
			goto unlock;
		}

		cn10k_outb_prepare_sa(x, sa_entry);

		err = cn10k_outb_write_sa(pf, sa_entry);
		if (err) {
			netdev_err(netdev, "Error writing outbound SA\n");
			cn10k_outb_free_sa(pf, sa_entry);
			goto unlock;
		}

		sa_info = kmalloc(sizeof(*sa_info), GFP_KERNEL);
		sa_info->sa_entry = sa_entry;
		sa_info->sa_iova = cn10k_outb_get_sa_iova(pf, sa_entry);
		x->xso.offload_handle = (unsigned long)sa_info;
	}

unlock:
	mutex_unlock(&pf->ipsec.lock);
	return err;
}

static void cn10k_ipsec_del_state(struct xfrm_state *x)
{
	struct net_device *netdev = x->xso.dev;
	struct cn10k_tx_sa_s *sa_entry;
	struct cpt_ctx_info_s *sa_info;
	struct otx2_nic *pf;
	u32 sa_index;

	if (x->xso.dir == XFRM_DEV_OFFLOAD_IN)
		return;

	pf = netdev_priv(netdev);
	if (!mutex_trylock(&pf->ipsec.lock)) {
		netdev_err(netdev, "IPSEC device is busy\n");
		return;
	}

	sa_info = (struct cpt_ctx_info_s *)x->xso.offload_handle;
	sa_entry = sa_info->sa_entry;
	sa_index = cn10k_outb_get_sa_index(pf, sa_entry);
	if (sa_index < 0 || !test_bit(sa_index, pf->ipsec.sa_bitmap)) {
		netdev_err(netdev, "Invalid SA (sa-index %d)\n", sa_index);
		goto error;
	}

	memset(sa_entry, 0, sizeof(struct cn10k_tx_sa_s));

	/* Disable SA in CPT h/w */
	sa_entry->ctx_push_size = cn10k_ipsec_get_ctx_push_size();
	sa_entry->ctx_size = (pf->ipsec.sa_size / OTX2_ALIGN)  & 0xF;
	sa_entry->aop_valid = 1;

	if (cn10k_outb_write_sa(pf, sa_entry)) {
		netdev_err(netdev, "Failed to delete sa index %d\n", sa_index);
		goto error;
	}
	x->xso.offload_handle = 0;
	clear_bit(sa_index, pf->ipsec.sa_bitmap);
	kfree(sa_info);
error:
	mutex_unlock(&pf->ipsec.lock);
}

static const struct xfrmdev_ops cn10k_ipsec_xfrmdev_ops = {
	.xdo_dev_state_add	= cn10k_ipsec_add_state,
	.xdo_dev_state_delete	= cn10k_ipsec_del_state,
};

int cn10k_ipsec_ethtool_init(struct net_device *netdev, bool enable)
{
	struct otx2_nic *pf = netdev_priv(netdev);

	/* Inline ipsec supported on cn10k */
	if (!is_dev_support_inline_ipsec(pf->pdev))
		return -ENODEV;

	if (!enable)
		return cn10k_outb_cpt_clean(pf);

	/* Initialize CPT for outbound inline ipsec */
	return cn10k_outb_cpt_init(netdev);
}

int cn10k_ipsec_init(struct net_device *netdev)
{
	struct otx2_nic *pf = netdev_priv(netdev);
	u32 sa_size;
	int err;

	if (!is_dev_support_inline_ipsec(pf->pdev))
		return 0;

	/* Each SA entry size is 128 Byte round up in size */
	sa_size = sizeof(struct cn10k_tx_sa_s) % OTX2_ALIGN ?
			 (sizeof(struct cn10k_tx_sa_s) / OTX2_ALIGN + 1) *
			 OTX2_ALIGN : sizeof(struct cn10k_tx_sa_s);
	err = qmem_alloc(pf->dev, &pf->ipsec.outb_sa, CN10K_IPSEC_OUTB_MAX_SA,
			 sa_size);
	if (err)
		return err;

	pf->ipsec.sa_size = sa_size;
	memset(pf->ipsec.outb_sa->base, 0, sa_size * CN10K_IPSEC_OUTB_MAX_SA);
	bitmap_zero(pf->ipsec.sa_bitmap, CN10K_IPSEC_OUTB_MAX_SA);

	mutex_init(&pf->ipsec.lock);
	return 0;
}

void cn10k_ipsec_clean(struct otx2_nic *pf)
{
	if (!is_dev_support_inline_ipsec(pf->pdev))
		return;

	bitmap_zero(pf->ipsec.sa_bitmap, CN10K_IPSEC_OUTB_MAX_SA);
	qmem_free(pf->dev, pf->ipsec.outb_sa);
	cn10k_outb_cpt_clean(pf);
}
