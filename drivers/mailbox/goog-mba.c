// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google MailBox Array (MBA) Driver
 *
 * Copyright (c) 2025 Google LLC
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/goog-mba-message.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "goog-mba-priv.h"

#define CREATE_TRACE_POINTS
#include "goog-mba-trace.h"

#define CLIENT_IRQ_TRIG_OFFSET		0x0
#define SET_HOST_IRQ			0x1

#define CLIENT_IRQ_CONFIG_OFFSET	0x4
#define ENABLE_HOST_AUTO_ACK		BIT(8)
#define CLIENT_IRQ_MASK_MSG_INT		BIT(16)
#define CLIENT_IRQ_MASK_ACK_INT		BIT(24)

#define CLIENT_IRQ_STATUS_OFFSET	0x8
#define CLIENT_IRQ_STATUS_MSG_INT	0x1
#define CLIENT_IRQ_STATUS_ACK_INT	0x100

#define CLIENT_MBA_IP_VER		0x30
#define CLIENT_NUM_MSG_REG		0x34
#define CLIENT_OUTSTANDING_MSG		0x10

#define GLOBAL_NUM_MSG_REG_OFFSET(x)	(0x10 + ((x) * 4))
#define GLOBAL_MBA_IP_VER_OFFSET	4

#define CLIENT_CLIENT_DOORBELL_TRIG	0x20
#define CLIENT_DOORBELL_MASK_OFFSET	0x24
#define CLIENT_DOORBELL_STATUS_OFFSET	0x28
#define CLIENT_HOST_DOORBELL_OFFSET	0x38
#define CLIENT_CLIENT_DOORBELL_OFFSET	0x3c

#define MAX_MBA_CHANNELS		1
#define NR_PHANDLE_ARG_COUNT		1

#define MSG_OFFSET(i)			(0x100 + (i) * sizeof(u32))

#define MBA_IP_MAJOR_VER_1		0x1
#define MBA_IP_MAJOR_VER_3		0x3

#define MBA_IP_MAJOR_VER_SHIFT		24
#define MBA_IP_MAJOR_VER_MASK		0xff
#define MBA_IP_MINOR_VER_SHIFT		16
#define MBA_IP_MINOR_VER_MASK		0xff
#define MBA_IP_INCREMENTAL_VER_SHIFT	0
#define MBA_IP_INCREMENTAL_VER_MASK	0xffff

/**
 * goog_mba_handle_tx_interrupt() - Handle interrupt that remote Acked our msg.
 * @goog_mbox: The mailbox info.
 */
static void goog_mba_handle_tx_interrupt(struct goog_mbox_info *goog_mbox)
{
	unsigned int reqs_completed;
	int i;

	/*
	 * ACK interrupt needs to be cleared before reading CLIENT_OUTSTANDING_MSG.
	 * Then if a "race" happens and another message gets Acked after we clear
	 * but before we read CLIENT_OUTSTANDING_MSG then the worst that will
	 * happen is we'll get a followup interrupt that will show 0 reqs_completed.
	 */
	writel(CLIENT_IRQ_STATUS_ACK_INT, goog_mbox->iomem + CLIENT_IRQ_STATUS_OFFSET);

	if (goog_mbox->queue_mode) {
		u32 outstanding_msgs;

		outstanding_msgs = readl(goog_mbox->iomem + CLIENT_OUTSTANDING_MSG);
		spin_lock(&goog_mbox->lock);

		if (goog_mbox->outstanding_msgs >= outstanding_msgs) {
			reqs_completed = goog_mbox->outstanding_msgs - outstanding_msgs;
		} else {
			/*
			 * The hardware's track of outstanding messages should always
			 * be less than or equal to the number of messages we queued.
			 * If it thinks there are more messages outstanding than we
			 * queued, something is wrong. Assume nothing was completed.
			 */
			dev_warn_ratelimited(goog_mbox->mba->dev,
					     "%pOFP: unexpected outstanding msgs: %u -> %u\n",
					     goog_mbox->np, goog_mbox->outstanding_msgs,
					     outstanding_msgs);
			reqs_completed = 0;
		}
		goog_mbox->outstanding_msgs = outstanding_msgs;
		spin_unlock(&goog_mbox->lock);

		trace_goog_mba_process_q_txdone(goog_mbox, reqs_completed, outstanding_msgs);
	} else {
		reqs_completed = 1;
		trace_goog_mba_process_nq_txdone(goog_mbox);
	}

	for (i = 0; i < reqs_completed; i++)
		mbox_chan_txdone(&goog_mbox->chan, 0);
}

