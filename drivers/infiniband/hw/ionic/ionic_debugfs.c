// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2026, Advanced Micro Devices, Inc. */

#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/module.h>

#include "ionic_ibdev.h"

static void ionic_umem_show(struct seq_file *s, const char *w,
			    struct ib_umem *umem, u8 page_size_log2)
{
	seq_printf(s, "%sumem.length:\t%#lx\n", w, umem->length);
	seq_printf(s, "%sumem.address:\t%#lx\n", w, umem->address);
	seq_printf(s, "%sumem.page_size:\t%llu\n", w, 1ULL << page_size_log2);
	seq_printf(s, "%sumem.writable:\t%d\n", w, umem->writable);
	seq_printf(s, "%sumem.nmap:\t%d\n", w, umem->sgt_append.sgt.nents);
	seq_printf(s, "%sumem.offset():\t%#x\n", w, ib_umem_offset(umem));
	seq_printf(s, "%sumem.num_pages():\t%lu\n",
		   w, ib_umem_num_pages(umem));
}

static void ionic_umem_dump(struct seq_file *s, struct ib_umem *umem,
			    u8 page_size_log2)
{
	int sg_i, page_i, page_count;
	struct scatterlist *sg;
	u64 page_dma, pg_sz;

	pg_sz = 1 << page_size_log2;
	for_each_sgtable_dma_sg(&umem->sgt_append.sgt, sg, sg_i) {
		page_dma = sg_dma_address(sg);
		page_count = sg_dma_len(sg) >> page_size_log2;

		for (page_i = 0; page_i < page_count; ++page_i) {
			seq_printf(s, "%#llx\n", page_dma);
			page_dma += pg_sz;
		}
	}
}

static void ionic_tbl_buf_show(struct seq_file *s, const char *w,
			       struct ionic_tbl_buf *buf)
{
	seq_printf(s, "%stbl_limit:\t%u\n", w, buf->tbl_limit);
	seq_printf(s, "%stbl_pages:\t%u\n", w, buf->tbl_pages);
	seq_printf(s, "%stbl_size:\t%zu\n", w, buf->tbl_size);
	seq_printf(s, "%stbl_dma:\t%#llx\n", w, buf->tbl_dma);
	seq_printf(s, "%spage_size_log2:\t%u\n", w, buf->page_size_log2);
}

static void ionic_tbl_buf_dump(struct seq_file *s, struct ionic_tbl_buf *buf)
{
	int page_i;

	if (!buf->tbl_buf)
		return;

	for (page_i = 0; page_i < buf->tbl_pages; ++page_i)
		seq_printf(s, "%llx\n", buf->tbl_buf[page_i]);
}

static void ionic_q_show(struct seq_file *s, const char *w,
			 struct ionic_queue *q)
{
	seq_printf(s, "%ssize:\t%#llx\n", w, (u64)q->size);
	seq_printf(s, "%sdma:\t%#llx\n", w, (u64)q->dma);
	seq_printf(s, "%sprod:\t%#06x (%#llx)\n",
		   w, q->prod, (u64)q->prod << q->stride_log2);
	seq_printf(s, "%scons:\t%#06x (%#llx)\n",
		   w, q->cons, (u64)q->cons << q->stride_log2);
	seq_printf(s, "%smask:\t%#06x\n", w, q->mask);
	seq_printf(s, "%sdepth_log2:\t%u\n", w, q->depth_log2);
	seq_printf(s, "%sstride_log2:\t%u\n", w, q->stride_log2);
	seq_printf(s, "%sdbell:\t%#llx\n", w, q->dbell);
}

static void ionic_q_dump(struct seq_file *s, struct ionic_queue *q)
{
	seq_hex_dump(s, "", DUMP_PREFIX_OFFSET, 16, 1, q->ptr, q->size, true);
}

