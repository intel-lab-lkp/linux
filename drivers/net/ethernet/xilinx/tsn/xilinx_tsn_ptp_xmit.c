// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xilinx FPGA Xilinx TSN PTP transfer protocol module.
 *
 */

#include "xilinx_tsn.h"

/**
 * ptp_iow - write to PTP register
 * @emac: Pointer to TSN EMAC structure
 * @off: Register offset
 * @val: Value to write
 *
 * This function writes to PTP control registers.
 */
static inline void ptp_iow(struct tsn_emac *emac, off_t off, u32 val)
{
	iowrite32(val, emac->regs + off);
}

/**
 * ptp_ior - read from PTP register
 * @emac: Pointer to TSN EMAC structure
 * @off: Register offset
 *
 * Return: Register value
 *
 * This function reads from PTP control registers.
 */
static inline u32 ptp_ior(struct tsn_emac *emac, u32 off)
{
	return ioread32(emac->regs + off);
}

/**
 * memcpy_fromio_32 - copy ptp buffer from HW
 * @emac: Pointer to TSN EMAC structure
 * @offset: Offset in the PTP buffer
 * @data: Destination buffer
 * @len: Length to copy
 *
 * This functions copies the data from PTP buffer to destination data buffer
 */
static void memcpy_fromio_32(struct tsn_emac *emac,
			     unsigned long offset, u8 *data, size_t len)
{
	while (len >= 4) {
		*(u32 *)data = ptp_ior(emac, offset);
		len -= 4;
		offset += 4;
		data += 4;
	}

	if (len > 0) {
		u32 leftover = ptp_ior(emac, offset);
		u8 *src = (u8 *)&leftover;

		while (len) {
			*data++ = *src++;
			len--;
		}
	}
}

/**
 * memcpy_toio_32 - copy ptp buffer to HW
 * @emac: Pointer to TSN EMAC structure
 * @offset: Offset in the PTP buffer
 * @data: Source data
 * @len: Length to copy
 *
 * This functions copies the source data to destination ptp buffer
 */
static void memcpy_toio_32(struct tsn_emac *emac,
			   unsigned long offset, u8 *data, size_t len)
{
	while (len >= 4) {
		ptp_iow(emac, offset, *(u32 *)data);
		len -= 4;
		offset += 4;
		data += 4;
	}

	if (len > 0) {
		u32 leftover = 0;
		u8 *dest = (u8 *)&leftover;

		while (len) {
			*dest++ = *data++;
			len--;
		}
		ptp_iow(emac, offset, leftover);
	}
}

/**
 * tsn_ptp_xmit - xmit skb using PTP HW
 * @skb: sk_buff pointer that contains data to be Txed.
 * @emac: Pointer to TSN EMAC structure.
 *
 * Return: NETDEV_TX_OK, on success
 *         NETDEV_TX_BUSY, if any of the descriptors are not free
 *
 * This function is called to transmit a PTP skb. The function uses
 * the free PTP TX buffer entry and sends the frame
 */
int tsn_ptp_xmit(struct sk_buff *skb, struct tsn_emac *emac)
{
	u16 queue = skb_get_queue_mapping(skb);
	u8 tx_frame_waiting;
	u32 cmd1_field = 0;
	u32 cmd2_field = 0;
	u8 free_index;

	tx_frame_waiting = (ptp_ior(emac, PTP_TX_CONTROL_OFFSET) &
			    PTP_TX_FRAME_WAITING_MASK) >>
			    PTP_TX_FRAME_WAITING_SHIFT;

	/* we reached last frame */
	if (tx_frame_waiting & (1 << 7)) {
		netif_stop_subqueue(emac->ndev, queue);
		emac->ndev->stats.tx_dropped++;
		netdev_dbg(emac->ndev, "PTP TX buffers full: 0x%x\n", tx_frame_waiting);
		return NETDEV_TX_BUSY;
	}

	/* go to next available slot */
	free_index  = fls(tx_frame_waiting);

	cmd1_field |= skb->len;

	ptp_iow(emac, PTP_TX_BUFFER_OFFSET(free_index), cmd1_field);
	ptp_iow(emac, PTP_TX_BUFFER_OFFSET(free_index) +
		PTP_TX_BUFFER_CMD2_FIELD, cmd2_field);
	memcpy_toio_32(emac,
		       (PTP_TX_BUFFER_OFFSET(free_index) +
			PTP_TX_CMD_FIELD_LEN),
		       skb->data, skb->len);

	/* send the frame */
	ptp_iow(emac, PTP_TX_CONTROL_OFFSET, (1 << free_index));

	scoped_guard(spinlock_irq, &emac->ptp_tx_lock) {
		skb->cb[0] = free_index;
		skb_queue_tail(&emac->ptp_txq, skb);

		if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)
			skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	}
	skb_tx_timestamp(skb);

	return NETDEV_TX_OK;
}

