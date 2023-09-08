// SPDX-License-Identifier: GPL-2.0+
/*
 * OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface framework
 *
 * Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#include <linux/etherdevice.h>
#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/oa_tc6.h>

static int oa_tc6_spi_transfer(struct spi_device *spi, u8 *ptx, u8 *prx,
			       u16 len)
{
	struct spi_transfer xfer = {
		.tx_buf = ptx,
		.rx_buf = prx,
		.len = len,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(spi, &msg);
}

static bool oa_tc6_get_parity(u32 p)
{
	bool parity = true;

	/* This function returns an odd parity bit */
	while (p) {
		parity = !parity;
		p = p & (p - 1);
	}
	return parity;
}

static void oa_tc6_prepare_ctrl_buf(struct oa_tc6 *tc6, u32 addr, u32 val[],
				    u8 len, bool wnr, u8 *buf, bool ctrl_prot)
{
	u32 hdr;

	/* Prepare the control header with the required details */
	hdr = FIELD_PREP(CTRL_HDR_DNC, 0) |
	      FIELD_PREP(CTRL_HDR_WNR, wnr) |
	      FIELD_PREP(CTRL_HDR_AID, 0) |
	      FIELD_PREP(CTRL_HDR_MMS, addr >> 16) |
	      FIELD_PREP(CTRL_HDR_ADDR, addr) |
	      FIELD_PREP(CTRL_HDR_LEN, len - 1);
	hdr |= FIELD_PREP(CTRL_HDR_P, oa_tc6_get_parity(hdr));
	*(u32 *)buf = cpu_to_be32(hdr);

	if (wnr) {
		for (u8 i = 0; i < len; i++) {
			u16 pos;

			if (!ctrl_prot) {
				/* Send the value to be written followed by the
				 * header.
				 */
				pos = (i + 1) * TC6_HDR_SIZE;
				*(u32 *)&buf[pos] = cpu_to_be32(val[i]);
			} else {
				/* If protected then send complemented value
				 * also followed by actual value.
				 */
				pos = TC6_HDR_SIZE + (i * (TC6_HDR_SIZE * 2));
				*(u32 *)&buf[pos] = cpu_to_be32(val[i]);
				pos = (i + 1) * (TC6_HDR_SIZE * 2);
				*(u32 *)&buf[pos] = cpu_to_be32(~val[i]);
			}
		}
	}
}

static int oa_tc6_check_control(struct oa_tc6 *tc6, u8 *ptx, u8 *prx, u8 len,
				bool wnr, bool ctrl_prot)
{
	/* 1st 4 bytes of rx chunk data can be discarded */
	u32 rx_hdr = *(u32 *)&prx[TC6_HDR_SIZE];
	u32 tx_hdr = *(u32 *)ptx;
	u32 rx_data_complement;
	u32 tx_data;
	u32 rx_data;
	u16 pos1;
	u16 pos2;

	/* If tx hdr and echoed hdr are not equal then there might be an issue
	 * with the connection between SPI host and MAC-PHY. Here this case is
	 * considered as MAC-PHY is not connected.
	 */
	if (tx_hdr != rx_hdr)
		return -ENODEV;

	if (wnr) {
		if (!ctrl_prot) {
			/* In case of ctrl write, both tx data & echoed
			 * data are compared for the error.
			 */
			pos1 = TC6_HDR_SIZE;
			pos2 = TC6_HDR_SIZE * 2;
			for (u8 i = 0; i < len; i++) {
				tx_data = *(u32 *)&ptx[pos1 + (i * TC6_HDR_SIZE)];
				rx_data = *(u32 *)&prx[pos2 + (i * TC6_HDR_SIZE)];
				if (tx_data != rx_data)
					return -ENODEV;
			}
			return 0;
		}
	} else {
		if (!ctrl_prot)
			return 0;
	}

	/* In case of ctrl read or ctrl write in protected mode, the rx data and
	 * the complement of rx data are compared for the error.
	 */
	pos1 = TC6_HDR_SIZE * 2;
	pos2 = TC6_HDR_SIZE * 3;
	for (u8 i = 0; i < len; i++) {
		rx_data = *(u32 *)&prx[pos1 + (i * TC6_HDR_SIZE * 2)];
		rx_data_complement = *(u32 *)&prx[pos2 + (i * TC6_HDR_SIZE * 2)];
		if (rx_data != ~rx_data_complement)
			return -ENODEV;
	}

	return 0;
}

