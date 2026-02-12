// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/kthread.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>

#include "cpc.h"
#include "header.h"
#include "host.h"

#define GB_CPC_SDIO_MSG_SIZE_MAX 4096
#define GB_CPC_SDIO_BLOCK_SIZE 256
#define GB_CPC_SDIO_ALIGNMENT 4
#define CPC_FRAME_HEADER_SIZE (sizeof(struct cpc_header) + sizeof(struct gb_operation_msg_hdr))

#define GB_CPC_SDIO_ADDR_FIFO		0x00
#define GB_CPC_SDIO_ADDR_RX_BYTES_CNT	0x0C

enum cpc_sdio_flags {
	CPC_SDIO_FLAG_RX_PENDING,
	CPC_SDIO_FLAG_SHUTDOWN,
};

struct cpc_sdio {
	struct cpc_host_device *cpc_hd;
	struct device *dev;
	struct sdio_func *func;

	struct work_struct rxtx_work;
	unsigned long flags;

	wait_queue_head_t event_queue;
	u8 max_aggregation;
};

struct frame_header {
	struct cpc_header cpc;
	struct gb_operation_msg_hdr gb;
} __packed;

static inline struct cpc_sdio *cpc_hd_to_cpc_sdio(struct cpc_host_device *cpc_hd)
{
	return (struct cpc_sdio *)cpc_hd->priv;
}

static int gb_cpc_sdio_wake_tx(struct cpc_host_device *cpc_hd)
{
	struct cpc_sdio *ctx = cpc_hd_to_cpc_sdio(cpc_hd);

	if (test_bit(CPC_SDIO_FLAG_SHUTDOWN, &ctx->flags))
		return 0;

	schedule_work(&ctx->rxtx_work);

	return 0;
}

/*
 * Return the memory requirement in bytes for the aggregated frame aligned to the block size.
 * All headers are contained in a dedicated block. Then payloads start at the beginning of second
 * block. Each individual payload must be 4-byte aligned, and the total must be block-aligned.
 */
static size_t cpc_sdio_get_aligned_size(struct cpc_sdio *ctx, struct sk_buff_head *frame_list)
{
	struct sk_buff *frame;
	size_t size = 0;

	skb_queue_walk(frame_list, frame)
		size += ALIGN(frame->len, GB_CPC_SDIO_ALIGNMENT);

	size = ALIGN(size, GB_CPC_SDIO_BLOCK_SIZE);

	return size;
}

static size_t cpc_sdio_build_aggregated_frame(struct cpc_sdio *ctx,
					      struct sk_buff_head *frame_list,
					      unsigned char **buffer)
{
	unsigned char *tx_buff;
	struct sk_buff *frame;
	__le32 *frame_count;
	size_t xfer_size;
	unsigned int i = 0;

	xfer_size = cpc_sdio_get_aligned_size(ctx, frame_list);

	tx_buff = kmalloc(xfer_size, GFP_KERNEL);
	if (!tx_buff)
		return 0;

	frame_count = (__le32 *)tx_buff;
	*frame_count = cpu_to_le32(skb_queue_len(frame_list));
	i += sizeof(*frame_count);

	/* First step is to aggregate all headers together */
	skb_queue_walk(frame_list, frame) {
		struct frame_header *fh = (struct frame_header *)&tx_buff[i];

		memcpy(fh, frame->data, sizeof(*fh));
		i += sizeof(*fh);
	}

	skb_queue_walk(frame_list, frame) {
		size_t payload_len, padding_len;

		if (frame->len <= CPC_FRAME_HEADER_SIZE)
			continue;

		payload_len = frame->len - CPC_FRAME_HEADER_SIZE;
		memcpy(&tx_buff[i], &frame->data[CPC_FRAME_HEADER_SIZE], payload_len);
		i += payload_len;

		padding_len = ALIGN(payload_len, GB_CPC_SDIO_ALIGNMENT) - payload_len;
		if (padding_len) {
			memset(&tx_buff[i], 0, padding_len);
			i += padding_len;
		}
	}

	*buffer = tx_buff;

	return xfer_size;
}

static bool cpc_sdio_get_payload_size(struct cpc_sdio *ctx, const struct frame_header *header,
				      size_t *payload_size)
{
	size_t gb_size;

	gb_size = le16_to_cpu(header->gb.size);

	if (gb_size < sizeof(header->gb)) {
		dev_dbg(ctx->dev, "Invalid Greybus header size: %zu\n", gb_size);
		return false;
	}

	if (gb_size > (GB_CPC_SDIO_MSG_SIZE_MAX + sizeof(header->gb))) {
		dev_dbg(ctx->dev, "Payload size exceeds maximum: %zu\n", gb_size);
		return false;
	}

	*payload_size = gb_size - sizeof(header->gb);

	return true;
}

/*
 * Process aggregated frame
 * Reconstructed frame layout:
 * +-----+-----+-----+------+------+------+------+-------+---------+
 * | CPC Header (4B) | Size | OpID | Type | Stat | CPort | Payload |
 * +-----+-----+-----+------+------+------+------+-------+---------+
 */
