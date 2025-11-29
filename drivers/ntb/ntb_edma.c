// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)

#include <linux/module.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/ntb.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/dmaengine.h>
#include <linux/pci-epc.h>
#include <linux/dma/edma.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

#include "ntb_edma.h"

/*
 * The interrupt register offsets below are taken from the DesignWare
 * eDMA "unrolled" register map (EDMA_MF_EDMA_UNROLL). The remote eDMA
 * backend currently only supports this layout.
 */
#define DMA_WRITE_INT_STATUS_OFF   0x4c
#define DMA_WRITE_INT_MASK_OFF     0x54
#define DMA_WRITE_INT_CLEAR_OFF    0x58
#define DMA_READ_INT_STATUS_OFF    0xa0
#define DMA_READ_INT_MASK_OFF      0xa8
#define DMA_READ_INT_CLEAR_OFF     0xac

#define NTB_EDMA_NOTIFY_MAX_QP		64

static unsigned int edma_spi = 417; /* 0x1a1 */
module_param(edma_spi, uint, 0644);
MODULE_PARM_DESC(edma_spi, "SPI number used by remote eDMA interrupt (EP local)");

static u64 edma_regs_phys = 0xe65d5000;
module_param(edma_regs_phys, ullong, 0644);
MODULE_PARM_DESC(edma_regs_phys, "Physical base address of local eDMA registers (EP)");

static unsigned long edma_regs_size = 0x1200;
module_param(edma_regs_size, ulong, 0644);
MODULE_PARM_DESC(edma_regs_size, "Size of the local eDMA register space (EP)");

struct ntb_edma_intr {
	u32 db[NTB_EDMA_NOTIFY_MAX_QP];
};

struct ntb_edma_ctx {
	void *ll_wr_virt[EDMA_WR_CH_NUM];
	dma_addr_t ll_wr_phys[EDMA_WR_CH_NUM];
	void *ll_rd_virt[EDMA_RD_CH_NUM + 1];
	dma_addr_t ll_rd_phys[EDMA_RD_CH_NUM + 1];

	struct ntb_edma_intr *intr_ep_virt;
	dma_addr_t intr_ep_phys;
	struct ntb_edma_intr *intr_rc_virt;
	dma_addr_t intr_rc_phys;
	u32 notify_qp_max;

	bool initialized;
};

static struct ntb_edma_ctx edma_ctx;

typedef void (*ntb_edma_interrupt_cb_t)(void *data, int qp_num);

struct ntb_edma_interrupt {
	int virq;
	void __iomem *base;
	ntb_edma_interrupt_cb_t cb;
	void *data;
};

static struct ntb_edma_interrupt ntb_edma_intr;

static int ntb_edma_map_spi_to_virq(struct device *dev, unsigned int spi)
{
	struct device_node *np = dev_of_node(dev);
	struct device_node *parent;
	struct irq_fwspec fwspec = { 0 };
	int virq;

	parent = of_irq_find_parent(np);
	if (!parent)
		return -ENODEV;

	fwspec.fwnode      = of_fwnode_handle(parent);
	fwspec.param_count = 3;
	fwspec.param[0]    = GIC_SPI;
	fwspec.param[1]    = spi;
	fwspec.param[2]    = IRQ_TYPE_LEVEL_HIGH;

	virq = irq_create_fwspec_mapping(&fwspec);
	of_node_put(parent);
	return (virq > 0) ? virq : -EINVAL;
}

static irqreturn_t ntb_edma_isr(int irq, void *data)
{
	struct ntb_edma_interrupt *v = data;
	u32 mask = BIT(EDMA_RD_CH_NUM);
	u32 i, val;

	/*
	 * We do not ack interrupts here but instead we mask all local interrupt
	 * sources except the read channel used for notification. This reduces
	 * needless ISR invocations.
	 *
	 * In theory we could configure LIE=1/RIE=0 only for the notification
	 * transfer (keeping all other channels at LIE=1/RIE=1), but that would
	 * require intrusive changes to the dw-edma core.
	 *
	 * Note: The host side may have already cleared the read interrupt used
	 * for notification, so reading DMA_READ_INT_CLEAR_OFF is not a reliable
	 * way to detect it. As a result, we cannot reliably tell which specific
	 * channel triggered this interrupt. intr_ep_virt->db[i] teaches us
	 * instead.
	 */
	iowrite32(~0x0, v->base + DMA_WRITE_INT_MASK_OFF);
	iowrite32(~mask, v->base + DMA_READ_INT_MASK_OFF);

	if (!v->cb || !edma_ctx.intr_ep_virt)
		return IRQ_HANDLED;

	for (i = 0; i < edma_ctx.notify_qp_max; i++) {
		val = READ_ONCE(edma_ctx.intr_ep_virt->db[i]);
		if (!val)
			continue;

		WRITE_ONCE(edma_ctx.intr_ep_virt->db[i], 0);
		v->cb(v->data, i);
	}

	return IRQ_HANDLED;
}