int oa_tc6_perform_ctrl(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len,
			bool wnr, bool ctrl_prot)
{
	u8 *tx_buf;
	u8 *rx_buf;
	u16 size;
	u16 pos;
	int ret;

	if (ctrl_prot)
		size = (TC6_HDR_SIZE * 2) + (len * (TC6_HDR_SIZE * 2));
	else
		size = (TC6_HDR_SIZE * 2) + (len * TC6_HDR_SIZE);

	tx_buf = kzalloc(size, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;

	rx_buf = kzalloc(size, GFP_KERNEL);
	if (!rx_buf) {
		ret = -ENOMEM;
		goto err_rx_buf_kzalloc;
	}

	/* Prepare control command */
	oa_tc6_prepare_ctrl_buf(tc6, addr, val, len, wnr, tx_buf, ctrl_prot);

	/* Perform SPI transfer */
	ret = oa_tc6_spi_transfer(tc6->spi, tx_buf, rx_buf, size);
	if (ret)
		goto err_ctrl;

	/* In case of reset write, the echoed control command doesn't have any
	 * valid data. So no need to check for error.
	 */
	if (addr != OA_TC6_RESET) {
		/* Check echoed/received control reply */
		ret = oa_tc6_check_control(tc6, tx_buf, rx_buf, len, wnr,
					   ctrl_prot);
		if (ret)
			goto err_ctrl;
	}

	if (!wnr) {
		/* Copy read data from the rx data in case of ctrl read */
		for (u8 i = 0; i < len; i++) {
			if (!ctrl_prot) {
				pos = (TC6_HDR_SIZE * 2) + (i * TC6_HDR_SIZE);
				val[i] = be32_to_cpu(*(u32 *)&rx_buf[pos]);
			} else {
				pos = (TC6_HDR_SIZE * 2) +
				       (i * (TC6_HDR_SIZE * 2));
				val[i] = be32_to_cpu(*(u32 *)&rx_buf[pos]);
			}
		}
	}

err_ctrl:
	kfree(rx_buf);
err_rx_buf_kzalloc:
	kfree(tx_buf);
	return ret;
}

static u16 oa_tc6_prepare_empty_chunk(struct oa_tc6 *tc6, u8 *buf, u8 cp_count)
{
	u32 hdr;

	/* Prepare empty chunks used for getting interrupt information or if
	 * receive data available.
	 */
	for (u8 i = 0; i < cp_count; i++) {
		hdr = FIELD_PREP(DATA_HDR_DNC, 1);
		hdr |= FIELD_PREP(DATA_HDR_P, oa_tc6_get_parity(hdr));
		hdr = cpu_to_be32(hdr);
		*(u32 *)&buf[i * (tc6->cps + TC6_HDR_SIZE)] = hdr;
		memset(&buf[TC6_HDR_SIZE + (i * (tc6->cps + TC6_HDR_SIZE))], 0,
		       tc6->cps);
	}

	return cp_count * (tc6->cps + TC6_HDR_SIZE);
}

static void oa_tc6_rx_eth_ready(struct oa_tc6 *tc6)
{
	struct sk_buff *skb = NULL;

	/* Send the received ethernet packet to network layer */
	skb = netdev_alloc_skb(tc6->netdev, tc6->rxd_bytes + NET_IP_ALIGN);
	if (!skb) {
		tc6->netdev->stats.rx_dropped++;
		netdev_err(tc6->netdev, "Out of memory for rx'd frame");
	} else {
		skb_reserve(skb, NET_IP_ALIGN);
		memcpy(skb_put(skb, tc6->rxd_bytes), &tc6->eth_rx_buf[0],
		       tc6->rxd_bytes);
		skb->protocol = eth_type_trans(skb, tc6->netdev);
		tc6->netdev->stats.rx_packets++;
		tc6->netdev->stats.rx_bytes += tc6->rxd_bytes;
		netif_rx(skb);
	}
}

static int oa_tc6_process_exst(struct oa_tc6 *tc6)
{
	u32 regval;
	int ret;

	ret = oa_tc6_read_register(tc6, OA_TC6_STS0, &regval, 1);
	if (ret) {
		netdev_err(tc6->netdev, "STS0 register read failed.\n");
		return ret;
	}
	if (regval & TXPE)
		netdev_err(tc6->netdev, "Transmit protocol error\n");
	if (regval & TXBOE)
		netdev_err(tc6->netdev, "Transmit buffer overflow\n");
	if (regval & TXBUE)
		netdev_err(tc6->netdev, "Transmit buffer underflow\n");
	if (regval & RXBOE)
		netdev_err(tc6->netdev, "Receive buffer overflow\n");
	if (regval & LOFE)
		netdev_err(tc6->netdev, "Loss of frame\n");
	if (regval & HDRE)
		netdev_err(tc6->netdev, "Header error\n");
	if (regval & TXFCSE)
		netdev_err(tc6->netdev, "Transmit Frame Check Sequence Error\n");
	ret = oa_tc6_write_register(tc6, OA_TC6_STS0, &regval, 1);
	if (ret)
		netdev_err(tc6->netdev, "STS0 register write failed.\n");
	return ret;
}

static int oa_tc6_process_rx_chunks(struct oa_tc6 *tc6, u8 *buf, u16 len)
{
	u8 cp_count;
	u8 *payload;
	u32 ftr;
	u16 ebo;
	u16 sbo;

	/* Calculate the number of chunks received */
	cp_count = len / (tc6->cps + TC6_FTR_SIZE);

	for (u8 i = 0; i < cp_count; i++) {
		/* Get the footer and payload */
		ftr = *(u32 *)&buf[tc6->cps + (i * (tc6->cps + TC6_FTR_SIZE))];
		ftr = be32_to_cpu(ftr);
		payload = &buf[(i * (tc6->cps + TC6_FTR_SIZE))];
		/* Check for footer parity error */
		if (oa_tc6_get_parity(ftr)) {
			netdev_err(tc6->netdev, "Footer: Parity error\n");
			goto err_exit;
		}
		/* If EXST set in the footer then read STS0 register to get the
		 * status information.
		 */
		if (FIELD_GET(DATA_FTR_EXST, ftr)) {
			if (oa_tc6_process_exst(tc6))
				netdev_err(tc6->netdev, "Failed to process EXST\n");
			goto err_exit;
		}
		if (FIELD_GET(DATA_FTR_HDRB, ftr)) {
			netdev_err(tc6->netdev, "Footer: Received header bad\n");
			goto err_exit;
		}
		if (!FIELD_GET(DATA_FTR_SYNC, ftr)) {
			netdev_err(tc6->netdev, "Footer: Configuration unsync\n");
			goto err_exit;
		}
		/* If Frame Drop is set, indicates that the MAC has detected a
		 * condition for which the SPI host should drop the received
		 * ethernet frame.
		 */
		if (FIELD_GET(DATA_FTR_FD, ftr) && FIELD_GET(DATA_FTR_EV, ftr)) {
			netdev_warn(tc6->netdev, "Footer: Frame drop\n");
			if (FIELD_GET(DATA_FTR_SV, ftr)) {
				goto start_new_frame;
			} else {
				if (tc6->rx_eth_started) {
					tc6->rxd_bytes = 0;
					tc6->rx_eth_started = false;
					tc6->netdev->stats.rx_dropped++;
				}
				continue;
			}
		}
		/* Check for data valid */
		if (FIELD_GET(DATA_FTR_DV, ftr)) {
			/* Check whether both start valid and end valid are in a
			 * single chunk payload means a single chunk payload may
			 * contain an entire ethernet frame.
			 */
			if (FIELD_GET(DATA_FTR_SV, ftr) &&
			    FIELD_GET(DATA_FTR_EV, ftr)) {
				sbo = FIELD_GET(DATA_FTR_SWO, ftr) * 4;
				ebo = FIELD_GET(DATA_FTR_EBO, ftr) + 1;
				if (ebo <= sbo) {
					memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes],
					       &payload[0], ebo);
					tc6->rxd_bytes += ebo;
					oa_tc6_rx_eth_ready(tc6);
					tc6->rxd_bytes = 0;
					memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes],
					       &payload[sbo], tc6->cps - sbo);
					tc6->rxd_bytes += (tc6->cps - sbo);
					goto exit;
				} else {
					memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes],
					       &payload[sbo], ebo - sbo);
					tc6->rxd_bytes += (ebo - sbo);
					oa_tc6_rx_eth_ready(tc6);
					tc6->rxd_bytes = 0;
					goto exit;
				}
			}