static void cpc_sdio_process_aggregated_frame(struct cpc_sdio *ctx, unsigned char *aggregated_frame,
					      unsigned int frame_len)
{
	const unsigned char *payload_start;
	const struct frame_header *header;
	unsigned int payload_offset;
	size_t aligned_payload_size;
	struct sk_buff *rx_skb;
	__le32 frame_count_le;
	size_t payload_size;
	size_t frame_size;
	u32 frame_count;

	frame_count_le = *((__le32 *)aggregated_frame);
	frame_count = le32_to_cpu(frame_count_le);

	if (frame_count > ctx->max_aggregation) {
		dev_warn(ctx->dev,
			 "Process aggregated frame: frame count %u exceeds negotiated maximum %u\n",
			 frame_count, ctx->max_aggregation);
		return;
	}

	/* Header starts at block 0 after frame count */
	header = (struct frame_header *)&aggregated_frame[sizeof(frame_count_le)];

	/* Payloads start at block 1 (after complete block 0) */
	payload_offset = (frame_count * CPC_FRAME_HEADER_SIZE) + sizeof(frame_count_le);

	for (unsigned int i = 0; i < frame_count; i++) {
		payload_start = &aggregated_frame[payload_offset];

		if (!cpc_sdio_get_payload_size(ctx, header, &payload_size)) {
			dev_err(ctx->dev,
				"Process aggregated frame: failed to get payload size, aborting.\n");
			return;
		}

		aligned_payload_size = ALIGN(payload_size, GB_CPC_SDIO_ALIGNMENT);

		if (payload_offset + aligned_payload_size > frame_len) {
			dev_err(ctx->dev,
				"Process aggregated frame: payload is out of buffer boundary, aborting at frame %u\n",
				i);
			return;
		}

		frame_size = CPC_FRAME_HEADER_SIZE + payload_size;

		rx_skb = alloc_skb(frame_size, GFP_KERNEL);
		if (!rx_skb)
			return;

		memcpy(skb_put(rx_skb, CPC_FRAME_HEADER_SIZE), header, CPC_FRAME_HEADER_SIZE);

		if (payload_size > 0)
			memcpy(skb_put(rx_skb, payload_size), payload_start, payload_size);

		cpc_hd_rcvd(ctx->cpc_hd, rx_skb);

		header++;
		payload_offset += aligned_payload_size;
	}
}

static u32 cpc_sdio_get_rx_num_bytes(struct sdio_func *func, int *err)
{
	unsigned int rx_num_block_addr = GB_CPC_SDIO_ADDR_RX_BYTES_CNT;

	return sdio_readl(func, rx_num_block_addr, err);
}

static void gb_cpc_sdio_rx(struct cpc_sdio *ctx)
{
	unsigned char *rx_buff;
	size_t data_len;
	int err;

	sdio_claim_host(ctx->func);
	data_len = cpc_sdio_get_rx_num_bytes(ctx->func, &err);

	if (err) {
		dev_err(ctx->dev, "failed to obtain byte count (%d)\n", err);
		goto release_host;
	}

	if (data_len < sizeof(u32) + CPC_FRAME_HEADER_SIZE) {
		dev_err(ctx->dev, "failed to obtain enough bytes for a header (%zu)\n", data_len);
		goto release_host;
	}

	rx_buff = kmalloc(data_len, GFP_KERNEL);
	if (!rx_buff)
		goto release_host;

	err = sdio_readsb(ctx->func, rx_buff, GB_CPC_SDIO_ADDR_FIFO, data_len);
	sdio_release_host(ctx->func);

	if (err) {
		dev_err(ctx->dev, "failed to sdio_readsb (%d)\n", err);
		goto free_rx_skb;
	}

	if (data_len < GB_CPC_SDIO_BLOCK_SIZE) {
		dev_err(ctx->dev, "received %zd bytes, expected at least %u bytes\n", data_len,
			GB_CPC_SDIO_BLOCK_SIZE);
		goto free_rx_skb;
	}

	cpc_sdio_process_aggregated_frame(ctx, rx_buff, data_len);

free_rx_skb:
	kfree(rx_buff);

	return;

release_host:
	sdio_release_host(ctx->func);
}

static int gb_cpc_sdio_tx(struct cpc_sdio *ctx)
{
	struct sk_buff_head frame_list;
	unsigned char *tx_buff;
	size_t tx_len;
	int pkt_sent;
	int err;

	skb_queue_head_init(&frame_list);

	cpc_hd_dequeue_many(ctx->cpc_hd, &frame_list, ctx->max_aggregation);

	if (skb_queue_empty(&frame_list))
		return 0;

	tx_len = cpc_sdio_build_aggregated_frame(ctx, &frame_list, &tx_buff);
	if (!tx_len) {
		dev_err(ctx->dev, "failed to build aggregated frame\n");
		goto cleanup_frames;
	}

	sdio_claim_host(ctx->func);
	err = sdio_writesb(ctx->func, GB_CPC_SDIO_ADDR_FIFO, tx_buff, tx_len);
	sdio_release_host(ctx->func);

	kfree(tx_buff);

cleanup_frames:
	pkt_sent = skb_queue_len(&frame_list);
	skb_queue_purge(&frame_list);

	return pkt_sent;
}