int ntb_edma_setup_isr(struct device *dev, struct device *epc_dev,
		       ntb_edma_interrupt_cb_t cb, void *data)
{
	struct ntb_edma_interrupt *v = &ntb_edma_intr;
	int virq = ntb_edma_map_spi_to_virq(epc_dev->parent, edma_spi);
	int ret;

	if (virq < 0) {
		dev_err(dev, "failed to get virq (%d)\n", virq);
		return virq;
	}

	v->virq = virq;
	v->cb = cb;
	v->data = data;
	if (edma_regs_phys && !v->base)
		v->base = devm_ioremap(dev, edma_regs_phys, edma_regs_size);
	if (!v->base) {
		dev_err(dev, "failed to setup v->base\n");
		return -1;
	}
	ret = devm_request_irq(dev, v->virq, ntb_edma_isr, 0, "ntb-edma", v);
	if (ret)
		return ret;

	if (v->base) {
		iowrite32(0x0, v->base + DMA_WRITE_INT_MASK_OFF);
		iowrite32(0x0, v->base + DMA_READ_INT_MASK_OFF);
	}
	return 0;
}

void ntb_edma_teardown_isr(struct device *dev)
{
	struct ntb_edma_interrupt *v = &ntb_edma_intr;

	/* Mask all write/read interrupts so we don't get called again. */
	if (v->base) {
		iowrite32(~0x0, v->base + DMA_WRITE_INT_MASK_OFF);
		iowrite32(~0x0, v->base + DMA_READ_INT_MASK_OFF);
	}

	if (v->virq > 0)
		devm_free_irq(dev, v->virq, v);

	if (v->base)
		devm_iounmap(dev, v->base);

	v->virq = 0;
	v->cb = NULL;
	v->data = NULL;
}

