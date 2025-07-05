// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/atomic.h>
#include <linux/crc-itu-t.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/greybus.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/minmax.h>
#include <linux/of.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#include "cpc.h"
#include "header.h"

#define CPC_SPI_CSUM_SIZE			2
#define GB_CPC_SPI_MSG_SIZE_MAX			2048
#define CPC_SPI_INTERRUPT_MAX_WAIT_MS		500

struct cpc_spi {
	struct cpc_host_device	cpc_hd;

	struct spi_device	*spi;

	struct task_struct *task;
	wait_queue_head_t event_queue;

	struct cpc_frame *tx_frame;
	u8 tx_csum[CPC_SPI_CSUM_SIZE];

	atomic_t event_cond;

	unsigned int rx_len;
	struct cpc_header rx_header;
	u8 rx_frame[GB_CPC_SPI_MSG_SIZE_MAX + CPC_SPI_CSUM_SIZE];
	u8 rx_csum[CPC_SPI_CSUM_SIZE];
};

struct cpc_xfer {
	u8		*data;
	unsigned int	total_len;
	unsigned int	remaining_len;
};

static inline struct cpc_spi *gb_hd_to_cpc_spi(struct gb_host_device *hd)
{
	return (struct cpc_spi *)&hd->hd_priv;
}

static inline struct cpc_spi *cpc_hd_to_cpc_spi(struct cpc_host_device *cpc_hd)
{
	return container_of(cpc_hd, struct cpc_spi, cpc_hd);
}

static int gb_cpc_spi_wake_tx(struct cpc_host_device *cpc_hd)
{
	struct cpc_spi *ctx = cpc_hd_to_cpc_spi(cpc_hd);

	wake_up_interruptible(&ctx->event_queue);

	return 0;
}

static bool buffer_is_zeroes(const u8 *buffer, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		if (buffer[i] != 0)
			return false;
	}

	return true;
}

static u16 gb_cpc_spi_csum(u16 start, const u8 *buffer, size_t length)
{
	return crc_itu_t(start, buffer, length);
}

static int gb_cpc_spi_do_xfer_header(struct cpc_spi *ctx)
{
	struct spi_transfer xfer_header = {
		.rx_buf = (u8 *)&ctx->rx_header,
		.len = CPC_HEADER_SIZE,
		.speed_hz = ctx->spi->max_speed_hz,
	};
	struct spi_transfer xfer_csum = {
		.rx_buf = &ctx->rx_csum,
		.len = sizeof(ctx->tx_csum),
		.speed_hz = ctx->spi->max_speed_hz,
	};
	enum cpc_frame_type type;
	struct spi_message msg;
	size_t payload_len = 0;
	u16 rx_csum;
	u16 csum;
	int ret;

	if (ctx->tx_frame) {
		u16 tx_hdr_csum = gb_cpc_spi_csum(0, (u8 *)&ctx->tx_frame->header, CPC_HEADER_SIZE);

		put_unaligned_le16(tx_hdr_csum, ctx->tx_csum);

		xfer_header.tx_buf = &ctx->tx_frame->header;
		xfer_csum.tx_buf = ctx->tx_csum;
	}

	spi_message_init(&msg);
	spi_message_add_tail(&xfer_header, &msg);
	spi_message_add_tail(&xfer_csum, &msg);

	ret = spi_sync(ctx->spi, &msg);
	if (ret)
		return ret;

	if (ctx->tx_frame) {
		if (!ctx->tx_frame->message) {
			cpc_frame_sent(ctx->tx_frame, ret);
			ctx->tx_frame = NULL;
		}
	}

	if (buffer_is_zeroes((u8 *)&ctx->rx_header, CPC_HEADER_SIZE))
		return 0;

	rx_csum = get_unaligned_le16(&ctx->rx_csum);
	csum = gb_cpc_spi_csum(0, (u8 *)&ctx->rx_header, CPC_HEADER_SIZE);

	if (rx_csum != csum || !cpc_header_get_type(&ctx->rx_header, &type)) {
		/*
		 * If the header checksum is invalid, its length can't be trusted, receive
		 * the maximum payload length to recover from that situation. If the frame
		 * type cannot be extracted from the header, use same recovery mechanism.
		 */
		ctx->rx_len = GB_CPC_SPI_MSG_SIZE_MAX;

		return 0;
	}

	if (type == CPC_FRAME_TYPE_DATA)
		payload_len = cpc_header_get_payload_len(&ctx->rx_header) +
			      sizeof(ctx->tx_csum);

	if (payload_len)
		ctx->rx_len = payload_len;
	else
		cpc_hd_rcvd(&ctx->cpc_hd, &ctx->rx_header, NULL, 0);

	return 0;
}

