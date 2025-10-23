// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)

#include <linux/dma/edma.h>
#include <linux/ntb.h>
#include <linux/pci-epc.h>

struct ntb_intr_dw {
	u64 base_addr;
	u64 end_addr;

	struct dw_edma *edma;
	resource_size_t rd_status_off;
	resource_size_t rd_clear_off;

	u32 __iomem *peer_mws[];
};

struct ntb_intr_dw_ctx {
	irq_handler_t handler;
	void *dev;
	struct dw_edma *edma;
};

static void dw_edma_selfirq_handler(struct dw_edma *dw, void *data)
{
	struct ntb_intr_dw_ctx *ctx = data;

	ctx->handler(0, ctx->dev);
}

static int dw_edma_find_backend_for_ntb(struct ntb_dev *ntb, struct ntb_intr_dw *intr_dw)
{
	struct pci_epc *epc = NULL;

	epc = ntb_get_pci_epc(ntb);
	if (!epc)
		return -ENODEV;
	intr_dw->edma = dw_edma_find_by_child(&epc->dev);
	if (!intr_dw->edma)
		return -ENODEV;
	dw_edma_selfirq_offsets(intr_dw->edma, &intr_dw->rd_status_off, &intr_dw->rd_clear_off);
	return 0;
}

static int dw_intr_init(struct ntb_dev *ntb, void (*desc_changed)(void *ctx))
{
	struct ntb_intr_dw *intr_dw;
	phys_addr_t mw_phys_addr;
	resource_size_t mw_size;
	int peer_widx;
	int peers;
	int ret;
	int i;

	peers = ntb_peer_port_count(ntb);
	if (peers <= 0)
		return -EINVAL;

	intr_dw = devm_kzalloc(&ntb->dev, struct_size(intr_dw, peer_mws, peers),
			       GFP_KERNEL);
	if (!intr_dw)
		return -ENOMEM;

	ret = dw_edma_find_backend_for_ntb(ntb, intr_dw);
	if (ret) {
		devm_kfree(&ntb->dev, intr_dw);
		return ret;
	}

	for (i = 0; i < peers; i++) {
		peer_widx = ntb_peer_mw_count(ntb) - 1 - i;

		ret = ntb_peer_mw_get_addr(ntb, peer_widx, &mw_phys_addr,
					   &mw_size);
		if (ret)
			goto unroll;

		intr_dw->peer_mws[i] = devm_ioremap(&ntb->dev, mw_phys_addr,
						    mw_size);
		if (!intr_dw->peer_mws[i]) {
			ret = -EFAULT;
			goto unroll;
		}
	}

	ntb->intr_priv = intr_dw;

	return 0;

unroll:
	for (i = 0; i < peers; i++)
		if (intr_dw->peer_mws[i])
			devm_iounmap(&ntb->dev, intr_dw->peer_mws[i]);

	devm_kfree(&ntb->dev, intr_dw);
	return ret;
}

static int dw_intr_setup_mws(struct ntb_dev *ntb)
{
	struct ntb_intr_dw *dwc = ntb->intr_priv;
	resource_size_t addr_align, size_align, offset;
	resource_size_t mw_size = SZ_32K;
	resource_size_t mw_min_size = mw_size;
	u64 addr = dwc->rd_status_off;
	int peer, peer_widx, ret;
	int i;

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			return peer_widx;

		ret = ntb_mw_get_align(ntb, peer, peer_widx, &addr_align,
				       NULL, NULL, NULL);
		if (ret)
			return ret;

		addr &= ~(addr_align - 1);
	}

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0) {
			ret = peer_widx;
			goto error_out;
		}

		ret = ntb_mw_get_align(ntb, peer, peer_widx, NULL,
				       &size_align, NULL, &offset);
		if (ret)
			goto error_out;

		mw_size = round_up(mw_size, size_align);
		if (mw_size < mw_min_size)
			mw_min_size = mw_size;

		ret = ntb_mw_set_trans(ntb, peer, peer_widx,
				       addr, mw_size, offset);
		if (ret)
			goto error_out;
	}

	dwc->base_addr = addr;
	dwc->end_addr = addr + mw_min_size;

	return 0;

error_out:
	for (i = 0; i < peer; i++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			continue;

		ntb_mw_clear_trans(ntb, i, peer_widx);
	}

	return ret;
}

static void dw_intr_clear_mws(struct ntb_dev *ntb)
{
	int peer, peer_widx;

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			continue;

		ntb_mw_clear_trans(ntb, peer, peer_widx);
	}
}

static void dw_intr_release_irq(void *data)
{
	struct ntb_intr_dw_ctx *ctx = data;

	dw_edma_unregister_selfirq(ctx->edma, dw_edma_selfirq_handler, ctx);
	kfree(ctx);
}

static int dw_intr_request_irq(struct ntb_dev *ntb, irq_handler_t h,
			       const char *name, void *dev_id,
			       struct ntb_intr_desc *intr_desc)
{
	struct ntb_intr_dw *dwc = ntb->intr_priv;
	struct dw_edma *edma = dwc->edma;
	int ret;

	if (intr_desc->ctx)
		return 1;

	struct ntb_intr_dw_ctx *ctx __free(kfree) = kzalloc(
						sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->handler = h;
	ctx->dev = dev_id;
	ctx->edma = edma;

	ret = dw_edma_register_selfirq(edma, dw_edma_selfirq_handler, ctx);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&ntb->dev, dw_intr_release_irq, ctx);
	if (ret)
		return ret;

	intr_desc->addr_offset = dwc->rd_status_off - dwc->base_addr;
	intr_desc->data = 0x0;
	intr_desc->ctx = no_free_ptr(ctx);
	return 1;
}

static void dw_intr_free_irq(struct ntb_dev *ntb, int irq, void *dev_id,
			     struct ntb_intr_desc *intr_desc)
{
	struct ntb_intr_dw *dwc = ntb->intr_priv;
	struct dw_edma *edma = dwc->edma;
	struct ntb_intr_dw_ctx *ctx;

	ctx = intr_desc->ctx;
	dw_edma_unregister_selfirq(edma, dw_edma_selfirq_handler, ctx);
	devm_remove_action(&ntb->dev, dw_intr_release_irq, ctx);
	kfree(ctx);
}

static int dw_intr_peer_trigger(struct ntb_dev *ntb, int peer, struct ntb_intr_desc *desc)
{
	struct ntb_intr_dw *intr_dw = ntb->intr_priv;
	int idx;

	idx = desc->addr_offset / sizeof(*intr_dw->peer_mws[peer]);

	iowrite32(desc->data, &intr_dw->peer_mws[peer][idx]);

	return 0;
}

static const struct ntb_intr_backend ntb_intr_backend_dw_edma = {
	.name = "dw-edma-testirq",
	.init = dw_intr_init,
	.setup_mws = dw_intr_setup_mws,
	.clear_mws = dw_intr_clear_mws,
	.request_irq = dw_intr_request_irq,
	.free_irq = dw_intr_free_irq,
	.peer_trigger = dw_intr_peer_trigger,
};

const struct ntb_intr_backend *ntb_intr_dw_edma_backend(void)
{
	return &ntb_intr_backend_dw_edma;
}