start_new_frame:
			/* Check for start valid to start capturing the incoming
			 * ethernet frame.
			 */
			if (FIELD_GET(DATA_FTR_SV, ftr) && !tc6->rx_eth_started) {
				tc6->rxd_bytes = 0;
				tc6->rx_eth_started = true;
				sbo = FIELD_GET(DATA_FTR_SWO, ftr) * 4;
				memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes],
				       &payload[sbo], tc6->cps - sbo);
				tc6->rxd_bytes += (tc6->cps - sbo);
				goto exit;
			}

			/* Check for end valid and calculate the copy length */
			if (tc6->rx_eth_started) {
				if (FIELD_GET(DATA_FTR_EV, ftr))
					ebo = FIELD_GET(DATA_FTR_EBO, ftr) + 1;
				else
					ebo = tc6->cps;

				memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes],
				       &payload[0], ebo);
				tc6->rxd_bytes += ebo;
				if (FIELD_GET(DATA_FTR_EV, ftr)) {
					/* If End Valid set then send the
					 * received ethernet frame to n/w.
					 */
					oa_tc6_rx_eth_ready(tc6);
					tc6->rxd_bytes = 0;
					tc6->rx_eth_started = false;
				}
			}
		}

exit:
		tc6->txc = FIELD_GET(DATA_FTR_TXC, ftr);
		tc6->rca = FIELD_GET(DATA_FTR_RCA, ftr);
	}
	return FTR_OK;