static int gb_cpc_spi_do_xfer_notch(struct cpc_spi *ctx)
{
	struct spi_transfer xfer = {
		.tx_buf = ctx->tx_csum,
		.len = 1,
		.speed_hz = ctx->spi->max_speed_hz,
	};
	struct spi_message msg;

	ctx->tx_csum[0] = 0;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(ctx->spi, &msg);
}

static unsigned int fill_xfer(struct spi_transfer *xfer,
			      u8 **tx, unsigned int *tx_len,
			      u8 **rx, unsigned int *rx_len)
{
	unsigned int xfer_len = 0;

	if (*tx_len && *rx_len)
		xfer_len = (*tx_len < *rx_len) ? *tx_len : *rx_len;
	else if (*tx_len)
		xfer_len = *tx_len;
	else if (*rx_len)
		xfer_len = *rx_len;
	else
		return 0;

	xfer->tx_buf = *tx;
	xfer->rx_buf = *rx;
	xfer->len = xfer_len;

	if (*tx) {
		*tx += xfer_len;
		*tx_len -= xfer_len;
	}

	if (*rx) {
		*rx += xfer_len;
		*rx_len -= xfer_len;
	}

	return xfer_len;
}

static int gb_cpc_spi_do_xfer_payload(struct cpc_spi *ctx)
{
	unsigned int rx_len = ctx->rx_len ? ctx->rx_len + CPC_SPI_CSUM_SIZE : 0;
	struct spi_transfer xfers[4];
	struct spi_message msg;
	int ret;

	unsigned int tx_lens[3] = { 0 };
	u8 *tx_ptrs[3] = { NULL };

	spi_message_init(&msg);

	if (ctx->tx_frame && ctx->tx_frame->message) {
		struct gb_message *m = ctx->tx_frame->message;
		unsigned int idx = 0;
		u16 csum = 0;

		tx_ptrs[idx]   = (u8 *)m->header;
		tx_lens[idx++] = sizeof(struct gb_operation_msg_hdr);
		csum = gb_cpc_spi_csum(csum, (u8 *)m->header, sizeof(struct gb_operation_msg_hdr));

		if (m->payload_size) {
			tx_ptrs[idx]   = m->payload;
			tx_lens[idx++] = m->payload_size;
			csum = gb_cpc_spi_csum(csum, m->payload, m->payload_size);
		}

		put_unaligned_le16(csum, ctx->tx_csum);

		tx_ptrs[idx]   = ctx->tx_csum;
		tx_lens[idx++] = CPC_SPI_CSUM_SIZE;
	}

	unsigned int tx_idx = 0;
	unsigned int tx_len = tx_lens[tx_idx];
	u8 *tx_ptr = tx_ptrs[tx_idx];
	u8 *rx_ptr = rx_len ? ctx->rx_frame : NULL;

	/*
	 * This loop goes over a list of TX elements to send. There can be 0, 2 or 3 (nothing,
	 * greybus header + csum, and optionally greybus payload).
	 * RX, if present, consists of only one element.
	 *	[ tx_ptr1; tx_len1 ] --> [ tx_ptr2; tx_len2 ] --> [ tx_ptr3; tx_len3 ]
	 *	[ rx_ptr1; rx_len1 ]
	 *
	 * The RX buffer can span over several TX buffers, the loop takes care of chunking into
	 * spi_transfer.
	 *
	 */
	for (unsigned int i = 0; i < ARRAY_SIZE(xfers); i++) {
		struct spi_transfer *xfer = &xfers[i];

		fill_xfer(xfer, &tx_ptr, &tx_len, &rx_ptr, &rx_len);

		spi_message_add_tail(xfer, &msg);

		/*
		 * If the rx pointer is not NULL, but the rx length is 0, it means that the rx
		 * buffer was fully transferred in this iteration.
		 */
		if (rx_ptr && !rx_len) {
			rx_ptr = NULL;

			/*
			 * And if tx_ptr is NULL, it means there was no TX data to send, so the
			 * transfer is done.
			 */
			if (!tx_ptr)
				break;
		}

		/*
		 * If tx_len is zero, it means we can go the next TX element to transfer.
		 */
		if (!tx_len) {
			tx_idx++;
			if (tx_idx < ARRAY_SIZE(tx_ptrs)) {
				tx_len = tx_lens[tx_idx];
				tx_ptr = tx_ptrs[tx_idx];
			} else {
				tx_len = 0;
				tx_ptr = NULL;
			}

			/*
			 * If there's nothing else to transfer and the rx_len was also NULL,
			 * that means the transfer is fully prepared.
			 */
			if (!tx_len && !rx_len)
				break;
		}
	}

	ret = spi_sync(ctx->spi, &msg);
	if (ret)
		goto exit;

	if (ctx->rx_len) {
		unsigned char *csum_ptr;
		u16 expected_csum;
		u16 csum;

		if (ret)
			goto exit;

		csum_ptr = ctx->rx_frame + ctx->rx_len;
		csum = get_unaligned_le16(csum_ptr);

		expected_csum = gb_cpc_spi_csum(0, ctx->rx_frame, ctx->rx_len);

		if (csum == expected_csum)
			cpc_hd_rcvd(&ctx->cpc_hd, &ctx->rx_header, ctx->rx_frame, ctx->rx_len);
	}

exit:
	ctx->rx_len = 0;

	return ret;
}