/**
 * tsn_set_timestamp - timestamp skb with HW timestamp
 * @emac: Pointer to TSN EMAC structure
 * @hwtstamps: Pointer to skb timestamp structure
 * @offset: offset of the timestamp in the PTP buffer
 *
 * Return:	None.
 *
 */
static void tsn_set_timestamp(struct tsn_emac *emac,
			      struct skb_shared_hwtstamps *hwtstamps,
			      unsigned int offset)
{
	u32 captured_ns;
	u32 captured_sec;

	captured_ns = ptp_ior(emac, offset + 4);
	captured_sec = ptp_ior(emac, offset);

	hwtstamps->hwtstamp = ktime_set(captured_sec, captured_ns);
}

/**
 * tsn_ptp_recv - receive ptp buffer in skb from HW
 * @ndev: Pointer to net_device structure.
 *
 * This function is called from the ptp rx isr. It allocates skb, and
 * copies the ptp rx buffer data to it and calls netif_rx for further
 * processing.
 *
 */
static void tsn_ptp_recv(struct net_device *ndev)
{
	struct tsn_emac *emac = netdev_priv(ndev);
	unsigned long ptp_frame_base_addr = 0;
	struct sk_buff *skb;
	u16 msg_len;
	u8 msg_type;
	u32 bytes = 0;
	u32 packets = 0;

	if (!ndev || !netif_running(ndev))
		return;

	while (((emac->ptp_rx_hw_pointer & 0xf) !=
		 (emac->ptp_rx_sw_pointer & 0xf))) {
		skb = netdev_alloc_skb(ndev, PTP_RX_FRAME_SIZE);
		if (!skb) {
			ndev->stats.rx_dropped++;
			emac->ptp_rx_sw_pointer += 1;
			continue;
		}
		emac->ptp_rx_sw_pointer += 1;

		ptp_frame_base_addr = PTP_RX_BASE_OFFSET +
				   ((emac->ptp_rx_sw_pointer & 0xf) *
				    PTP_RX_HWBUF_SIZE);

		memcpy_fromio_32(emac, ptp_frame_base_addr, skb->data,
				 PTP_RX_FRAME_SIZE);

		msg_type  = *(u8 *)(skb->data + ETH_HLEN) & 0xf;
		msg_len  = *(u16 *)(skb->data + ETH_HLEN + 2);

		skb_put(skb, ntohs(msg_len) + ETH_HLEN);

		bytes += skb->len;
		packets++;

		skb->protocol = eth_type_trans(skb, ndev);
		skb->ip_summed = CHECKSUM_UNNECESSARY;

		/* timestamp only event messages */
		if (!(msg_type & PTP_MSG_TYPE_MASK)) {
			tsn_set_timestamp(emac, skb_hwtstamps(skb),
					  (ptp_frame_base_addr +
					   PTP_HW_TSTAMP_OFFSET));
		}

		netif_rx(skb);
	}
	ndev->stats.rx_packets += packets;
	ndev->stats.rx_bytes += bytes;
}