/**
 * goog_mba_handle_rx_interrupt() - Handle interrupt that remote send a msg.
 * @goog_mbox: The mailbox info.
 */
static void goog_mba_handle_rx_interrupt(struct goog_mbox_info *goog_mbox)
{
	struct goog_mba_rx_msg msg = {
		.payload = goog_mbox->rx_buffer,
		.payload_words = goog_mbox->rx_payload_words,
	};
	int i;

	for (i = 0; i < goog_mbox->rx_payload_words; i++)
		goog_mbox->rx_buffer[i] = readl(goog_mbox->iomem +
						MSG_OFFSET(goog_mbox->rx_idx + i));

	if (goog_mbox->queue_mode) {
		goog_mbox->rx_idx = (goog_mbox->rx_idx + goog_mbox->rx_payload_words) %
				    goog_mbox->msg_buffer_words;
		trace_goog_mba_process_q_rx(goog_mbox);
	} else {
		trace_goog_mba_process_nq_rx(goog_mbox);
	}

	/*
	 * Ack the interrupt after we've read the message to local memory but
	 * before passing it to the client. This frees up space for the remote
	 * processor to send another message as the client is processing this one.
	 */
	writel(CLIENT_IRQ_STATUS_MSG_INT, goog_mbox->iomem + CLIENT_IRQ_STATUS_OFFSET);

	mbox_chan_received_data(&goog_mbox->chan, &msg);
}

/**
 * goog_mba_isr() - Main interrupt routine
 * @irq: The IRQ number.
 * @data: Data passed when registering (the goog_mbox_info for this IRQ).
 *
 * Return: IRQ_HANDLED if IRQ was handled; IRQ_NONE if no interrupt was found.
 */
static irqreturn_t goog_mba_isr(int irq, void *data)
{
	struct goog_mbox_info *goog_mbox = data;
	u32 irq_status;

	irq_status = readl(goog_mbox->iomem + CLIENT_IRQ_STATUS_OFFSET);
	if (!irq_status)
		return IRQ_NONE;

	while (true) {
		if (irq_status & CLIENT_IRQ_STATUS_ACK_INT)
			goog_mba_handle_tx_interrupt(goog_mbox);

		if (irq_status & CLIENT_IRQ_STATUS_MSG_INT) {
			goog_mba_handle_rx_interrupt(goog_mbox);

			/*
			 * In queue mode the RX interrupt will re-assert itself
			 * right after we clear it if there is more than one
			 * message waiting. As an optimization to avoid returning
			 * and immediately re-triggering our interrupt, re-check.
			 *
			 * NOTE: we don't need to loop for the TX (Ack) case
			 * since TX interrupts aren't counted in the same way.
			 */
			if (goog_mbox->queue_mode)
				irq_status = readl(goog_mbox->iomem + CLIENT_IRQ_STATUS_OFFSET);
			else
				break;
		} else {
			break;
		}
	}

	return IRQ_HANDLED;
}

/**
 * goog_mba_send_data() - Mailbox op for send_data.
 * @chan: The Linux mbox_chan structure associated with the mailbox.
 * @msg: The mailbox message, expected to be of type `struct goog_mba_tx_msg`.
 *	 If NULL, we'll assume a 0-byte doorbell-only message.
 *
 * Return: 0 if the data was sent; -EBUSY if the queue was full; other
 *	   negative error values for other problems.
 */