static int ionic_dev_info_show(struct seq_file *s, void *v)
{
	struct ionic_ibdev *dev = s->private;
	struct ionic_lif_cfg *lif_cfg;

	lif_cfg = &dev->lif_cfg;

	seq_printf(s, "lif_index:\t%d\n", lif_cfg->lif_index);
	seq_printf(s, "dbid:\t%u\n", lif_cfg->dbid);

	seq_printf(s, "rdma_version:\t%u\n", lif_cfg->rdma_version);
	seq_printf(s, "rdma_minor_version:\t%u\n", lif_cfg->minor_version);
	seq_printf(s, "qp_opcodes:\t%u\n", lif_cfg->qp_opcodes);
	seq_printf(s, "admin_opcodes:\t%u\n", lif_cfg->admin_opcodes);
	seq_printf(s, "reset_cnt:\t%u\n", dev->reset_cnt);

	seq_printf(s, "aq_base:\t%u\n", lif_cfg->aq_base);
	seq_printf(s, "cq_base:\t%u\n", lif_cfg->cq_base);
	seq_printf(s, "eq_base:\t%u\n", lif_cfg->eq_base);

	seq_printf(s, "aq_count:\t%u\n", lif_cfg->aq_count);
	seq_printf(s, "eq_count:\t%u\n", lif_cfg->eq_count);

	seq_printf(s, "aq_qtype:\t%u\n", lif_cfg->aq_qtype);
	seq_printf(s, "sq_qtype:\t%u\n", lif_cfg->sq_qtype);
	seq_printf(s, "rq_qtype:\t%u\n", lif_cfg->rq_qtype);
	seq_printf(s, "cq_qtype:\t%u\n", lif_cfg->cq_qtype);
	seq_printf(s, "eq_qtype:\t%u\n", lif_cfg->eq_qtype);

	seq_printf(s, "max_stride:\t%u\n", lif_cfg->max_stride);

	seq_printf(s, "sq_expdb:\t%u\n", lif_cfg->sq_expdb);
	seq_printf(s, "rq_expdb:\t%u\n", lif_cfg->rq_expdb);
	seq_printf(s, "expdb_mask:\t%u\n", lif_cfg->expdb_mask);

	seq_printf(s, "udma_count:\t%u\n", lif_cfg->udma_count);
	seq_printf(s, "udma_qgrp_shift:\t%u\n", lif_cfg->udma_qgrp_shift);

	if (dev->aq_vec[0])
		seq_printf(s, "AQ[0] admin_state:\t%u\n",
			   atomic_read(&dev->aq_vec[0]->admin_state));

	seq_printf(s, "size_pdid:\t%u\n", dev->inuse_pdid.inuse_size);
	seq_printf(s, "size_mrid:\t%u\n", dev->inuse_mrid.inuse_size);
	seq_printf(s, "next_mrkey:\t%u\n", dev->next_mrkey);
	seq_printf(s, "size_cqid:\t%u\n", dev->lif_cfg.cq_count);
	seq_printf(s, "size_qpid:\t%u\n", dev->lif_cfg.qp_count);

	seq_printf(s, "page_size_supported:\t0x%llX\n",
		   lif_cfg->page_size_supported);
	seq_printf(s, "stats_type:\t0x%0X\n", lif_cfg->stats_type);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_dev_info);

static int ionic_eq_info_show(struct seq_file *s, void *v)
{
	struct ionic_eq *eq = s->private;
	struct ionic_intr __iomem *intr;

	seq_printf(s, "eqid:\t%u\n", eq->eqid);
	seq_printf(s, "intr:\t%u\n", eq->intr);

	ionic_q_show(s, "q.",  &eq->q);
	seq_printf(s, "enable:\t%u\n", eq->enable);
	seq_printf(s, "armed:\t%u\n", eq->armed);
	seq_printf(s, "irq:\t%u\n", eq->irq);
	seq_printf(s, "name:\t%s\n", eq->name);

	/* interrupt control readback */
	intr = &eq->dev->lif_cfg.intr_ctrl[eq->intr];
	seq_printf(s, "intr_coalesce_init:\t%#x\n", ioread32(&intr->coal_init));
	seq_printf(s, "intr_mask:\t%#x\n", ioread32(&intr->mask));
	seq_printf(s, "intr_credits:\t%#x\n", ioread32(&intr->credits));
	seq_printf(s, "intr_mask_assert:\t%#x\n", ioread32(&intr->mask_assert));
	seq_printf(s, "intr_coalesce:\t%#x\n", ioread32(&intr->coal));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_eq_info);

