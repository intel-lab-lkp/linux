// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/atomic.h>
#include <linux/crc-itu-t.h>
#include <linux/delay.h>
#include <linux/device.h>
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
#include "interface.h"
#include "spi.h"

#define CPC_SPI_CSUM_SIZE		2
#define CPC_SPI_INTERRUPT_MAX_WAIT_MS	1000
#define CPC_SPI_MAX_PAYLOAD_SIZE	4096

struct cpc_spi {
	struct spi_device *spi;
	struct cpc_interface *intf;

	struct task_struct *task;
	wait_queue_head_t event_queue;

	struct sk_buff *tx_skb;
	u8 tx_csum[CPC_SPI_CSUM_SIZE];

	atomic_t event_cond;
	struct sk_buff *rx_skb;
	unsigned int rx_len;
	u8 rx_header[CPC_HEADER_SIZE + CPC_SPI_CSUM_SIZE];
};

static bool buffer_is_zeroes(const u8 *buffer, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		if (buffer[i] != 0)
			return false;
	}

	return true;
}

static u16 cpc_spi_csum(const u8 *buffer, size_t length)
{
	return crc_itu_t(0, buffer, length);
}

static int cpc_spi_do_xfer_header(struct cpc_spi *ctx)
{
	struct spi_transfer xfer_header = {
		.rx_buf = ctx->rx_header,
		.len = CPC_HEADER_SIZE,
		.speed_hz = ctx->spi->max_speed_hz,
	};
	struct spi_transfer xfer_csum = {
		.rx_buf = &ctx->rx_header[CPC_HEADER_SIZE],
		.len = sizeof(ctx->tx_csum),
		.speed_hz = ctx->spi->max_speed_hz,
	};
	enum cpc_frame_type type;
	struct spi_message msg;
	size_t payload_len = 0;
	struct sk_buff *skb;
	u16 rx_csum;
	u16 csum;
	int ret;

	if (ctx->tx_skb) {
		u16 tx_hdr_csum = cpc_spi_csum(ctx->tx_skb->data, CPC_HEADER_SIZE);

		put_unaligned_le16(tx_hdr_csum, ctx->tx_csum);

		xfer_header.tx_buf = ctx->tx_skb->data;
		xfer_csum.tx_buf = ctx->tx_csum;
	}

	spi_message_init(&msg);
	spi_message_add_tail(&xfer_header, &msg);
	spi_message_add_tail(&xfer_csum, &msg);

	ret = spi_sync(ctx->spi, &msg);
	if (ret)
		return ret;

	if (ctx->tx_skb) {
		if (skb_headlen(ctx->tx_skb) == CPC_HEADER_SIZE) {
			kfree_skb(ctx->tx_skb);
			ctx->tx_skb = NULL;
		} else {
			skb_pull(ctx->tx_skb, CPC_HEADER_SIZE);
		}
	}

	if (buffer_is_zeroes(ctx->rx_header, CPC_HEADER_SIZE))
		return 0;

	rx_csum = get_unaligned_le16(&ctx->rx_header[CPC_HEADER_SIZE]);
	csum = cpc_spi_csum(ctx->rx_header, CPC_HEADER_SIZE);

	if (rx_csum != csum || !cpc_header_get_type(ctx->rx_header, &type)) {
		/*
		 * If the header checksum is invalid, its length can't be trusted, receive
		 * the maximum payload length to recover from that situation. If the frame
		 * type cannot be extracted from the header, use same recovery mechanism.
		 */
		ctx->rx_len = CPC_SPI_MAX_PAYLOAD_SIZE;

		return 0;
	}

	if (type == CPC_FRAME_TYPE_DATA)
		payload_len = cpc_header_get_payload_len(ctx->rx_header) +
			      sizeof(ctx->tx_csum);

	skb = cpc_skb_alloc(payload_len, GFP_KERNEL);
	if (!skb) {
		/*
		 * Failed to allocate memory to receive the payload. Driver must clock in
		 * these bytes even if there is no room, to keep the sender in sync.
		 */
		ctx->rx_len = payload_len;

		return 0;
	}

	memcpy(skb_push(skb, CPC_HEADER_SIZE), ctx->rx_header, CPC_HEADER_SIZE);

	if (payload_len) {
		ctx->rx_skb = skb;
		ctx->rx_len = payload_len;
	} else {
		cpc_interface_receive_frame(ctx->intf, skb);
	}

	return 0;
}