err_exit:
	if (tc6->rx_eth_started) {
		tc6->rxd_bytes = 0;
		tc6->rx_eth_started = false;
		tc6->netdev->stats.rx_dropped++;
	}
	return FTR_ERR;
}

static int oa_tc6_handler(void *data)
{
	struct oa_tc6 *tc6 = data;
	bool txc_wait = false;
	u16 tx_pos = 0;
	u32 regval;
	u16 len;
	int ret;

	while (likely(!kthread_should_stop())) {
		wait_event_interruptible(tc6->tc6_wq, tc6->tx_flag ||
					 tc6->int_flag || tc6->rca ||
					 kthread_should_stop());
		if (tc6->int_flag && !tc6->reset) {
			tc6->int_flag = false;
			tc6->reset = true;
			ret = oa_tc6_perform_ctrl(tc6, OA_TC6_STS0, &regval, 1,
						  false, false);
			if (ret) {
				dev_err(&tc6->spi->dev, "Failed to read STS0\n");
				continue;
			}
			/* Check for reset complete interrupt status */
			if (regval & RESETC) {
				regval = RESETC;
				/* SPI host should write RESETC bit with one to
				 * clear the reset interrupt status.
				 */
				ret = oa_tc6_perform_ctrl(tc6, OA_TC6_STS0,
							  &regval, 1, true,
							  false);
				if (ret) {
					dev_err(&tc6->spi->dev,
						"Failed to write STS0\n");
					continue;
				}
				complete(&tc6->rst_complete);
			}
		}

		if (tc6->int_flag || tc6->rca) {
			/* If rca is updated from the previous footer then
			 * prepare the empty chunks equal to rca and perform
			 * SPI transfer to receive the ethernet frame.
			 */
			if (tc6->rca) {
				len = oa_tc6_prepare_empty_chunk(tc6,
								 tc6->spi_tx_buf,
								 tc6->rca);
			} else {
				/* If there is an interrupt then perform a SPI
				 * transfer with a empty chunk to get the
				 * details.
				 */
				tc6->int_flag = false;
				len = oa_tc6_prepare_empty_chunk(tc6,
								 tc6->spi_tx_buf,
								 1);
			}
			/* Perform SPI transfer */
			ret = oa_tc6_spi_transfer(tc6->spi, tc6->spi_tx_buf,
						  tc6->spi_rx_buf, len);
			if (ret) {
				netdev_err(tc6->netdev, "SPI transfer failed\n");
				continue;
			}
			/* Process the received chunks to get the ethernet frame
			 * or interrupt details.
			 */
			if (oa_tc6_process_rx_chunks(tc6, tc6->spi_rx_buf, len))
				continue;
		}

		/* If there is a tx ethernet frame available */
		if (tc6->tx_flag || txc_wait) {
			tc6->tx_flag = false;
			txc_wait = false;
			len = 0;
			if (!tc6->txc) {
				/* If there is no txc available to transport the
				 * tx ethernet frames then wait for the MAC-PHY
				 * interrupt to get the txc availability.
				 */
				txc_wait = true;
				continue;
			} else if (tc6->txc >= tc6->txc_needed) {
				len = tc6->txc_needed * (tc6->cps + TC6_HDR_SIZE);
			} else {
				len = tc6->txc * (tc6->cps + TC6_HDR_SIZE);
			}
			memcpy(&tc6->spi_tx_buf[0], &tc6->eth_tx_buf[tx_pos],
			       len);
			ret = oa_tc6_spi_transfer(tc6->spi, tc6->spi_tx_buf,
						  tc6->spi_rx_buf, len);
			if (ret) {
				netdev_err(tc6->netdev, "SPI transfer failed\n");
				continue;
			}
			/* Process the received chunks to get the ethernet frame
			 * or status.
			 */
			if (oa_tc6_process_rx_chunks(tc6, tc6->spi_rx_buf,
						     len)) {
				/* In case of error while processing rx chunks
				 * discard the incomplete tx ethernet frame and
				 * resend it.
				 */
				tx_pos = 0;
				tc6->txc_needed = tc6->total_txc_needed;
			} else {
				tx_pos += len;
				tc6->txc_needed = tc6->txc_needed -
						  (len / (tc6->cps + TC6_HDR_SIZE));
				/* If the complete ethernet frame is transmitted
				 * then return the skb and update the details to
				 * n/w layer.
				 */
				if (!tc6->txc_needed) {
					tc6->netdev->stats.tx_packets++;
					tc6->netdev->stats.tx_bytes += tc6->tx_skb->len;
					dev_kfree_skb(tc6->tx_skb);
					tx_pos = 0;
					tc6->tx_skb = NULL;
					if (netif_queue_stopped(tc6->netdev))
						netif_wake_queue(tc6->netdev);
				} else if (tc6->txc) {
					/* If txc is available again and updated
					 * from the previous footer then perform
					 * tx again.
					 */
					tc6->tx_flag = true;
				} else {
					/* If there is no txc then wait for the
					 * interrupt to indicate txc
					 * availability.
					 */
					txc_wait = true;
				}
			}
		}
	}
	return 0;
}