static int gb_cpc_spi_do_xfer_thread(void *data)
{
	struct cpc_spi *ctx = data;
	bool xfer_idle = true;
	int ret;

	while (!kthread_should_stop()) {
		if (xfer_idle) {
			ret = wait_event_interruptible(ctx->event_queue,
						       (!cpc_hd_tx_queue_empty(&ctx->cpc_hd) ||
							atomic_read(&ctx->event_cond) == 1 ||
							kthread_should_stop()));

			if (ret)
				continue;

			if (kthread_should_stop())
				return 0;

			if (!ctx->tx_frame)
				ctx->tx_frame = cpc_hd_dequeue(&ctx->cpc_hd);

			/*
			 * Reset thread event right before transmission to prevent interrupts that
			 * happened while the thread was already awake to wake up the thread again,
			 * as the event is going to be handled by this iteration.
			 */
			atomic_set(&ctx->event_cond, 0);

			ret = gb_cpc_spi_do_xfer_header(ctx);
			if (!ret)
				xfer_idle = false;
		} else {
			ret = wait_event_timeout(ctx->event_queue,
						 (atomic_read(&ctx->event_cond) == 1 ||
						  kthread_should_stop()),
						 msecs_to_jiffies(CPC_SPI_INTERRUPT_MAX_WAIT_MS));
			if (ret == 0) {
				dev_err_once(&ctx->spi->dev, "device didn't assert interrupt in a timely manner\n");
				continue;
			}

			atomic_set(&ctx->event_cond, 0);

			if (!ctx->tx_frame && !ctx->rx_len)
				ret = gb_cpc_spi_do_xfer_notch(ctx);
			else
				ret = gb_cpc_spi_do_xfer_payload(ctx);

			if (!ret)
				xfer_idle = true;
		}
	}

	return 0;
}

static irqreturn_t gb_cpc_spi_irq_handler(int irq, void *data)
{
	struct cpc_spi *ctx = data;

	atomic_set(&ctx->event_cond, 1);
	wake_up(&ctx->event_queue);

	return IRQ_HANDLED;
}

static int gb_cpc_spi_cport_allocate(struct gb_host_device *hd, int cport_id, unsigned long flags)
{
	struct cpc_spi *ctx = gb_hd_to_cpc_spi(hd);
	struct cpc_endpoint *ep;

	for (int i = 0; i < ARRAY_SIZE(ctx->cpc_hd.endpoints); i++) {
		if (ctx->cpc_hd.endpoints[i] != NULL)
			continue;

		if (cport_id < 0)
			cport_id = i;

		ep = cpc_endpoint_alloc(cport_id, GFP_KERNEL);
		if (!ep)
			return -ENOMEM;

		ep->cpc_hd = &ctx->cpc_hd;

		ctx->cpc_hd.endpoints[i] = ep;
		return cport_id;
	}

	return -ENOSPC;
}

static void gb_cpc_spi_cport_release(struct gb_host_device *hd, u16 cport_id)
{
	struct cpc_spi *ctx = gb_hd_to_cpc_spi(hd);
	struct cpc_endpoint *ep;

	for (int i = 0; i < ARRAY_SIZE(ctx->cpc_hd.endpoints); i++) {
		ep = ctx->cpc_hd.endpoints[i];
		if (ep && ep->id == cport_id) {
			cpc_endpoint_release(ep);
			ctx->cpc_hd.endpoints[i] = NULL;
			break;
		}
	}
}

static int gb_cpc_spi_cport_enable(struct gb_host_device *hd, u16 cport_id,
				   unsigned long flags)
{
	struct cpc_spi *ctx = gb_hd_to_cpc_spi(hd);
	struct cpc_endpoint *ep;

	ep = cpc_hd_get_endpoint(&ctx->cpc_hd, cport_id);
	if (!ep)
		return -ENODEV;

	return cpc_endpoint_connect(ep);
}