static int goog_mba_send_data(struct mbox_chan *chan, void *msg)
{
	struct goog_mbox_info *goog_mbox = chan->con_priv;
	struct device *dev = goog_mbox->mba->dev;
	bool queue_mode = goog_mbox->queue_mode;
	unsigned int payload_words = 0;
	const u32 *payload = NULL;
	bool is_init = false;
	unsigned int tx_idx;
	unsigned int i;
	unsigned long flags;

	if (msg) {
		struct goog_mba_tx_msg *mba_msg = msg;

		payload_words = mba_msg->payload_words;
		payload = mba_msg->payload;
		is_init = mba_msg->init;
	}

	if (payload_words > goog_mbox->msg_buffer_words) {
		dev_err(dev, "%pOFP: Payload too big: %u > %u\n",
			goog_mbox->np, payload_words, goog_mbox->msg_buffer_words);
		return -EINVAL;
	}

	/*
	 * Currently all queue-mode transfers need to be the same size and
	 * need to evenly divide the message buffer. If this is the first
	 * transfer, we need to validate/store the payload_words. For subsequent
	 * transfers we just need to confirm it hasn't changed.
	 */
	if (queue_mode) {
		if (!goog_mbox->tx_payload_words && payload_words) {
			if (goog_mbox->msg_buffer_words % payload_words != 0) {
				dev_err(dev, "%pOFP: Invalid initial TX queue payload words: %u\n",
					goog_mbox->np, payload_words);
				return -EINVAL;
			}
			goog_mbox->tx_payload_words = payload_words;
		} else if (payload_words != goog_mbox->tx_payload_words) {
			dev_err(dev, "%pOFP: Payload size mismatch: %u vs %u\n",
				goog_mbox->np, payload_words, goog_mbox->tx_payload_words);
			return -EINVAL;
		}
	}

	/*
	 * HW can handle full duplex but there is no current scheme for dividing
	 * up the message buffer between TX and RX portions. Non-queue mode
	 * could work half-duplex, but that doesn't make sense in queue mode.
	 * If space is reserved for RX then disallow using it for TX.
	 */
	if (queue_mode && payload_words && goog_mbox->rx_payload_words) {
		dev_err(dev, "%pOFP: Can't TX if buffer is used for RX\n", goog_mbox->np);
		return -EINVAL;
	}

	if (queue_mode) {
		bool out_of_space;

		spin_lock_irqsave(&goog_mbox->lock, flags);
		out_of_space = payload_words &&
			       (goog_mbox->outstanding_msgs >=
				goog_mbox->msg_buffer_words / payload_words);
		spin_unlock_irqrestore(&goog_mbox->lock, flags);

		/*
		 * IMPORTANT: don't print an error for this. It's normal and
		 * expected that the core will keep trying to queue messages
		 * until the controller reports -EBUSY.
		 */
		if (out_of_space)
			return -EBUSY;

		tx_idx = goog_mbox->tx_idx;
		trace_goog_mba_send_data_q(goog_mbox, payload, payload_words);
	} else {
		tx_idx = 0;
		trace_goog_mba_send_data_nq(goog_mbox, payload, payload_words);
	}

	for (i = 0; i < payload_words; i++)
		writel(payload[i], goog_mbox->iomem + MSG_OFFSET(tx_idx + i));

	if (queue_mode) {
		if (is_init)
			goog_mbox->tx_idx = 0;
		else
			goog_mbox->tx_idx = (goog_mbox->tx_idx + payload_words) %
					    goog_mbox->msg_buffer_words;
	}

	/*
	 * We need to be careful on how we deal with `outstanding_msgs`.
	 * The hardware will give us an interrupt every time it transfers
	 * a message, but if it transfers two messages before our interrupt
	 * handler fires then we'll still only get one interrupt. We have
	 * to look at the difference between the hardware's idea of
	 * `outstanding_msgs` and ours to figure out how many messages were
	 * actually transferred.
	 *
	 * We need to start the transfer and increment our concept of
	 * `outstanding_msgs` in lockstep so protect against the interrupt
	 * handler also touching `outstanding_msgs`.
	 */
	if (queue_mode)
		spin_lock_irqsave(&goog_mbox->lock, flags);

	writel(SET_HOST_IRQ, goog_mbox->iomem + CLIENT_IRQ_TRIG_OFFSET);

	if (queue_mode) {
		goog_mbox->outstanding_msgs++;
		spin_unlock_irqrestore(&goog_mbox->lock, flags);
	}

	return 0;
}

/**
 * goog_mba_startup() - Mailbox op for startup.
 * @chan: The Linux mbox_chan structure associated with the mailbox.
 *
 * Return: 0 for OK; negative error code if problems.
 */
static int goog_mba_startup(struct mbox_chan *chan)
{
	struct goog_mbox_info *goog_mbox = chan->con_priv;

	writel(ENABLE_HOST_AUTO_ACK | CLIENT_IRQ_MASK_MSG_INT | CLIENT_IRQ_MASK_ACK_INT,
	       goog_mbox->iomem + CLIENT_IRQ_CONFIG_OFFSET);
	enable_irq(goog_mbox->irq);

	return 0;
}