static void oa_tc6_prepare_tx_chunks(struct oa_tc6 *tc6, u8 *buf,
				     struct sk_buff *skb)
{
	bool frame_started = false;
	u16 copied_bytes = 0;
	u16 copy_len;
	u32 hdr;

	/* Calculate the number tx credit counts needed to transport the tx
	 * ethernet frame.
	 */
	tc6->txc_needed = (skb->len / tc6->cps) + ((skb->len % tc6->cps) ? 1 : 0);
	tc6->total_txc_needed = tc6->txc_needed;

	for (u8 i = 0; i < tc6->txc_needed; i++) {
		/* Prepare the header for each chunks to be transmitted */
		hdr = FIELD_PREP(DATA_HDR_DNC, 1) |
		      FIELD_PREP(DATA_HDR_DV, 1);
		if (!frame_started) {
			hdr |= FIELD_PREP(DATA_HDR_SV, 1) |
			       FIELD_PREP(DATA_HDR_SWO, 0);
			frame_started = true;
		}
		if ((tc6->cps + copied_bytes) >= skb->len) {
			copy_len = skb->len - copied_bytes;
			hdr |= FIELD_PREP(DATA_HDR_EBO, copy_len - 1) |
			       FIELD_PREP(DATA_HDR_EV, 1);
		} else {
			copy_len = tc6->cps;
		}
		copied_bytes += copy_len;
		hdr |= FIELD_PREP(DATA_HDR_P, oa_tc6_get_parity(hdr));
		hdr = cpu_to_be32(hdr);
		*(u32 *)&buf[i * (tc6->cps + TC6_HDR_SIZE)] = hdr;
		/* Copy the ethernet frame in the chunk payload section */
		memcpy(&buf[TC6_HDR_SIZE + (i * (tc6->cps + TC6_HDR_SIZE))],
		       &skb->data[copied_bytes - copy_len], copy_len);
	}
}