static int cpc_spi_do_xfer_notch(struct cpc_spi *ctx)
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

static int cpc_spi_do_xfer_payload(struct cpc_spi *ctx)
{
	struct spi_transfer shared_xfer = {
		.speed_hz = ctx->spi->max_speed_hz,
		.rx_buf = NULL,
		.tx_buf = NULL,
	};
	struct spi_transfer pad_xfer1 = {
		.speed_hz = ctx->spi->max_speed_hz,
		.rx_buf = NULL,
		.tx_buf = NULL,
	};
	struct spi_transfer pad_xfer2 = {
		.speed_hz = ctx->spi->max_speed_hz,
		.rx_buf = NULL,
		.tx_buf = NULL,
	};
	unsigned int rx_len = ctx->rx_len;
	unsigned int tx_data_len;
	struct spi_message msg;
	int ret;

	spi_message_init(&msg);
	spi_message_add_tail(&shared_xfer, &msg);

	/*
	 * This can happen if header checksum was invalid. In that case, protocol
	 * mandates to be ready to receive the maximum number of bytes that the
	 * device is capable to send, in order to be sure its TX queue is flushed.
	 */
	if (!ctx->rx_skb && rx_len) {
		shared_xfer.rx_buf = kmalloc(rx_len, GFP_KERNEL);
		if (!shared_xfer.rx_buf)
			return -ENOMEM;

		shared_xfer.len = rx_len;
	}

	if (ctx->rx_skb && !ctx->tx_skb) {
		shared_xfer.rx_buf = skb_put(ctx->rx_skb, rx_len);
		shared_xfer.len = rx_len;
	}

	if (ctx->tx_skb) {
		u16 csum = ctx->tx_skb->csum;

		put_unaligned_le16(csum, ctx->tx_csum);

		tx_data_len = ctx->tx_skb->len;

		shared_xfer.tx_buf = ctx->tx_skb->data;
		shared_xfer.len = tx_data_len;

		if (!ctx->rx_skb) {
			pad_xfer1.tx_buf = ctx->tx_csum;
			pad_xfer1.len = sizeof(ctx->tx_csum);

			spi_message_add_tail(&pad_xfer1, &msg);
		}
	}

	if (ctx->rx_skb && ctx->tx_skb) {
		unsigned int shared_len;
		unsigned int pad_len;

		shared_len = min(rx_len, tx_data_len);
		pad_len = max(rx_len, tx_data_len) - shared_len;

		shared_xfer.rx_buf = skb_put(ctx->rx_skb, shared_len);
		shared_xfer.len = shared_len;

		if (rx_len < tx_data_len) {
			/*
			 * |------- RX BUFFER + RX CSUM ------|
			 * |------------------- TX BUFFER ------------|---- TX CSUM ----|
			 *
			 * |             SHARED               |
			 *                                    | PAD 1 |
			 *                                            |      PAD 2      |
			 */
			pad_xfer1.rx_buf = NULL;
			pad_xfer1.tx_buf = ctx->tx_skb->data + shared_len;
			pad_xfer1.len = pad_len;

			pad_xfer2.rx_buf = NULL;
			pad_xfer2.tx_buf = ctx->tx_csum;
			pad_xfer2.len = sizeof(ctx->tx_csum);

			spi_message_add_tail(&pad_xfer1, &msg);
			spi_message_add_tail(&pad_xfer2, &msg);
		} else if (rx_len == tx_data_len) {
			/*
			 * |------------- RX BUFFER + RX CSUM ---------|
			 * |------------------- TX BUFFER -------------|---- TX CSUM ---|
			 *
			 * |             SHARED                        |
			 *                                             |      PAD 1     |
			 */
			pad_xfer1.rx_buf = NULL;
			pad_xfer1.tx_buf = ctx->tx_csum;
			pad_xfer1.len = sizeof(ctx->tx_csum);

			spi_message_add_tail(&pad_xfer1, &msg);
		} else if (rx_len == tx_data_len + 1) {
			/*
			 * |----------------- RX BUFFER + RX CSUM ----------------|
			 * |------------------- TX BUFFER -------------|---- TX CSUM ---|
			 *
			 * |             SHARED                        |
			 *                                             |  PAD 1 |
			 *                                                      | PAD 2 |
			 */
			pad_xfer1.tx_buf = ctx->tx_csum;
			pad_xfer1.rx_buf = skb_put(ctx->rx_skb, 1);
			pad_xfer1.len = 1;

			pad_xfer2.tx_buf = &ctx->tx_csum[1];
			pad_xfer2.rx_buf = NULL;
			pad_xfer2.len = 1;

			spi_message_add_tail(&pad_xfer1, &msg);
			spi_message_add_tail(&pad_xfer2, &msg);
		} else {
			/*
			 * |----------------------------- RX BUFFER + RX CSUM -------------------|
			 * |------------------- TX BUFFER -------------|---- TX CSUM ---|
			 *
			 * |             SHARED                        |
			 *                                             |       PAD 1    |
			 *                                                              |  PAD 2 |
			 */
			pad_xfer1.tx_buf = ctx->tx_csum;
			pad_xfer1.rx_buf = skb_put(ctx->rx_skb, sizeof(ctx->tx_csum));
			pad_xfer1.len = sizeof(ctx->tx_csum);

			pad_xfer2.tx_buf = NULL;
			pad_xfer2.rx_buf = skb_put(ctx->rx_skb, pad_len - sizeof(ctx->tx_csum));
			pad_xfer2.len = pad_len - sizeof(ctx->tx_csum);

			spi_message_add_tail(&pad_xfer1, &msg);
			spi_message_add_tail(&pad_xfer2, &msg);
		}
	}

	ret = spi_sync(ctx->spi, &msg);

	if (ctx->tx_skb) {
		kfree_skb(ctx->tx_skb);
		ctx->tx_skb = NULL;
	}

	if (ctx->rx_skb) {
		unsigned char *csum_ptr;
		u16 expected_csum;
		u16 csum;

		if (ret) {
			kfree_skb(ctx->rx_skb);
			goto exit;
		}

		csum_ptr = skb_tail_pointer(ctx->rx_skb) - sizeof(csum);
		csum = get_unaligned_le16(csum_ptr);

		expected_csum = cpc_spi_csum(ctx->rx_skb->data + CPC_HEADER_SIZE,
					     ctx->rx_len - sizeof(csum));

		if (csum == expected_csum) {
			skb_trim(ctx->rx_skb, ctx->rx_skb->len - sizeof(csum));

			cpc_interface_receive_frame(ctx->intf, ctx->rx_skb);
		} else {
			kfree_skb(ctx->rx_skb);
		}
	}

exit:
	ctx->rx_skb = NULL;
	ctx->rx_len = 0;

	return ret;
}

