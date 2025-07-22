// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */
#include <linux/kernel.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "mt76.h"
#include "dma.h"
#include "mt76_connac.h"

#define MT76_NPU_RX_BUF_SIZE	(1800 + \
				 SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))

static bool npu_enable = true;
module_param(npu_enable, bool, 0644);

int mt76_npu_fill_rx_queue(struct mt76_dev *dev, struct mt76_queue *q)
{
	int nframes = 0;

	while (q->queued < q->ndesc - 1) {
		struct airoha_npu_rx_dma_desc *desc = (void *)q->desc;
		struct mt76_queue_entry *e = &q->entry[q->head];
		struct page *page;
		int offset;

		e->buf = mt76_get_page_pool_buf(q, &offset, q->buf_size);
		if (!e->buf)
			break;

		e->dma_len[0] = SKB_WITH_OVERHEAD(q->buf_size);
		page = virt_to_head_page(e->buf);
		e->dma_addr[0] = page_pool_get_dma_addr(page) + offset;

		memset(&desc[q->head], 0, sizeof(*desc));
		desc[q->head].addr = e->dma_addr[0];

		q->head = (q->head + 1) % q->ndesc;
		q->queued++;
		nframes++;
	}

	return nframes;
}

void mt76_npu_queue_cleanup(struct mt76_dev *dev, struct mt76_queue *q)
{
	spin_lock_bh(&q->lock);
	while (q->queued > 0) {
		struct mt76_queue_entry *e = &q->entry[q->tail];

		dma_sync_single_for_cpu(dev->dma_dev, e->dma_addr[0],
					e->dma_len[0],
					page_pool_get_dma_dir(q->page_pool));
		mt76_put_page_pool_buf(e->buf, false);
		q->tail = (q->tail + 1) % q->ndesc;
		q->queued--;
	}
	spin_unlock_bh(&q->lock);
}

static struct sk_buff *mt76_npu_dequeue(struct mt76_dev *dev,
					struct mt76_queue *q,
					u32 *info)
{
	struct airoha_npu_rx_dma_desc *desc = (void *)q->desc;
	int i, nframes, index = q->tail;
	struct sk_buff *skb = NULL;

	nframes = FIELD_GET(NPU_RX_DMA_PKT_COUNT_MASK, desc[index].info);
	nframes = max_t(int, nframes, 1);

	for (i = 0; i < nframes; i++) {
		struct mt76_queue_entry *e = &q->entry[index];
		int len = FIELD_GET(NPU_RX_DMA_DESC_CUR_LEN_MASK,
				    desc[index].ctrl);

		if (!FIELD_GET(NPU_RX_DMA_DESC_DONE_MASK, desc[index].ctrl)) {
			dev_kfree_skb(skb);
			return NULL;
		}

		dma_sync_single_for_cpu(dev->dma_dev, e->dma_addr[0],
					e->dma_len[0],
					page_pool_get_dma_dir(q->page_pool));

		if (!skb) {
			skb = napi_build_skb(e->buf, q->buf_size);
			if (!skb)
				return NULL;

			__skb_put(skb, len);
			skb_reset_mac_header(skb);
		} else {
			struct skb_shared_info *shinfo = skb_shinfo(skb);
			struct page *page = virt_to_head_page(e->buf);
			int nr_frags = shinfo->nr_frags;

			if (nr_frags < ARRAY_SIZE(shinfo->frags))
				skb_add_rx_frag(skb, nr_frags, page,
						e->buf - page_address(page),
						len, q->buf_size);
		}

		*info = desc[index].info;
		index = (index + 1) % q->ndesc;
	}
	q->tail = index;
	q->queued -= i;
	Q_WRITE(q, dma_idx, q->tail);

	return skb;
}