static int ionic_eq_q_show(struct seq_file *s, void *v)
{
	struct ionic_eq *eq = s->private;

	ionic_q_dump(s, &eq->q);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_eq_q);

static int ionic_mr_info_show(struct seq_file *s, void *v)
{
	struct ionic_mr *mr = s->private;

	seq_printf(s, "mrid:\t%u\n", mr->mrid);

	ionic_tbl_buf_show(s, "", &mr->buf);

	if (mr->umem)
		ionic_umem_show(s, "", mr->umem, mr->buf.page_size_log2);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_mr_info);

static int ionic_mr_umem_show(struct seq_file *s, void *v)
{
	struct ionic_mr *mr = s->private;

	ionic_umem_dump(s, mr->umem, mr->buf.page_size_log2);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_mr_umem);

static int ionic_mr_tbl_buf_show(struct seq_file *s, void *v)
{
	struct ionic_mr *mr = s->private;

	ionic_tbl_buf_dump(s, &mr->buf);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_mr_tbl_buf);

static int ionic_cq_info_show(struct seq_file *s, void *v)
{
	struct ionic_cq *cq = s->private;

	seq_printf(s, "cqid:\t%u\n", cq->cqid);
	seq_printf(s, "eqid:\t%u\n", cq->eqid);

	if (cq->q.ptr) {
		ionic_q_show(s, "", &cq->q);
		seq_printf(s, "arm_any_prod:\t%#06x\n", cq->arm_any_prod);
		seq_printf(s, "arm_sol_prod:\t%#06x\n", cq->arm_sol_prod);
	}

	if (cq->umem)
		ionic_umem_show(s, "", cq->umem, PAGE_SHIFT);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_cq_info);

static int ionic_cq_q_show(struct seq_file *s, void *v)
{
	struct ionic_cq *cq = s->private;

	ionic_q_dump(s, &cq->q);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_cq_q);

static int ionic_cq_umem_show(struct seq_file *s, void *v)
{
	struct ionic_cq *cq = s->private;

	ionic_umem_dump(s, cq->umem, PAGE_SHIFT);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_cq_umem);

static int ionic_aq_info_show(struct seq_file *s, void *v)
{
	struct ionic_aq *aq = s->private;

	seq_printf(s, "armed:\t%u\n", aq->armed);
	seq_printf(s, "aqid:\t%u\n", aq->aqid);
	seq_printf(s, "cqid:\t%u\n", aq->cqid);

	if (aq->q.ptr)
		ionic_q_show(s, "", &aq->q);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_aq_info);

static int ionic_aq_q_show(struct seq_file *s, void *v)
{
	struct ionic_aq *aq = s->private;

	ionic_q_dump(s, &aq->q);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_aq_q);

static int ionic_aq_wqe_show(struct seq_file *s, void *v)
{
	struct ionic_aq *aq = s->private;
	struct ionic_v1_admin_wqe *wqe;

	wqe = &aq->debug_wr->wqe;

	seq_hex_dump(s, "", DUMP_PREFIX_OFFSET, 16, 1, wqe, sizeof(*wqe),
		     true);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_aq_wqe);

static int ionic_aq_cqe_show(struct seq_file *s, void *v)
{
	struct ionic_aq *aq = s->private;
	struct ionic_v1_cqe *cqe;

	cqe = &aq->debug_wr->cqe;

	seq_hex_dump(s, "", DUMP_PREFIX_OFFSET, 16, 1, cqe, sizeof(*cqe),
		     true);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_aq_cqe);