static int cpc_spi_do_xfer_thread(void *data)
{
	struct cpc_spi *ctx = data;
	bool xfer_idle = true;
	int ret;

	while (!kthread_should_stop()) {
		if (xfer_idle) {
			ret = wait_event_interruptible(ctx->event_queue,
						       (!cpc_interface_tx_queue_empty(ctx->intf) ||
							atomic_read(&ctx->event_cond) == 1 ||
							kthread_should_stop()));

			if (ret)
				continue;

			if (kthread_should_stop())
				return 0;

			if (!ctx->tx_skb)
				ctx->tx_skb = cpc_interface_dequeue(ctx->intf);

			/*
			 * Reset thread event right before transmission to prevent interrupts that
			 * happened while the thread was already awake to wake up the thread again,
			 * as the event is going to be handled by this iteration.
			 */
			atomic_set(&ctx->event_cond, 0);

			ret = cpc_spi_do_xfer_header(ctx);
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

			if (!ctx->tx_skb && !ctx->rx_skb)
				ret = cpc_spi_do_xfer_notch(ctx);
			else
				ret = cpc_spi_do_xfer_payload(ctx);

			if (!ret)
				xfer_idle = true;
		}
	}

	return 0;
}

static irqreturn_t cpc_spi_irq_handler(int irq, void *data)
{
	struct cpc_spi *ctx = data;

	atomic_set(&ctx->event_cond, 1);
	wake_up(&ctx->event_queue);

	return IRQ_HANDLED;
}