netdev_tx_t oa_tc6_send_eth_pkt(struct oa_tc6 *tc6, struct sk_buff *skb)
{
	if (tc6->tx_skb) {
		netif_stop_queue(tc6->netdev);
		return NETDEV_TX_BUSY;
	}

	tc6->tx_skb = skb;
	/* Prepare tx chunks using the tx ethernet frame */
	oa_tc6_prepare_tx_chunks(tc6, tc6->eth_tx_buf, skb);

	/* Wake tc6 task to perform tx transfer */
	tc6->tx_flag = true;
	wake_up_interruptible(&tc6->tc6_wq);

	return NETDEV_TX_OK;
}
EXPORT_SYMBOL_GPL(oa_tc6_send_eth_pkt);

static irqreturn_t macphy_irq(int irq, void *dev_id)
{
	struct oa_tc6 *tc6 = dev_id;

	/* Wake tc6 task to perform interrupt action */
	tc6->int_flag = true;
	wake_up_interruptible(&tc6->tc6_wq);

	return IRQ_HANDLED;
}

static int oa_tc6_sw_reset(struct oa_tc6 *tc6)
{
	long timeleft;
	u32 regval;
	int ret;

	/* Perform software reset with both protected and unprotected control
	 * commands because the driver doesn't know the current status of the
	 * MAC-PHY.
	 */
	regval = SW_RESET;
	reinit_completion(&tc6->rst_complete);
	ret = oa_tc6_perform_ctrl(tc6, OA_TC6_RESET, &regval, 1, true, false);
	if (ret) {
		dev_err(&tc6->spi->dev, "RESET register write failed\n");
		return ret;
	}

	ret = oa_tc6_perform_ctrl(tc6, OA_TC6_RESET, &regval, 1, true, true);
	if (ret) {
		dev_err(&tc6->spi->dev, "RESET register write failed\n");
		return ret;
	}
	timeleft = wait_for_completion_interruptible_timeout(&tc6->rst_complete,
							     msecs_to_jiffies(1));
	if (timeleft <= 0) {
		dev_err(&tc6->spi->dev, "MAC-PHY reset failed\n");
		return -ENODEV;
	}

	return 0;
}

int oa_tc6_write_register(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len)
{
	return oa_tc6_perform_ctrl(tc6, addr, val, len, true, tc6->ctrl_prot);
}
EXPORT_SYMBOL_GPL(oa_tc6_write_register);

int oa_tc6_read_register(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len)
{
	return oa_tc6_perform_ctrl(tc6, addr, val, len, false, tc6->ctrl_prot);
}
EXPORT_SYMBOL_GPL(oa_tc6_read_register);

int oa_tc6_configure(struct oa_tc6 *tc6, u8 cps, bool ctrl_prot, bool tx_cut_thr,
		     bool rx_cut_thr)
{
	u32 regval;
	int ret;

	/* Read BUFSTS register to get the current txc and rca. */
	ret = oa_tc6_read_register(tc6, OA_TC6_BUFSTS, &regval, 1);
	if (ret)
		return ret;

	tc6->txc = FIELD_GET(TXC, regval);
	tc6->rca = FIELD_GET(RCA, regval);

	/* Read and configure the IMASK0 register for unmasking the interrupts */
	ret = oa_tc6_read_register(tc6, OA_TC6_IMASK0, &regval, 1);
	if (ret)
		return ret;

	regval &= TXPEM & TXBOEM & TXBUEM & RXBOEM & LOFEM & HDREM;
	ret = oa_tc6_write_register(tc6, OA_TC6_IMASK0, &regval, 1);
	if (ret)
		return ret;

	/* Configure the CONFIG0 register with the required configurations */
	regval = SYNC;
	if (ctrl_prot)
		regval |= PROTE;
	if (tx_cut_thr)
		regval |= TXCTE;
	if (rx_cut_thr)
		regval |= RXCTE;
	regval |= FIELD_PREP(CPS, ilog2(cps) / ilog2(2));

	ret = oa_tc6_write_register(tc6, OA_TC6_CONFIG0, &regval, 1);
	if (ret)
		return ret;

	tc6->cps = cps;
	tc6->ctrl_prot = ctrl_prot;
	tc6->tx_cut_thr = tx_cut_thr;
	tc6->rx_cut_thr = rx_cut_thr;

	return 0;
}
EXPORT_SYMBOL_GPL(oa_tc6_configure);