/**
 * tsn_ptp_rx_irq - PTP RX ISR handler
 * @irq: irq number
 * @data: net_device pointer
 *
 * Return:	IRQ_HANDLED for all cases.
 */
irqreturn_t tsn_ptp_rx_irq(int irq, void *data)
{
	struct tsn_emac *emac = data;

	emac->ptp_rx_hw_pointer = (ptp_ior(emac, PTP_RX_CONTROL_OFFSET)
					& PTP_RX_PACKET_FIELD_MASK)  >> 8;

	tsn_ptp_recv(emac->ndev);

	return IRQ_HANDLED;
}

/**
 * tsn_ptp_tx_tstamp - timestamp skb on transmit path
 * @work: Pointer to work_struct structure
 *
 * This adds TX timestamp to skb
 */
void tsn_ptp_tx_tstamp(struct work_struct *work)
{
	struct tsn_emac *emac = container_of(work, struct tsn_emac,
			tx_tstamp_work);
	struct net_device *ndev = emac->ndev;
	struct skb_shared_hwtstamps hwtstamps;
	struct sk_buff *skb;
	unsigned long ts_reg_offset;
	unsigned long flags;
	u8 tx_packet;
	u8 index;
	u32 bytes = 0;
	u32 packets = 0;

	memset(&hwtstamps, 0, sizeof(struct skb_shared_hwtstamps));

	spin_lock_irqsave(&emac->ptp_tx_lock, flags);

	tx_packet =  (ptp_ior(emac, PTP_TX_CONTROL_OFFSET) &
				PTP_TX_PACKET_FIELD_MASK) >>
				PTP_TX_PACKET_FIELD_SHIFT;

	while ((skb = __skb_dequeue(&emac->ptp_txq)) != NULL) {
		index = skb->cb[0];

		/* dequeued packet yet to be xmited? */
		if (index > tx_packet) {
			/* enqueue it back and break */
			skb_queue_tail(&emac->ptp_txq, skb);
			break;
		}
		/* time stamp reg offset */
		ts_reg_offset = PTP_TX_BUFFER_OFFSET(index) +
					PTP_HW_TSTAMP_OFFSET;

		if (skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS) {
			tsn_set_timestamp(emac, &hwtstamps, ts_reg_offset);
			skb_tstamp_tx(skb, &hwtstamps);
		}

		bytes += skb->len;
		packets++;
		dev_kfree_skb_any(skb);
	}
	ndev->stats.tx_packets += packets;
	ndev->stats.tx_bytes += bytes;

	spin_unlock_irqrestore(&emac->ptp_tx_lock, flags);
}

/**
 * tsn_ptp_tx_irq - PTP TX irq handler
 * @irq: irq number
 * @data: net_device pointer
 *
 * Return: IRQ_HANDLED for all cases.
 *
 */
irqreturn_t tsn_ptp_tx_irq(int irq, void *data)
{
	struct tsn_emac *emac = data;

	if (!emac || !emac->ndev || !emac->common)
		return IRQ_HANDLED;

	/* read ctrl register to clear the interrupt */
	ptp_ior(emac, PTP_TX_CONTROL_OFFSET);

	schedule_work(&emac->tx_tstamp_work);
	if (__netif_subqueue_stopped(emac->ndev, emac->common->num_priorities))
		netif_wake_subqueue(emac->ndev, emac->common->num_priorities);

	return IRQ_HANDLED;
}

/**
 * tsn_ptp_get_irq_info - Get PTP interrupt information from device tree
 * @emac: Pointer to TSN EMAC structure
 * @emac_np: Device tree node for EMAC
 *
 * Return: 0 on success, negative error code on failure
 *
 * This function retrieves PTP RX and TX interrupt numbers from device tree.
 */