/**
 * goog_mba_shutdown() - Mailbox op for shutdown.
 * @chan: The Linux mbox_chan structure associated with the mailbox.
 */
static void goog_mba_shutdown(struct mbox_chan *chan)
{
	struct goog_mbox_info *goog_mbox = chan->con_priv;

	disable_irq(goog_mbox->irq);
	writel(0x0, goog_mbox->iomem + CLIENT_IRQ_CONFIG_OFFSET);
}

static const struct mbox_chan_ops goog_mba_chan_ops = {
	.send_data = goog_mba_send_data,
	.startup = goog_mba_startup,
	.shutdown = goog_mba_shutdown,
};

/**
 * goog_mba_get_msg_buf_words() - Return # msg buffer words in HW for this mailbox.
 * @goog_mbox: The mailbox info.
 * @np: The device tree node associated with this mailbox.
 *
 * Return: The number of 32-bit words in the hardware message buffer.
 */
static unsigned int goog_mba_get_msg_buf_words(struct goog_mbox_info *goog_mbox,
					       struct device_node *np)
{
	struct goog_mba_info *mba = goog_mbox->mba;
	struct resource res;
	unsigned int index;

	/*
	 * On newer IP there's no global space and the register moved to the
	 * client address space.
	 */
	if (!mba->global_iomem)
		return readl(goog_mbox->iomem + CLIENT_NUM_MSG_REG);

	/*
	 * On older IP we need to find the index so we can look up the value
	 * in the global memory. Our index is based on the physical address
	 * of the mbox since on older hardware mailboxes are 4K apart.
	 */
	if (of_address_to_resource(np, 0, &res)) {
		dev_err(mba->dev, "%pOFP: Failed to find physical address\n", np);
		return 0;
	}
	index = (res.start & 0x1ffff) / 0x1000;

	return readl(mba->global_iomem + GLOBAL_NUM_MSG_REG_OFFSET(index));
}

/**
 * of_node_put_void() - of_node_put() for passing to devm_add_action_or_reset().
 * @data: Our struct device_node pointer.
 */
static void of_node_put_void(void *data)
{
	of_node_put(data);
}

/**
 * goog_mba_mbox_init() - Initialize one single mailbox.
 * @goog_mbox: The mailbox info.
 * @mba: The array containing this mailbox.
 * @np: The device tree node associated with this mailbox.
 *
 * Return: 0 or a negative error code. If an error is returned, the error has
 *	   already been logged.
 */
static int goog_mba_mbox_init(struct goog_mbox_info *goog_mbox,
			      struct goog_mba_info *mba, struct device_node *np)
{
	struct mbox_controller *mbox = &goog_mbox->mbox;
	struct mbox_chan *chan = &goog_mbox->chan;
	struct device *dev = mba->dev;
	unsigned int msg_buffer_words;
	u32 rx_payload_words;
	int ret;

	goog_mbox->mba = mba;

	of_node_get(np);
	ret = devm_add_action_or_reset(dev, of_node_put_void, np);
	if (ret)
		return ret;
	goog_mbox->np = np;

	goog_mbox->iomem = devm_of_iomap(dev, np, 0, NULL);
	if (IS_ERR(goog_mbox->iomem))
		return dev_err_probe(dev, PTR_ERR(goog_mbox->iomem),
				     "%pOFP: Failed to map memory\n", np);

	/* Start with all interrupts disabled and cleared */
	writel(0x0, goog_mbox->iomem + CLIENT_IRQ_CONFIG_OFFSET);
	writel(CLIENT_IRQ_STATUS_MSG_INT | CLIENT_IRQ_STATUS_ACK_INT,
	       goog_mbox->iomem + CLIENT_IRQ_STATUS_OFFSET);

	goog_mbox->irq = of_irq_get(np, 0);
	if (goog_mbox->irq <= 0) {
		if (goog_mbox->irq == 0)
			goog_mbox->irq = -EINVAL;
		return dev_err_probe(dev, goog_mbox->irq, "%pOFP: Failed to get IRQ\n", np);
	}
	ret = devm_request_irq(dev, goog_mbox->irq, goog_mba_isr,
			       IRQF_NO_SUSPEND | IRQF_NO_AUTOEN,
			       NULL, goog_mbox);
	if (ret)
		return dev_err_probe(dev, ret, "%pOFP: Failed to request interrupt\n", np);