struct oa_tc6 *oa_tc6_init(struct spi_device *spi, struct net_device *netdev)
{
	struct oa_tc6 *tc6;
	int ret;

	if (!spi)
		return NULL;

	if (!netdev)
		return NULL;

	tc6 = kzalloc(sizeof(*tc6), GFP_KERNEL);
	if (!tc6)
		return NULL;

	tc6->spi = spi;
	tc6->netdev = netdev;

	/* Allocate memory for the tx buffer used for SPI transfer. */
	tc6->spi_tx_buf = kzalloc(MAX_ETH_LEN + (OA_TC6_MAX_CPS * TC6_HDR_SIZE),
				  GFP_KERNEL);
	if (!tc6->spi_tx_buf)
		goto err_spi_tx_buf_alloc;

	/* Allocate memory for the rx buffer used for SPI transfer. */
	tc6->spi_rx_buf = kzalloc(MAX_ETH_LEN + (OA_TC6_MAX_CPS * TC6_FTR_SIZE),
				  GFP_KERNEL);
	if (!tc6->spi_rx_buf)
		goto err_spi_rx_buf_alloc;

	/* Allocate memory for the tx ethernet chunks to transfer on SPI. */
	tc6->eth_tx_buf = kzalloc(MAX_ETH_LEN + (OA_TC6_MAX_CPS * TC6_HDR_SIZE),
				  GFP_KERNEL);
	if (!tc6->eth_tx_buf)
		goto err_eth_tx_buf_alloc;

	/* Allocate memory for the rx ethernet packet. */
	tc6->eth_rx_buf = kzalloc(MAX_ETH_LEN + (OA_TC6_MAX_CPS * TC6_FTR_SIZE),
				  GFP_KERNEL);
	if (!tc6->eth_rx_buf)
		goto err_eth_rx_buf_alloc;

	/* Used for triggering the OA TC6 task */
	init_waitqueue_head(&tc6->tc6_wq);

	init_completion(&tc6->rst_complete);

	/* This task performs the SPI transfer */
	tc6->tc6_task = kthread_run(oa_tc6_handler, tc6, "OA TC6 Task");
	if (IS_ERR(tc6->tc6_task))
		goto err_tc6_task;

	/* Set the highest priority to the tc6 task as it is time critical */
	sched_set_fifo(tc6->tc6_task);

	/* Register MAC-PHY interrupt service routine */
	ret = devm_request_irq(&spi->dev, spi->irq, macphy_irq, 0, "macphy int",
			       tc6);
	if ((ret != -ENOTCONN) && ret < 0) {
		dev_err(&spi->dev, "Error attaching macphy irq %d\n", ret);
		goto err_macphy_irq;
	}

	/* Perform MAC-PHY software reset */
	if (oa_tc6_sw_reset(tc6))
		goto err_macphy_reset;

	return tc6;

err_macphy_reset:
	devm_free_irq(&tc6->spi->dev, tc6->spi->irq, tc6);
err_macphy_irq:
	kthread_stop(tc6->tc6_task);
err_tc6_task:
	kfree(tc6->eth_rx_buf);
err_eth_rx_buf_alloc:
	kfree(tc6->eth_tx_buf);
err_eth_tx_buf_alloc:
	kfree(tc6->spi_rx_buf);
err_spi_rx_buf_alloc:
	kfree(tc6->spi_tx_buf);
err_spi_tx_buf_alloc:
	kfree(tc6);
	return NULL;
}
EXPORT_SYMBOL_GPL(oa_tc6_init);

int oa_tc6_deinit(struct oa_tc6 *tc6)
{
	int ret;

	devm_free_irq(&tc6->spi->dev, tc6->spi->irq, tc6);
	ret = kthread_stop(tc6->tc6_task);
	if (!ret) {
		kfree(tc6->eth_rx_buf);
		kfree(tc6->eth_tx_buf);
		kfree(tc6->spi_rx_buf);
		kfree(tc6->spi_tx_buf);
		kfree(tc6);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(oa_tc6_deinit);