int ntb_edma_setup_mws(struct ntb_dev *ndev)
{
	const size_t info_bytes = PAGE_SIZE;
	resource_size_t size_max, offset;
	dma_addr_t intr_phys, info_phys;
	u32 wr_done = 0, rd_done = 0;
	struct ntb_edma_intr *intr;
	struct ntb_edma_info *info;
	int peer_mw, mw_index, rc;
	struct iommu_domain *dom;
	bool reg_mapped = false;
	size_t ll_bytes, size;
	struct pci_epc *epc;
	struct device *dev;
	unsigned long iova;
	phys_addr_t phys;
	u64 need;
	u32 i;

	/* +1 is for interruption */
	ll_bytes = (EDMA_WR_CH_NUM + EDMA_RD_CH_NUM + 1) * DMA_LLP_MEM_SIZE;
	need = EDMA_REG_SIZE + info_bytes + ll_bytes;

	epc = ntb_get_pci_epc(ndev);
	if (!epc)
		return -ENODEV;
	dev = epc->dev.parent;

	if (edma_ctx.initialized)
		return 0;

	info = dma_alloc_coherent(dev, info_bytes, &info_phys, GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	memset(info, 0, info_bytes);
	info->magic = NTB_EDMA_INFO_MAGIC;
	info->wr_cnt = EDMA_WR_CH_NUM;
	info->rd_cnt = EDMA_RD_CH_NUM + 1; /* +1 for interruption */
	info->regs_phys = edma_regs_phys;
	info->ll_stride = DMA_LLP_MEM_SIZE;

	for (i = 0; i < EDMA_WR_CH_NUM; i++) {
		edma_ctx.ll_wr_virt[i] = dma_alloc_attrs(dev, DMA_LLP_MEM_SIZE,
							 &edma_ctx.ll_wr_phys[i],
							 GFP_KERNEL,
							 DMA_ATTR_FORCE_CONTIGUOUS);
		if (!edma_ctx.ll_wr_virt[i]) {
			rc = -ENOMEM;
			goto err_free_ll;
		}
		wr_done++;
		info->ll_wr_phys[i] = edma_ctx.ll_wr_phys[i];
	}
	for (i = 0; i < EDMA_RD_CH_NUM + 1; i++) {
		edma_ctx.ll_rd_virt[i] = dma_alloc_attrs(dev, DMA_LLP_MEM_SIZE,
							 &edma_ctx.ll_rd_phys[i],
							 GFP_KERNEL,
							 DMA_ATTR_FORCE_CONTIGUOUS);
		if (!edma_ctx.ll_rd_virt[i]) {
			rc = -ENOMEM;
			goto err_free_ll;
		}
		rd_done++;
		info->ll_rd_phys[i] = edma_ctx.ll_rd_phys[i];
	}

	/* For interruption */
	edma_ctx.notify_qp_max = NTB_EDMA_NOTIFY_MAX_QP;
	intr = dma_alloc_coherent(dev, sizeof(*intr), &intr_phys, GFP_KERNEL);
	if (!intr) {
		rc = -ENOMEM;
		goto err_free_ll;
	}
	memset(intr, 0, sizeof(*intr));
	edma_ctx.intr_ep_virt = intr;
	edma_ctx.intr_ep_phys = intr_phys;
	info->intr_dar_base = intr_phys;

	peer_mw = ntb_peer_mw_count(ndev);
	if (peer_mw <= 0) {
		rc = -ENODEV;
		goto err_free_ll;
	}

	mw_index = peer_mw - 1; /* last MW */

	rc = ntb_mw_get_align(ndev, 0, mw_index, 0, NULL, &size_max,
			      &offset);
	if (rc)
		goto err_free_ll;

	if (size_max < need) {
		rc = -ENOSPC;
		goto err_free_ll;
	}

	/* Map register space (direct) */
	dom = iommu_get_domain_for_dev(dev);
	if (dom) {
		phys = edma_regs_phys & PAGE_MASK;
		size = PAGE_ALIGN(EDMA_REG_SIZE + edma_regs_phys - phys);
		iova = phys;

		rc = iommu_map(dom, iova, phys, EDMA_REG_SIZE,
			       IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO, GFP_KERNEL);
		if (rc)
			dev_err(&ndev->dev, "failed to create direct mapping for eDMA reg space\n");
		reg_mapped = true;
	}

	rc = ntb_mw_set_trans(ndev, 0, mw_index, edma_regs_phys, EDMA_REG_SIZE, offset);
	if (rc)
		goto err_unmap_reg;

	offset += EDMA_REG_SIZE;

	/* Map ntb_edma_info */
	rc = ntb_mw_set_trans(ndev, 0, mw_index, info_phys, info_bytes, offset);
	if (rc)
		goto err_clear_trans;
	offset += info_bytes;

	/* Map LL location */
	for (i = 0; i < EDMA_WR_CH_NUM; i++) {
		rc = ntb_mw_set_trans(ndev, 0, mw_index, edma_ctx.ll_wr_phys[i],
				      DMA_LLP_MEM_SIZE, offset);
		if (rc)
			goto err_clear_trans;
		offset += DMA_LLP_MEM_SIZE;
	}
	for (i = 0; i < EDMA_RD_CH_NUM + 1; i++) {
		rc = ntb_mw_set_trans(ndev, 0, mw_index, edma_ctx.ll_rd_phys[i],
				      DMA_LLP_MEM_SIZE, offset);
		if (rc)
			goto err_clear_trans;
		offset += DMA_LLP_MEM_SIZE;
	}
	edma_ctx.initialized = true;

	return 0;

err_clear_trans:
	/*
	 * Tear down the NTB translation window used for the eDMA MW.
	 * There is no sub-range clear API for ntb_mw_set_trans(), so we
	 * unconditionally drop the whole mapping on error.
	 */
	ntb_mw_clear_trans(ndev, 0, mw_index);

err_unmap_reg:
	if (reg_mapped)
		iommu_unmap(dom, iova, size);
err_free_ll:
	while (rd_done--)
		dma_free_attrs(dev, DMA_LLP_MEM_SIZE,
			       edma_ctx.ll_rd_virt[rd_done],
			       edma_ctx.ll_rd_phys[rd_done],
			       DMA_ATTR_FORCE_CONTIGUOUS);
	while (wr_done--)
		dma_free_attrs(dev, DMA_LLP_MEM_SIZE,
			       edma_ctx.ll_wr_virt[wr_done],
			       edma_ctx.ll_wr_phys[wr_done],
			       DMA_ATTR_FORCE_CONTIGUOUS);
	if (edma_ctx.intr_ep_virt)
		dma_free_coherent(dev, sizeof(struct ntb_edma_intr),
				  edma_ctx.intr_ep_virt,
				  edma_ctx.intr_ep_phys);
	dma_free_coherent(dev, info_bytes, info, info_phys);
	return rc;
}

static int ntb_edma_irq_vector(struct device *dev, unsigned int nr)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret, nvec;

	nvec = pci_msi_vec_count(pdev);
	for (; nr < nvec; nr++) {
		ret = pci_irq_vector(pdev, nr);
		if (!irq_has_action(ret))
			return ret;
	}
	return 0;
}