struct ionic_dbg_admin_wr {
	struct ionic_aq *aq;
	struct ionic_admin_wr wr;
	void *data;
	dma_addr_t dma;
};

static int ionic_aq_data_show(struct seq_file *s, void *v)
{
	struct ionic_aq *aq = s->private;
	struct ionic_dbg_admin_wr *wr;

	wr = container_of(aq->debug_wr, struct ionic_dbg_admin_wr, wr);

	dma_sync_single_for_cpu(aq->dev->lif_cfg.hwdev, wr->dma, PAGE_SIZE,
				DMA_FROM_DEVICE);

	seq_hex_dump(s, "", DUMP_PREFIX_OFFSET, 16, 1,
		     wr->data, PAGE_SIZE, true);

	dma_sync_single_for_device(aq->dev->lif_cfg.hwdev, wr->dma, PAGE_SIZE,
				   DMA_FROM_DEVICE);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_aq_data);

static int ionic_qp_info_show(struct seq_file *s, void *v)
{
	struct ionic_qp *qp = s->private;

	seq_printf(s, "qpid:\t%u\n", qp->qpid);
	seq_printf(s, "udma_idx:\t%u\n", qp->udma_idx);
	seq_printf(s, "state:\t%d\n", qp->state);

	if (qp->sq.ptr) {
		ionic_q_show(s, "sq.", &qp->sq);
		seq_printf(s, "sq_msn_prod:\t%#06x\n",
			   qp->sq_msn_prod);
		seq_printf(s, "sq_msn_cons:\t%#06x\n",
			   qp->sq_msn_cons);
	}

	if (qp->sq_umem)
		ionic_umem_show(s, "sq.", qp->sq_umem, PAGE_SHIFT);

	seq_printf(s, "sq_is_cmb:\t%d\n", !!(qp->sq_cmb & IONIC_CMB_ENABLE));
	if (qp->sq_cmb & IONIC_CMB_ENABLE) {
		seq_printf(s, "sq_is_expdb:\t%d\n",
			   !!(qp->sq_cmb & IONIC_CMB_EXPDB));
		seq_printf(s, "sq_cmb_order:\t%d\n", qp->sq_cmb_order);
		seq_printf(s, "sq_cmb_pgid:\t%d\n", qp->sq_cmb_pgid);
		seq_printf(s, "sq_cmb_addr:\t%#llx\n",
			   (u64)qp->sq_cmb_addr);
	}

	seq_printf(s, "sq_flush:\t%d\n", qp->sq_flush);
	seq_printf(s, "sq_flush_rcvd:\t%d\n", qp->sq_flush_rcvd);
	seq_printf(s, "sq_spec:\t%d\n", qp->sq_spec);
	seq_printf(s, "sq_cqid:\t%u\n", qp->sq_cqid);

	if (qp->rq.ptr)
		ionic_q_show(s, "rq.", &qp->rq);

	if (qp->rq_umem)
		ionic_umem_show(s, "rq.", qp->rq_umem, PAGE_SHIFT);

	seq_printf(s, "rq_is_cmb:\t%d\n", !!(qp->rq_cmb & IONIC_CMB_ENABLE));
	if (qp->rq_cmb & IONIC_CMB_ENABLE) {
		seq_printf(s, "rq_is_expdb:\t%d\n",
			   !!(qp->rq_cmb & IONIC_CMB_EXPDB));
		seq_printf(s, "rq_cmb_order:\t%d\n", qp->rq_cmb_order);
		seq_printf(s, "rq_cmb_pgid:\t%d\n", qp->rq_cmb_pgid);
		seq_printf(s, "rq_cmb_addr:\t%#llx\n",
			   (u64)qp->rq_cmb_addr);
	}

	seq_printf(s, "rq_flush:\t%d\n", qp->rq_flush);
	seq_printf(s, "rq_spec:\t%d\n", qp->rq_spec);
	seq_printf(s, "rq_cqid:\t%u\n", qp->rq_cqid);

	if (qp->has_ah) {
		bool is_ip4 = false, is_ip6 = false;
		struct ib_ud_header *hdr = qp->hdr;

		seq_printf(s, "hdr_eth_smac:\t%pM\n", hdr->eth.smac_h);
		seq_printf(s, "hdr_eth_dmac:\t%pM\n", hdr->eth.dmac_h);

		if (hdr->eth.type == cpu_to_be16(ETH_P_8021Q)) {
			seq_printf(s, "hdr_eth_vlan:\t%u\n",
				   be16_to_cpu(hdr->vlan.tag));
			is_ip4 = hdr->vlan.type == cpu_to_be16(ETH_P_IP);
			is_ip6 = hdr->vlan.type == cpu_to_be16(ETH_P_IPV6);
		} else {
			is_ip4 = hdr->eth.type == cpu_to_be16(ETH_P_IP);
			is_ip6 = hdr->eth.type == cpu_to_be16(ETH_P_IPV6);
		}

		if (is_ip4) {
			seq_printf(s, "hdr_ip4_saddr:\t%pI4\n", &hdr->ip4.saddr);
			seq_printf(s, "hdr_ip4_daddr:\t%pI4\n", &hdr->ip4.daddr);
			seq_printf(s, "hdr_ip4_ttl:\t%u\n", hdr->ip4.ttl);
			seq_printf(s, "hdr_ip4_tos:\t%u\n", hdr->ip4.tos);
		}

		if (is_ip6) {
			seq_printf(s, "hdr_ip6_saddr:\t%pI6\n",
				   hdr->grh.source_gid.raw);
			seq_printf(s, "hdr_ip6_daddr:\t%pI6\n",
				   hdr->grh.destination_gid.raw);
			seq_printf(s, "hdr_ip6_flow_label:\t%u\n",
				   be32_to_cpu(hdr->grh.flow_label));
			seq_printf(s, "hdr_ip6_hop_limit:\t%u\n",
				   hdr->grh.hop_limit);
			seq_printf(s, "hdr_ip6_traffic_class:\t%u\n",
				   hdr->grh.traffic_class);
		}

		seq_printf(s, "hdr_udp_sport:\t%u\n", be16_to_cpu(hdr->udp.sport));
		seq_printf(s, "hdr_udp_dport:\t%u\n", be16_to_cpu(hdr->udp.dport));
	}

	seq_printf(s, "dcqcn_profile:\t%d\n", qp->dcqcn_profile);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_qp_info);