int tsn_ptp_get_irq_info(struct tsn_emac *emac, struct device_node *emac_np)
{
	struct device *dev = emac->common->dev;

	emac->ptp_rx_irq = of_irq_get_byname(emac_np, "interrupt_ptp_rx");
	if (emac->ptp_rx_irq < 0) {
		dev_err(dev,
			"EMAC %d: Failed to get mandatory 'interrupt_ptp_rx': %d\n",
			emac->emac_num, emac->ptp_rx_irq);
		return emac->ptp_rx_irq;
	}

	emac->ptp_tx_irq = of_irq_get_byname(emac_np, "interrupt_ptp_tx");
	if (emac->ptp_tx_irq < 0) {
		dev_err(dev,
			"EMAC %d: Failed to get mandatory 'interrupt_ptp_tx': %d\n",
			emac->emac_num, emac->ptp_tx_irq);
		return emac->ptp_tx_irq;
	}

	dev_info(dev, "EMAC %d: PTP IRQs - RX: %d, TX: %d\n",
		 emac->emac_num, emac->ptp_rx_irq, emac->ptp_tx_irq);

	return 0;
}

/**
 * tsn_ptp_init_and_register_irqs - Initialize PTP subsystem and register interrupts
 * @emac: Pointer to TSN EMAC structure
 *
 * Return: 0 on success, negative error code on failure
 *
 * This function initializes the PTP packet handling subsystem and registers
 * interrupt handlers for PTP RX and TX events.
 */
int tsn_ptp_init_and_register_irqs(struct tsn_emac *emac)
{
	struct device *dev = emac->common->dev;
	int ret;

	/* Initialize PTP TX queue and lock */
	skb_queue_head_init(&emac->ptp_txq);
	spin_lock_init(&emac->ptp_tx_lock);
	INIT_WORK(&emac->tx_tstamp_work, tsn_ptp_tx_tstamp);

	/* Initialize PTP RX pointers */
	emac->current_rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
	emac->ptp_ts_type = HWTSTAMP_TX_ON;
	emac->ptp_rx_hw_pointer = 0;
	emac->ptp_rx_sw_pointer = 0xff;

	/* Clear PTP RX control register */
	ptp_iow(emac, PTP_RX_CONTROL_OFFSET, PTP_RX_PACKET_CLEAR);

	/* Register PTP RX interrupt */
	ret = request_irq(emac->ptp_rx_irq, tsn_ptp_rx_irq, 0,
			  "tsn_ptp_rx", emac);
	if (ret) {
		dev_err(dev, "EMAC %d: Failed to register PTP RX IRQ %d: %d\n",
			emac->emac_num, emac->ptp_rx_irq, ret);
		return ret;
	}

	/* Register PTP TX interrupt */
	ret = request_irq(emac->ptp_tx_irq, tsn_ptp_tx_irq, 0,
			  "tsn_ptp_tx", emac);
	if (ret) {
		dev_err(dev, "EMAC %d: Failed to register PTP TX IRQ %d: %d\n",
			emac->emac_num, emac->ptp_tx_irq, ret);
		free_irq(emac->ptp_rx_irq, emac);
		return ret;
	}

	dev_info(dev, "EMAC %d: PTP interrupts registered\n", emac->emac_num);
	return 0;
}

/**
 * tsn_ptp_unregister_irqs - Unregister PTP interrupts
 * @emac: Pointer to TSN EMAC structure
 *
 * This function unregisters PTP RX and TX interrupt handlers and cleans up
 * PTP TX queue. Called during interface close.
 */
void tsn_ptp_unregister_irqs(struct tsn_emac *emac)
{
	struct sk_buff *skb;

	if (emac->ptp_tx_irq > 0)
		free_irq(emac->ptp_tx_irq, emac);

	if (emac->ptp_rx_irq > 0)
		free_irq(emac->ptp_rx_irq, emac);

	cancel_work_sync(&emac->tx_tstamp_work);

	while ((skb = skb_dequeue(&emac->ptp_txq)) != NULL)
		dev_kfree_skb_any(skb);

	dev_info(emac->common->dev, "EMAC %d: PTP interrupts unregistered\n",
		 emac->emac_num);
}