static void gb_cpc_sdio_rxtx_work(struct work_struct *work)
{
	struct cpc_sdio *ctx = container_of(work, struct cpc_sdio, rxtx_work);
	int num_tx = 0;

	do {
		if (test_and_clear_bit(CPC_SDIO_FLAG_RX_PENDING, &ctx->flags))
			gb_cpc_sdio_rx(ctx);

		num_tx = gb_cpc_sdio_tx(ctx);
	} while (num_tx);
}

static struct cpc_hd_driver cpc_sdio_driver = {
	.wake_tx = gb_cpc_sdio_wake_tx,
};

static int cpc_sdio_init(struct sdio_func *func)
{
	unsigned char rx_data_ready_irq_en_bit = BIT(0);
	unsigned int irq_enable_addr = 0x09;
	int err;

	sdio_writeb(func, rx_data_ready_irq_en_bit, irq_enable_addr, &err);

	return err;
}

static void cpc_sdio_irq_handler(struct sdio_func *func)
{
	unsigned int rx_data_pending_irq_bit = 0;
	unsigned int irq_status_addr = 0x08;
	unsigned long int_status;
	struct cpc_sdio *ctx;
	struct device *dev;
	int err;

	ctx = sdio_get_drvdata(func);
	dev = &func->dev;

	int_status = sdio_readb(func, irq_status_addr, &err);
	if (err)
		return;

	if (__test_and_clear_bit(rx_data_pending_irq_bit, &int_status)) {
		sdio_writeb(func, BIT(rx_data_pending_irq_bit), irq_status_addr, &err);
		if (err)
			return;

		set_bit(CPC_SDIO_FLAG_RX_PENDING, &ctx->flags);
		schedule_work(&ctx->rxtx_work);
	}

	if (int_status)
		dev_err_once(dev, "unhandled interrupt from the device (%ld)\n", int_status);
}

static int cpc_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	struct cpc_host_device *cpc_hd;
	struct cpc_sdio *ctx;
	int err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	cpc_hd = cpc_hd_create(&cpc_sdio_driver, &func->dev, ctx);
	if (IS_ERR(cpc_hd)) {
		kfree(ctx);
		return PTR_ERR(cpc_hd);
	}

	ctx->cpc_hd = cpc_hd;
	ctx->dev = cpc_hd_dev(cpc_hd);
	ctx->func = func;
	ctx->max_aggregation = 1;

	INIT_WORK(&ctx->rxtx_work, gb_cpc_sdio_rxtx_work);

	sdio_set_drvdata(func, ctx);

	sdio_claim_host(func);

	err = sdio_enable_func(func);
	if (err)
		goto release_host;

	err = sdio_set_block_size(func, GB_CPC_SDIO_BLOCK_SIZE);
	if (err)
		goto disable_func;

	err = cpc_sdio_init(func);
	if (err)
		goto disable_func;

	err = sdio_claim_irq(func, cpc_sdio_irq_handler);
	if (err)
		goto disable_func;

	err = cpc_hd_add(cpc_hd);
	if (err)
		goto release_irq;

	sdio_release_host(func);

	return 0;

release_irq:
	sdio_release_irq(func);

disable_func:
	sdio_disable_func(func);

release_host:
	sdio_release_host(func);
	cpc_hd_put(cpc_hd);
	kfree(ctx);

	return err;
}

static void cpc_sdio_remove(struct sdio_func *func)
{
	struct cpc_sdio *ctx = sdio_get_drvdata(func);
	struct cpc_host_device *cpc_hd = ctx->cpc_hd;

	set_bit(CPC_SDIO_FLAG_SHUTDOWN, &ctx->flags);


	sdio_claim_host(func);
	sdio_release_irq(func);

	cancel_work_sync(&ctx->rxtx_work);

	sdio_disable_func(func);
	sdio_release_host(func);

	cpc_hd_del(cpc_hd);
	cpc_hd_put(cpc_hd);

	kfree(ctx);
}

static const struct sdio_device_id sdio_ids[] = {
	/* No official ID */
	{ SDIO_DEVICE(0x0000, 0x1000), },
	{ },
};
MODULE_DEVICE_TABLE(sdio, sdio_ids);

static struct sdio_driver gb_cpc_sdio_driver = {
	.name     = KBUILD_MODNAME,
	.id_table = sdio_ids,
	.probe    = cpc_sdio_probe,
	.remove   = cpc_sdio_remove,
	.drv = {
		.owner = THIS_MODULE,
		.name  = KBUILD_MODNAME,
	},
};

module_sdio_driver(gb_cpc_sdio_driver);

MODULE_DESCRIPTION("Greybus Host Driver for Silicon Labs devices using SDIO");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Damien Riégel <damien.riegel@silabs.com>");