static int ionic_qp_sq_show(struct seq_file *s, void *v)
{
	struct ionic_qp *qp = s->private;

	ionic_q_dump(s, &qp->sq);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_qp_sq);

static int ionic_qp_sq_umem_show(struct seq_file *s, void *v)
{
	struct ionic_qp *qp = s->private;

	ionic_umem_dump(s, qp->sq_umem, PAGE_SHIFT);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_qp_sq_umem);

static int ionic_qp_rq_show(struct seq_file *s, void *v)
{
	struct ionic_qp *qp = s->private;

	ionic_q_dump(s, &qp->rq);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_qp_rq);

static int ionic_qp_rq_umem_show(struct seq_file *s, void *v)
{
	struct ionic_qp *qp = s->private;

	ionic_umem_dump(s, qp->rq_umem, PAGE_SHIFT);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ionic_qp_rq_umem);

void ionic_dbg_add_eq(struct ionic_ibdev *dev, struct ionic_eq *eq)
{
	char name[8];

	eq->debug = NULL;

	if (!dev->debug_eq)
		return;

	snprintf(name, sizeof(name), "%u", eq->eqid);

	eq->debug = debugfs_create_dir(name, dev->debug_eq);
	if (IS_ERR(eq->debug))
		eq->debug = NULL;
	if (!eq->debug)
		return;

	debugfs_create_file("info", 0440, eq->debug, eq,
			    &ionic_eq_info_fops);

	debugfs_create_file("q", 0440, eq->debug, eq,
			    &ionic_eq_q_fops);
}