static int mt76_npu_rx_poll(struct napi_struct *napi, int budget)
{
	struct mt76_dev *dev = mt76_priv(napi->dev);
	enum mt76_rxq_id qid = napi - dev->napi;
	struct airoha_npu *npu;
	int done = 0;

	rcu_read_lock();

	npu = rcu_dereference(dev->mmio.npu);
	if (!npu)
		goto out;

	while (done < budget) {
		struct sk_buff *skb;
		u32 info = 0;

		skb = mt76_npu_dequeue(dev, &dev->q_rx[qid], &info);
		if (!skb)
			break;

		dev->drv->rx_skb(dev, qid, skb, &info);
		mt76_rx_poll_complete(dev, qid, napi);
		done++;
	}

	mt76_npu_fill_rx_queue(dev, &dev->q_rx[qid]);
out:
	if (done < budget && napi_complete(napi))
		dev->drv->rx_poll_complete(dev, qid);

	rcu_read_unlock();

	return done;
}

static irqreturn_t mt76_npu_irq_handler(int irq, void *q_instance)
{
	struct mt76_queue *q = q_instance;
	struct mt76_dev *dev = q->dev;
	int qid = q - &dev->q_rx[0];
	int index = qid - MT_RXQ_NPU0;
	struct airoha_npu *npu;
	u32 status;

	rcu_read_lock();

	npu = rcu_dereference(dev->mmio.npu);
	if (!npu)
		goto out;

	status = airoha_npu_wlan_get_irq_status(npu, index);
	airoha_npu_wlan_set_irq_status(npu, status);

	airoha_npu_wlan_disable_irq(npu, index);
	napi_schedule(&dev->napi[qid]);
out:
	rcu_read_unlock();

	return IRQ_HANDLED;
}

int mt76_npu_dma_add_buf(struct mt76_phy *phy, struct mt76_queue *q,
			 struct sk_buff *skb, struct mt76_queue_buf *buf,
			 void *txwi_ptr)
{
	u16 txwi_len = min_t(u16, phy->dev->drv->txwi_size, NPU_TXWI_LEN);
	struct airoha_npu_tx_dma_desc *desc = (void *)q->desc;
	int ret;

	/* FIXME: Take into account unlinear skbs */
	memcpy(desc[q->head].txwi, txwi_ptr, txwi_len);
	desc[q->head].addr = buf->addr;
	desc[q->head].ctrl = FIELD_PREP(NPU_TX_DMA_DESC_VEND_LEN_MASK, txwi_len) |
			     FIELD_PREP(NPU_TX_DMA_DESC_LEN_MASK, skb->len) |
			     NPU_TX_DMA_DESC_DONE_MASK;

	ret = q->head;
	q->entry[q->head].skip_buf0 = true;
	q->entry[q->head].skip_buf1 = true;
	q->entry[q->head].txwi = NULL;
	q->entry[q->head].skb = NULL;
	q->entry[q->head].wcid = 0xffff;

	q->head = (q->head + 1) % q->ndesc;
	q->queued++;

	return ret;
}

void mt76_npu_txdesc_cleanup(struct mt76_queue *q, int index)
{
	struct airoha_npu_tx_dma_desc *desc = (void *)q->desc;

	if (!mt76_queue_is_npu_tx(q))
		return;

	desc[index].ctrl &= ~NPU_TX_DMA_DESC_DONE_MASK;
}

void mt76_npu_queue_setup(struct mt76_dev *dev, struct mt76_queue *q)
{
	int qid = FIELD_GET(MT_QFLAG_WED_RING, q->flags);
	bool xmit = mt76_queue_is_npu_tx(q);
	struct airoha_npu *npu;

	if (!mt76_queue_is_npu(q))
		return;

	mutex_lock(&dev->mutex);

	npu = rcu_dereference_protected(dev->mmio.npu, &dev->mutex);
	if (npu)
		q->wed_regs = airoha_npu_wlan_get_queue_addr(npu, qid, xmit);

	mutex_unlock(&dev->mutex);
}