static const struct dw_edma_plat_ops ntb_edma_ops = {
	.irq_vector     = ntb_edma_irq_vector,
};

int ntb_edma_setup_peer(struct ntb_dev *ndev)
{
	struct ntb_edma_info *info;
	unsigned int wr_cnt, rd_cnt;
	struct dw_edma_chip *chip;
	void __iomem *edma_virt;
	phys_addr_t edma_phys;
	resource_size_t mw_size;
	u64 off = EDMA_REG_SIZE;
	int peer_mw, mw_index;
	unsigned int i;
	int ret;

	peer_mw = ntb_peer_mw_count(ndev);
	if (peer_mw <= 0)
		return -ENODEV;

	mw_index = peer_mw - 1; /* last MW */

	ret = ntb_peer_mw_get_addr(ndev, mw_index, &edma_phys,
				   &mw_size);
	if (ret)
		return -1;

	edma_virt = ioremap(edma_phys, mw_size);

	chip = devm_kzalloc(&ndev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		ret = -ENOMEM;
		return ret;
	}

	chip->dev = &ndev->pdev->dev;
	chip->nr_irqs = 4;
	chip->ops = &ntb_edma_ops;
	chip->flags = 0;
	chip->reg_base = edma_virt;
	chip->mf = EDMA_MF_EDMA_UNROLL;

	info = edma_virt + off;
	if (info->magic != NTB_EDMA_INFO_MAGIC)
		return -EINVAL;
	wr_cnt = info->wr_cnt;
	rd_cnt = info->rd_cnt;
	chip->ll_wr_cnt = wr_cnt;
	chip->ll_rd_cnt = rd_cnt;
	off += PAGE_SIZE;

	edma_ctx.notify_qp_max = NTB_EDMA_NOTIFY_MAX_QP;
	edma_ctx.intr_ep_phys = info->intr_dar_base;
	if (edma_ctx.intr_ep_phys) {
		edma_ctx.intr_rc_virt =
			dma_alloc_coherent(&ndev->pdev->dev,
					   sizeof(struct ntb_edma_intr),
					   &edma_ctx.intr_rc_phys,
					   GFP_KERNEL);
		if (!edma_ctx.intr_rc_virt)
			return -ENOMEM;
		memset(edma_ctx.intr_rc_virt, 0,
		       sizeof(struct ntb_edma_intr));
	}

	for (i = 0; i < wr_cnt; i++) {
		chip->ll_region_wr[i].vaddr.io = edma_virt + off;
		chip->ll_region_wr[i].paddr = info->ll_wr_phys[i];
		chip->ll_region_wr[i].sz = DMA_LLP_MEM_SIZE;
		off += DMA_LLP_MEM_SIZE;
	}
	for (i = 0; i < rd_cnt; i++) {
		chip->ll_region_rd[i].vaddr.io = edma_virt + off;
		chip->ll_region_rd[i].paddr = info->ll_rd_phys[i];
		chip->ll_region_rd[i].sz = DMA_LLP_MEM_SIZE;
		off += DMA_LLP_MEM_SIZE;
	}

	if (!pci_dev_msi_enabled(ndev->pdev))
		return -ENXIO;

	ret = dw_edma_probe(chip);
	if (ret) {
		dev_err(&ndev->dev, "dw_edma_probe failed: %d\n", ret);
		return ret;
	}

	return 0;
}

struct ntb_edma_filter {
	struct device *dma_dev;
	u32 direction;
};

static bool ntb_edma_filter_fn(struct dma_chan *chan, void *arg)
{
	struct ntb_edma_filter *filter = arg;
	u32 dir = filter->direction;
	struct dma_slave_caps caps;
	int ret;

	if (chan->device->dev != filter->dma_dev)
		return false;

	ret = dma_get_slave_caps(chan, &caps);
	if (ret < 0)
		return false;

	return !!(caps.directions & dir);
}