void ionic_dbg_rm_eq(struct ionic_eq *eq)
{
	debugfs_remove_recursive(eq->debug);

	eq->debug = NULL;
}

void ionic_dbg_add_cq(struct ionic_ibdev *dev, struct ionic_cq *cq)
{
	char name[8];

	cq->debug = NULL;

	if (!dev->debug_cq)
		return;

	snprintf(name, sizeof(name), "%u", cq->cqid);

	cq->debug = debugfs_create_dir(name, dev->debug_cq);
	if (IS_ERR(cq->debug))
		cq->debug = NULL;
	if (!cq->debug)
		return;

	debugfs_create_file("info", 0440, cq->debug, cq,
			    &ionic_cq_info_fops);

	if (cq->q.ptr)
		debugfs_create_file("q", 0440, cq->debug, cq,
				    &ionic_cq_q_fops);

	if (cq->umem)
		debugfs_create_file("umem", 0440, cq->debug, cq,
				    &ionic_cq_umem_fops);
}

void ionic_dbg_rm_cq(struct ionic_cq *cq)
{
	debugfs_remove_recursive(cq->debug);

	cq->debug = NULL;
}

void ionic_dbg_add_aq(struct ionic_ibdev *dev, struct ionic_aq *aq)
{
	struct ionic_dbg_admin_wr *wr;
	char name[8];

	mutex_init(&aq->debug_mutex);

	aq->debug = NULL;

	if (!dev->debug_aq)
		return;

	snprintf(name, sizeof(name), "%u", aq->aqid);

	aq->debug = debugfs_create_dir(name, dev->debug_aq);
	if (IS_ERR(aq->debug))
		aq->debug = NULL;
	if (!aq->debug)
		return;

	debugfs_create_file("info", 0440, aq->debug, aq,
			    &ionic_aq_info_fops);

	if (aq->q.ptr)
		debugfs_create_file("q", 0440, aq->debug, aq,
				    &ionic_aq_q_fops);

	wr = kzalloc_obj(*wr);
	if (!wr)
		goto err_wr;

	wr->data = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!wr->data)
		goto err_data;

	wr->dma = dma_map_single(dev->lif_cfg.hwdev, wr->data, PAGE_SIZE,
				 DMA_FROM_DEVICE);
	if (dma_mapping_error(dev->lif_cfg.hwdev, wr->dma))
		goto err_dma;

	wr->wr.wqe.op = IONIC_V1_ADMIN_DEBUG;
	wr->wr.wqe.cmd.stats.dma_addr = cpu_to_le64(wr->dma);
	wr->wr.wqe.cmd.stats.length = cpu_to_le32(PAGE_SIZE);

	init_completion(&wr->wr.work);

	aq->debug_wr = &wr->wr;

	debugfs_create_file("dbg_wr_wqe", 0440, aq->debug, aq,
			    &ionic_aq_wqe_fops);

	debugfs_create_file("dbg_wr_cqe", 0440, aq->debug, aq,
			    &ionic_aq_cqe_fops);

	debugfs_create_file("dbg_wr_data", 0440, aq->debug, aq,
			    &ionic_aq_data_fops);

	return;

err_dma:
	kfree(wr->data);
err_data:
	kfree(wr);
err_wr:
	return;
}

void ionic_dbg_rm_aq(struct ionic_aq *aq)
{
	struct ionic_ibdev *dev = aq->dev;
	struct ionic_dbg_admin_wr *wr;

	debugfs_remove_recursive(aq->debug);

	aq->debug = NULL;

	mutex_destroy(&aq->debug_mutex);

	if (!aq->debug_wr)
		return;

	wr = container_of(aq->debug_wr, struct ionic_dbg_admin_wr, wr);

	dma_unmap_single(dev->lif_cfg.hwdev, wr->dma, PAGE_SIZE, DMA_FROM_DEVICE);
	kfree(wr->data);
	kfree(wr);
}