int mt76_npu_rx_queue_init(struct mt76_dev *dev, struct mt76_queue *q)
{
	int err, irq, qid = q - &dev->q_rx[0];
	int size, index = qid - MT_RXQ_NPU0;
	struct airoha_npu *npu;
	const char *name;

	mutex_lock(&dev->mutex);

	npu = rcu_dereference_protected(dev->mmio.npu, &dev->mutex);
	irq = npu && index < ARRAY_SIZE(npu->irqs) ? npu->irqs[index]
						   : -EINVAL;
	if (irq < 0) {
		err = irq;
		goto error_unlock;
	}

	err = mt76_create_page_pool(dev, q);
	if (err)
		goto error_unlock;

	q->flags = MT_NPU_Q_RX(index);
	size = qid == MT_RXQ_NPU1 ? NPU_RX1_DESC_NUM : NPU_RX0_DESC_NUM;
	err = dev->queue_ops->alloc(dev, q, 0, size,
				    MT76_NPU_RX_BUF_SIZE, 0);
	if (err)
		goto erro_destroy_page_pool;

	name = devm_kasprintf(dev->dev, GFP_KERNEL, "mt76-npu" ".%d", index);
	if (!name) {
		err = -ENOMEM;
		goto erro_destroy_page_pool;
	}

	err = devm_request_irq(dev->dev, irq, mt76_npu_irq_handler,
			       IRQF_SHARED, name, q);
	if (err)
		goto erro_destroy_page_pool;

	netif_napi_add(dev->napi_dev, &dev->napi[qid], mt76_npu_rx_poll);
	mt76_npu_fill_rx_queue(dev, q);
	napi_enable(&dev->napi[qid]);

	mutex_unlock(&dev->mutex);

	return 0;

erro_destroy_page_pool:
	page_pool_destroy(q->page_pool);
error_unlock:
	mutex_unlock(&dev->mutex);

	return err;
}
EXPORT_SYMBOL_GPL(mt76_npu_rx_queue_init);

void mt76_npu_disable_irqs(struct mt76_dev *dev)
{
	struct airoha_npu *npu;
	u32 status;
	int i;

	rcu_read_lock();

	npu = rcu_dereference(dev->mmio.npu);
	if (!npu)
		goto unlock;

	for (i = MT_RXQ_NPU0; i <= MT_RXQ_NPU1; i++) {
		status = airoha_npu_wlan_get_irq_status(npu, i);
		airoha_npu_wlan_set_irq_status(npu, status);
		airoha_npu_wlan_disable_irq(npu, i);
	}
unlock:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(mt76_npu_disable_irqs);

int mt76_npu_init(struct mt76_dev *dev, phys_addr_t phy_addr, int type)
{
	struct airoha_npu *npu;
	int err = 0;

	/* NPU offloading is only supported by MT7992 */
	if (!is_mt7992(dev))
		return 0;

	if (!npu_enable)
		return 0;

	mutex_lock(&dev->mutex);

	npu = airoha_npu_get(dev->dev, NULL);
	if (IS_ERR(npu)) {
		request_module("airoha-npu");
		npu = airoha_npu_get(dev->dev, NULL);
	}

	if (IS_ERR(npu)) {
		err = PTR_ERR(npu);
		goto error_unlock;
	}

	err = airoha_npu_wlan_init_reserved_memory(npu);
	if (err)
		goto error_npu_put;

	dev->dma_dev = npu->dev;
	dev->mmio.phy_addr = phy_addr;
	dev->mmio.npu_type = type;

	rcu_assign_pointer(dev->mmio.npu, npu);
	synchronize_rcu();

	mutex_unlock(&dev->mutex);

	return err;

error_npu_put:
	airoha_npu_put(npu);
error_unlock:
	mutex_unlock(&dev->mutex);

	return err;
}
EXPORT_SYMBOL_GPL(mt76_npu_init);

void mt76_npu_deinit(struct mt76_dev *dev)
{
	struct airoha_npu *npu;

	mutex_lock(&dev->mutex);

	npu = rcu_replace_pointer(dev->mmio.npu, NULL,
				  lockdep_is_held(&dev->mutex));
	if (npu)
		airoha_npu_put(npu);

	mutex_unlock(&dev->mutex);

	mt76_npu_queue_cleanup(dev, &dev->q_rx[MT_RXQ_NPU0]);
	mt76_npu_queue_cleanup(dev, &dev->q_rx[MT_RXQ_NPU1]);
}