static int gb_cpc_spi_cport_disable(struct gb_host_device *hd, u16 cport_id)
{
	struct cpc_spi *ctx = gb_hd_to_cpc_spi(hd);
	struct cpc_endpoint *ep;

	ep = cpc_hd_get_endpoint(&ctx->cpc_hd, cport_id);
	if (!ep)
		return -ENODEV;

	return cpc_endpoint_disconnect(ep);
}

static int gb_cpc_spi_message_send(struct gb_host_device *hd, u16 cport_id,
				   struct gb_message *message, gfp_t gfp_mask)
{
	struct cpc_spi *ctx = gb_hd_to_cpc_spi(hd);
	struct cpc_endpoint *ep;
	struct cpc_frame *frame;

	frame = cpc_frame_alloc(message, gfp_mask);
	if (!frame)
		return -ENOMEM;

	ep = cpc_hd_get_endpoint(&ctx->cpc_hd, cport_id);
	if (!ep) {
		cpc_frame_free(frame);
		return -ENODEV;
	}

	message->hcpriv = frame;

	return cpc_endpoint_frame_send(ep, frame);
}

static void gb_cpc_spi_message_cancel(struct gb_message *message)
{
	struct cpc_frame *frame = message->hcpriv;

	frame->cancelled = true;
}

static struct gb_hd_driver gb_cpc_driver = {
	.hd_priv_size			= sizeof(struct cpc_spi),
	.message_send			= gb_cpc_spi_message_send,
	.message_cancel			= gb_cpc_spi_message_cancel,
	.cport_allocate			= gb_cpc_spi_cport_allocate,
	.cport_release			= gb_cpc_spi_cport_release,
	.cport_enable			= gb_cpc_spi_cport_enable,
	.cport_disable			= gb_cpc_spi_cport_disable,
};


static int cpc_spi_probe(struct spi_device *spi)
{
	struct gb_host_device *hd;
	struct cpc_spi *ctx;
	int ret;

	if (!spi->irq) {
		dev_err(&spi->dev, "cannot function without IRQ, please provide one\n");
		return -EINVAL;
	}

	hd = gb_hd_create(&gb_cpc_driver, &spi->dev,
			  GB_CPC_SPI_MSG_SIZE_MAX, GB_CPC_SPI_NUM_CPORTS);
	if (IS_ERR(hd))
		return PTR_ERR(hd);

	ctx = gb_hd_to_cpc_spi(hd);
	ctx->cpc_hd.gb_hd = hd;
	ctx->cpc_hd.wake_tx = gb_cpc_spi_wake_tx;

	spi_set_drvdata(spi, ctx);

	ret = gb_hd_add(hd);
	if (ret)
		goto err_hd_del;

	ret = request_irq(spi->irq, gb_cpc_spi_irq_handler, IRQF_TRIGGER_FALLING,
			  dev_name(&spi->dev), ctx);
	if (ret)
		goto err_hd_remove;

	ctx->task = kthread_run(gb_cpc_spi_do_xfer_thread, ctx, "%s",
				    dev_name(&spi->dev));
	if (IS_ERR(ctx->task)) {
		ret = PTR_ERR(ctx->task);
		goto free_irq;
	}

	return 0;

free_irq:
	free_irq(spi->irq, ctx);
err_hd_remove:
	gb_hd_del(hd);
err_hd_del:
	gb_hd_put(hd);

	return ret;
}

static void cpc_spi_remove(struct spi_device *spi)
{
	struct cpc_spi *ctx = spi_get_drvdata(spi);

	kthread_stop(ctx->task);
	free_irq(spi->irq, ctx);
	gb_hd_del(ctx->cpc_hd.gb_hd);
	gb_hd_put(ctx->cpc_hd.gb_hd);
}

static const struct of_device_id cpc_dt_ids[] = {
	{ .compatible = "silabs,cpc-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, cpc_dt_ids);

static const struct spi_device_id cpc_spi_ids[] = {
	{ .name = "cpc-spi" },
	{},
};
MODULE_DEVICE_TABLE(spi, cpc_spi_ids);

static struct spi_driver gb_cpc_spi_driver = {
	.driver = {
		.name = "cpc-spi",
		.of_match_table = cpc_dt_ids,
	},
	.probe = cpc_spi_probe,
	.remove = cpc_spi_remove,
};

module_spi_driver(gb_cpc_spi_driver);

MODULE_DESCRIPTION("Greybus Host Driver for Silicon Labs devices using SPI");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Damien Riégel <damien.riegel@silabs.com>");