void ionic_dbg_add_qp(struct ionic_ibdev *dev, struct ionic_qp *qp)
{
	char name[8];

	qp->debug = NULL;

	if (!dev->debug_qp)
		return;

	snprintf(name, sizeof(name), "%u", qp->qpid);

	qp->debug = debugfs_create_dir(name, dev->debug_qp);
	if (IS_ERR(qp->debug))
		qp->debug = NULL;
	if (!qp->debug)
		return;

	debugfs_create_file("info", 0440, qp->debug, qp,
			    &ionic_qp_info_fops);

	if (qp->sq.ptr)
		debugfs_create_file("sq", 0440, qp->debug, qp,
				    &ionic_qp_sq_fops);

	if (qp->sq_umem)
		debugfs_create_file("sq_umem", 0440, qp->debug, qp,
				    &ionic_qp_sq_umem_fops);

	if (qp->rq.ptr)
		debugfs_create_file("rq", 0440, qp->debug, qp,
				    &ionic_qp_rq_fops);

	if (qp->rq_umem)
		debugfs_create_file("rq_umem", 0440, qp->debug, qp,
				    &ionic_qp_rq_umem_fops);
}

void ionic_dbg_rm_qp(struct ionic_qp *qp)
{
	debugfs_remove_recursive(qp->debug);

	qp->debug = NULL;
}

void ionic_dbg_add_mr(struct ionic_ibdev *dev, struct ionic_mr *mr)
{
	char name[8];

	mr->debug = NULL;

	if (!dev->debug_mr)
		return;

	snprintf(name, sizeof(name), "%u", ionic_mrid_index(mr->mrid));

	mr->debug = debugfs_create_dir(name, dev->debug_mr);
	if (IS_ERR(mr->debug))
		mr->debug = NULL;
	if (!mr->debug)
		return;

	debugfs_create_file("info", 0440, mr->debug, mr,
			    &ionic_mr_info_fops);

	if (mr->umem)
		debugfs_create_file("umem", 0440, mr->debug, mr,
				    &ionic_mr_umem_fops);

	if (mr->buf.tbl_buf)
		debugfs_create_file("buf", 0440, mr->debug, mr,
				    &ionic_mr_tbl_buf_fops);
}

void ionic_dbg_rm_mr(struct ionic_mr *mr)
{
	debugfs_remove_recursive(mr->debug);

	mr->debug = NULL;
}

void ionic_dbg_add_dev(struct ionic_ibdev *dev, struct dentry *parent)
{
	if (IS_ERR_OR_NULL(parent))
		return;

	dev->debug = debugfs_create_dir("rdma", parent);
	if (IS_ERR(dev->debug))
		dev->debug = NULL;
	if (!dev->debug)
		return;

	debugfs_create_file("info", 0440, dev->debug, dev,
			    &ionic_dev_info_fops);

	dev->debug_aq = debugfs_create_dir("aq", dev->debug);
	if (IS_ERR(dev->debug_aq))
		dev->debug_aq = NULL;

	dev->debug_cq = debugfs_create_dir("cq", dev->debug);
	if (IS_ERR(dev->debug_cq))
		dev->debug_cq = NULL;

	dev->debug_eq = debugfs_create_dir("eq", dev->debug);
	if (IS_ERR(dev->debug_eq))
		dev->debug_eq = NULL;

	dev->debug_mr = debugfs_create_dir("mr", dev->debug);
	if (IS_ERR(dev->debug_mr))
		dev->debug_mr = NULL;

	dev->debug_qp = debugfs_create_dir("qp", dev->debug);
	if (IS_ERR(dev->debug_qp))
		dev->debug_qp = NULL;
}

void ionic_dbg_rm_dev(struct ionic_ibdev *dev)
{
	debugfs_remove_recursive(dev->debug);

	dev->debug = NULL;
	dev->debug_cq = NULL;
	dev->debug_eq = NULL;
	dev->debug_mr = NULL;
	dev->debug_qp = NULL;
}