void ntb_edma_teardown_chans(struct ntb_edma_chans *edma)
{
	unsigned int i;

	for (i = 0; i < edma->num_wr_chan; i++)
		dma_release_channel(edma->wr_chan[i]);

	for (i = 0; i < edma->num_rd_chan; i++)
		dma_release_channel(edma->rd_chan[i]);

	if (edma->intr_chan)
		dma_release_channel(edma->intr_chan);
}

int ntb_edma_setup_chans(struct device *dma_dev, struct ntb_edma_chans *edma)
{
	struct ntb_edma_filter filter;
	dma_cap_mask_t dma_mask;
	unsigned int i;

	dma_cap_zero(dma_mask);
	dma_cap_set(DMA_SLAVE, dma_mask);

	memset(edma, 0, sizeof(*edma));
	edma->dev = dma_dev;

	filter.dma_dev = dma_dev;
	filter.direction = BIT(DMA_DEV_TO_MEM);
	for (i = 0; i < EDMA_WR_CH_NUM; i++) {
		edma->wr_chan[i] = dma_request_channel(dma_mask,
						       ntb_edma_filter_fn,
						       &filter);
		if (!edma->wr_chan[i])
			break;
		edma->num_wr_chan++;
	}

	filter.direction = BIT(DMA_MEM_TO_DEV);
	for (i = 0; i < EDMA_RD_CH_NUM; i++) {
		edma->rd_chan[i] = dma_request_channel(dma_mask,
						       ntb_edma_filter_fn,
						       &filter);
		if (!edma->rd_chan[i])
			break;
		edma->num_rd_chan++;
	}

	edma->intr_chan = dma_request_channel(dma_mask, ntb_edma_filter_fn,
					      &filter);
	if (!edma->intr_chan)
		dev_warn(dma_dev,
			 "Remote eDMA notify channel could not be allocated\n");

	if (!edma->num_wr_chan || !edma->num_rd_chan) {
		dev_warn(dma_dev, "Remote eDMA channels failed to initialize\n");
		ntb_edma_teardown_chans(edma);
		return -ENODEV;
	}
	return 0;
}

struct dma_chan *ntb_edma_pick_chan(struct ntb_edma_chans *edma,
				    remote_edma_dir_t dir)
{
	unsigned int n, cur, idx;
	struct dma_chan **chans;
	atomic_t *cur_chan;

	if (dir == REMOTE_EDMA_WRITE) {
		n = edma->num_wr_chan;
		chans = edma->wr_chan;
		cur_chan = &edma->cur_wr_chan;
	} else {
		n = edma->num_rd_chan;
		chans = edma->rd_chan;
		cur_chan = &edma->cur_rd_chan;
	}
	if (WARN_ON_ONCE(!n))
		return NULL;

	/* Simple round-robin */
	cur = (unsigned int)atomic_inc_return(cur_chan) - 1;
	idx = cur % n;
	return chans[idx];
}

int ntb_edma_notify_peer(struct ntb_edma_chans *edma, int qp_num)
{
	struct dma_async_tx_descriptor *txd;
	struct dma_slave_config cfg;
	struct scatterlist sgl;
	dma_cookie_t cookie;
	struct device *dev;

	if (!edma || !edma->intr_chan)
		return -ENXIO;

	if (qp_num < 0 || qp_num >= edma_ctx.notify_qp_max)
		return -EINVAL;

	if (!edma_ctx.intr_rc_virt || !edma_ctx.intr_ep_phys)
		return -EINVAL;

	dev = edma->dev;
	if (!dev)
		return -ENODEV;

	WRITE_ONCE(edma_ctx.intr_rc_virt->db[qp_num], 1);

	/* Ensure store is visible before kicking the DMA transfer */
	wmb();

	sg_init_table(&sgl, 1);
	sg_dma_address(&sgl) = edma_ctx.intr_rc_phys + qp_num * sizeof(u32);
	sg_dma_len(&sgl) = sizeof(u32);

	memset(&cfg, 0, sizeof(cfg));
	cfg.dst_addr       = edma_ctx.intr_ep_phys + qp_num * sizeof(u32);
	cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.direction      = DMA_MEM_TO_DEV;

	if (dmaengine_slave_config(edma->intr_chan, &cfg))
		return -EINVAL;

	txd = dmaengine_prep_slave_sg(edma->intr_chan, &sgl, 1,
				      DMA_MEM_TO_DEV,
				      DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
	if (!txd)
		return -ENOSPC;

	cookie = dmaengine_submit(txd);
	if (dma_submit_error(cookie))
		return -ENOSPC;

	dma_async_issue_pending(edma->intr_chan);
	return 0;
}