	goog_mbox->queue_mode = of_property_read_bool(np, "google,mba-queue-mode");

	rx_payload_words = 0;
	ret = of_property_read_u32(np, "google,rx-payload-words", &rx_payload_words);
	goog_mbox->rx_payload_words = rx_payload_words;

	msg_buffer_words = goog_mba_get_msg_buf_words(goog_mbox, np);
	goog_mbox->msg_buffer_words = msg_buffer_words;

	if (goog_mbox->queue_mode && !msg_buffer_words)
		return dev_err_probe(dev, -EINVAL,
				     "%pOFP: Queue mode requires non-zero buffer\n", np);

	if (msg_buffer_words < rx_payload_words)
		return dev_err_probe(dev, -EINVAL,
				     "%pOFP: Buffer (%u) < RX Payload (%u)\n",
				     np, msg_buffer_words, rx_payload_words);
	if (goog_mbox->queue_mode && rx_payload_words && msg_buffer_words % rx_payload_words != 0)
		return dev_err_probe(dev, -EINVAL,
				     "%pOFP: Queue buffer (%u) must be multiple of RX payload (%u)\n",
				     np, msg_buffer_words, rx_payload_words);

	if (rx_payload_words) {
		goog_mbox->rx_buffer = devm_kcalloc(dev, rx_payload_words,
						    sizeof(*goog_mbox->rx_buffer), GFP_KERNEL);
		if (!goog_mbox->rx_buffer)
			return -ENOMEM;
	}

	mbox->fwnode = of_fwnode_handle(np);
	mbox->dev = dev;
	mbox->ops = &goog_mba_chan_ops;
	mbox->chans = chan;
	mbox->num_chans = 1;
	mbox->txdone_irq = true;
	mbox->has_queue = goog_mbox->queue_mode;
	spin_lock_init(&goog_mbox->lock);

	chan->con_priv = goog_mbox;

	ret = devm_mbox_controller_register(dev, mbox);
	if (ret)
		return dev_err_probe(dev, ret,
				     "%pOFP: Failed to register mailbox controller\n", np);

	return 0;
}

/**
 * goog_mba_probe() - MBA probe routine.
 * @pdev: Our platform device.
 *
 * Return: 0 or a negative error code.
 */
static int goog_mba_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct goog_mbox_info *goog_mboxes;
	struct goog_mba_info *mba;
	unsigned int num_mboxes;
	unsigned int i;
	int ret;

	mba = devm_kzalloc(dev, sizeof(*mba), GFP_KERNEL);
	if (!mba)
		return -ENOMEM;
	mba->dev = dev;

	num_mboxes = of_get_available_child_count(dev->of_node);
	goog_mboxes = devm_kzalloc(dev, sizeof(*goog_mboxes) * num_mboxes, GFP_KERNEL);
	if (!goog_mboxes)
		return -ENOMEM;

	/*
	 * The first memory range is the memory range that the other side
	 * of the mailbox uses. The second memory range is the shared/global
	 * range. Note that newer versions of the IP don't have the global
	 * range, so mba->global_iomem is left NULL on newer IP.
	 */
	if (platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		mba->global_iomem = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(mba->global_iomem))
			return dev_err_probe(dev, PTR_ERR(mba->global_iomem),
					     "Failed to iomap global region\n");
	}

	i = 0;
	for_each_available_child_of_node_scoped(dev->of_node, child_np) {
		ret = goog_mba_mbox_init(&goog_mboxes[i], mba, child_np);
		if (ret)
			return ret;
		i++;
	}

	return 0;
}

static const struct of_device_id goog_mba_match[] = {
	{ .compatible = "google,mailbox-array" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, goog_mba_match);

static struct platform_driver mba = {
	.driver = {
		.name = "goog-mba",
		.of_match_table = goog_mba_match,
	},
	.probe = goog_mba_probe,
};

module_platform_driver(mba);

MODULE_DESCRIPTION("Google MailBox Array (MBA) Driver");
MODULE_AUTHOR("Douglas Anderson <dianders@chromium.org>");
MODULE_LICENSE("GPL");