static int cpc_spi_ops_wake_tx(struct cpc_interface *intf)
{
	struct cpc_spi *ctx = cpc_interface_get_priv(intf);

	wake_up_interruptible(&ctx->event_queue);

	return 0;
}

static void cpc_spi_ops_csum(struct sk_buff *skb)
{
	skb->csum = cpc_spi_csum(skb->data, skb->len);
}

static const struct cpc_interface_ops spi_intf_cpc_ops = {
	.wake_tx = cpc_spi_ops_wake_tx,
	.csum = cpc_spi_ops_csum,
};

static int cpc_spi_probe(struct spi_device *spi)
{
	struct cpc_interface *intf;
	struct cpc_spi *ctx;
	int err;

	if (!spi->irq) {
		dev_err(&spi->dev, "cannot function without IRQ, please provide one\n");
		return -EINVAL;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	intf = cpc_interface_alloc(&spi->dev, &spi_intf_cpc_ops, ctx);
	if (IS_ERR(intf)) {
		kfree(ctx);

		return PTR_ERR(intf);
	}

	spi_set_drvdata(spi, ctx);

	ctx->spi = spi;
	ctx->intf = intf;

	ctx->tx_skb = NULL;

	atomic_set(&ctx->event_cond, 0);
	ctx->rx_skb = NULL;

	init_waitqueue_head(&ctx->event_queue);

	err = cpc_interface_register(intf);
	if (err)
		goto put_interface;

	err = request_irq(spi->irq, cpc_spi_irq_handler, IRQF_TRIGGER_FALLING,
			  dev_name(&spi->dev), ctx);
	if (err)
		goto unregister_interface;

	ctx->task = kthread_run(cpc_spi_do_xfer_thread, ctx, "%s",
				dev_name(&spi->dev));
	if (IS_ERR(ctx->task)) {
		err = PTR_ERR(ctx->task);
		goto free_irq;
	}

	return 0;

free_irq:
	free_irq(spi->irq, ctx);

unregister_interface:
	cpc_interface_unregister(intf);

put_interface:
	cpc_interface_put(intf);

	kfree(ctx);

	return err;
}

static void cpc_spi_remove(struct spi_device *spi)
{
	struct cpc_spi *ctx = spi_get_drvdata(spi);
	struct cpc_interface *intf = ctx->intf;

	kthread_stop(ctx->task);
	free_irq(spi->irq, ctx);
	cpc_interface_unregister(intf);
	kfree(ctx);
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

static struct spi_driver cpc_spi_driver = {
	.driver = {
		.name = "cpc-spi",
		.of_match_table = cpc_dt_ids,
	},
	.probe = cpc_spi_probe,
	.remove = cpc_spi_remove,
};

/**
 * cpc_spi_register_driver - Register driver to the SPI subsytem.
 *
 * @return: 0 on success, otherwise a negative error code.
 */
int cpc_spi_register_driver(void)
{
	return spi_register_driver(&cpc_spi_driver);
}

/**
 * cpc_spi_unregister_driver - Unregister driver from the SPI subsytem.
 */
void cpc_spi_unregister_driver(void)
{
	spi_unregister_driver(&cpc_spi_driver);
}
