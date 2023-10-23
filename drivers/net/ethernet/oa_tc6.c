// SPDX-License-Identifier: GPL-2.0+
/*
 * OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface framework
 *
 * Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mdio.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/oa_tc6.h>

/* Opaque structure for MACPHY drivers */
struct oa_tc6 {
	int (*config_cps_buf)(void *tc6, u32 cps);
	struct work_struct tx_work;
	struct net_device *netdev;
	struct phy_device *phydev;
	struct mii_bus *mdiobus;
	struct spi_device *spi;
	struct sk_buff *tx_skb;
	bool rx_eth_started;
	struct device *dev;
	/* Protects oa_tc6_perform_spi_xfer function elements between MAC-PHY
	 * interrupt handler and the tx work handler.
	 */
	struct mutex lock;
	u8 *ctrl_tx_buf;
	u8 *ctrl_rx_buf;
	u8 *spi_tx_buf;
	u8 *spi_rx_buf;
	u8 *eth_tx_buf;
	u8 *eth_rx_buf;
	u16 rxd_bytes;
	u8 txc_needed;
	bool int_flag;
	bool tx_flag;
	bool dprac;
	bool iprac;
	bool prote;
	u16 tx_pos;
	u32 cps;
	u8 txc;
	u8 rca;
};

static int oa_tc6_spi_transfer(struct spi_device *spi, u8 *ptx, u8 *prx, u16 len)
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

static int oa_tc6_get_parity(u32 p)
{
	/* Public domain code snippet, lifted from
	 * http://www-graphics.stanford.edu/~seander/bithacks.html
	 */
	p ^= p >> 1;
	p ^= p >> 2;
	p = (p & 0x11111111U) * 0x11111111U;

	/* Odd parity is used here */
	return !((p >> 28) & 1);
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
		*(__be32 *)&buf[i * (tc6->cps + TC6_HDR_SIZE)] = cpu_to_be32(hdr);
		memset(&buf[TC6_HDR_SIZE + (i * (tc6->cps + TC6_HDR_SIZE))], 0,
		       tc6->cps);
	}

	return cp_count * (tc6->cps + TC6_HDR_SIZE);
}

static void oa_tc6_prepare_ctrl_buf(struct oa_tc6 *tc6, u32 addr, u32 val[],
				    u8 len, bool wnr, u8 *buf, bool prote)
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
	*(__be32 *)buf = cpu_to_be32(hdr);

	if (wnr) {
		for (u8 i = 0; i < len; i++) {
			u16 pos;

			if (!prote) {
				/* Send the value to be written followed by the
				 * header.
				 */
				pos = (i + 1) * TC6_HDR_SIZE;
				*(__be32 *)&buf[pos] = cpu_to_be32(val[i]);
			} else {
				/* If protected then send complemented value
				 * also followed by actual value.
				 */
				pos = TC6_HDR_SIZE + (i * (TC6_HDR_SIZE * 2));
				*(__be32 *)&buf[pos] = cpu_to_be32(val[i]);
				pos = (i + 1) * (TC6_HDR_SIZE * 2);
				*(__be32 *)&buf[pos] = cpu_to_be32(~val[i]);
			}
		}
	}
}

static int oa_tc6_check_control(struct oa_tc6 *tc6, u8 *ptx, u8 *prx, u8 len,
				bool wnr, bool prote)
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
		if (!prote) {
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
		if (!prote)
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

/**
 * oa_tc6_perform_ctrl - function to perform control transaction.
 * @tc6: oa_tc6 struct.
 * @addr: register address of the MACPHY to be written/read.
 * @val: value to be written/read in the @addr register address of the MACPHY.
 * @len: number of consecutive registers to be written/read from @addr.
 * @wnr: operation to be performed for the register @addr (write not read).
 * @prote: control data (register) read/write protection enable/disable.
 *
 * Returns 0 on success otherwise failed.
 */
static int oa_tc6_perform_ctrl(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len,
			       bool wnr, bool prote)
{
	u8 *tx_buf = tc6->ctrl_tx_buf;
	u8 *rx_buf = tc6->ctrl_rx_buf;
	u16 size;
	u16 pos;
	int ret;

	if (prote)
		size = (TC6_HDR_SIZE * 2) + (len * (TC6_HDR_SIZE * 2));
	else
		size = (TC6_HDR_SIZE * 2) + (len * TC6_HDR_SIZE);

	/* Prepare control command */
	oa_tc6_prepare_ctrl_buf(tc6, addr, val, len, wnr, tx_buf, prote);

	/* Perform SPI transfer */
	ret = oa_tc6_spi_transfer(tc6->spi, tx_buf, rx_buf, size);
	if (ret)
		return ret;

	/* In case of reset write, the echoed control command doesn't have any
	 * valid data. So no need to check for errors.
	 */
	if (addr != RESET) {
		/* Check echoed/received control reply for errors */
		ret = oa_tc6_check_control(tc6, tx_buf, rx_buf, len, wnr, prote);
		if (ret)
			return ret;
	}

	if (!wnr) {
		/* Copy read data from the rx data in case of ctrl read */
		for (u8 i = 0; i < len; i++) {
			if (!prote) {
				pos = (TC6_HDR_SIZE * 2) + (i * TC6_HDR_SIZE);
				val[i] = be32_to_cpu(*(__be32 *)&rx_buf[pos]);
			} else {
				pos = (TC6_HDR_SIZE * 2) +
				       (i * (TC6_HDR_SIZE * 2));
				val[i] = be32_to_cpu(*(__be32 *)&rx_buf[pos]);
			}
		}
	}

	return ret;
}

static int oa_tc6_configure(struct oa_tc6 *tc6)
{
	struct spi_device *spi = tc6->spi;
	struct device_node *oa_node;
	bool dprac;
	bool iprac;
	u32 regval;
	u8 mincps;
	bool ctc;
	int ret;

	/* Read BUFSTS register to get the current txc and rca. */
	ret = oa_tc6_read_register(tc6, OA_TC6_BUFSTS, &regval);
	if (ret)
		return ret;

	tc6->txc = FIELD_GET(TXC, regval);
	tc6->rca = FIELD_GET(RCA, regval);

	/* Read and configure the IMASK0 register for unmasking the interrupts */
	ret = oa_tc6_perform_ctrl(tc6, IMASK0, &regval, 1, false, false);
	if (ret)
		return ret;

	regval &= ~(TXPEM & TXBOEM & TXBUEM & RXBOEM & LOFEM & HDREM);

	ret = oa_tc6_perform_ctrl(tc6, IMASK0, &regval, 1, true, false);
	if (ret)
		return ret;

	/* Read STDCAP register to get the MAC-PHY standard capabilities */
	ret = oa_tc6_perform_ctrl(tc6, STDCAP, &regval, 1, false, false);
	if (ret)
		return ret;

	/* Minimum supported Chunk Payload Size */
	mincps = FIELD_GET(MINCPS, regval);
	/* Cut-Through Capability */
	ctc = (regval & CTC) ? true : false;
	/* Direct PHY Register Access Capability */
	dprac = (regval & DPRAC) ? true : false;
	/* Indirect PHY Register access Capability */
	iprac = (regval & IPRAC) ? true : false;

	regval = 0;
	oa_node = of_get_child_by_name(spi->dev.of_node, "oa-tc6");
	if (oa_node) {
		/* Read OA parameters from DT */
		if (of_property_present(oa_node, "oa-cps")) {
			ret = of_property_read_u32(oa_node, "oa-cps", &tc6->cps);
			if (ret < 0)
				return ret;
			/* Return error if the configured cps is less than the
			 * minimum cps supported by the MAC-PHY.
			 */
			if (tc6->cps < mincps)
				return -ENODEV;
		} else {
			tc6->cps = OA_TC6_MAX_CPS;
		}
		/* Call queue buffer size config function if defined by MAC */
		if (tc6->config_cps_buf) {
			ret = tc6->config_cps_buf(tc6, tc6->cps);
			if (ret)
				return ret;
		}
		if (of_property_present(oa_node, "oa-txcte")) {
			/* Return error if the tx cut through mode is configured
			 * but it is not supported by MAC-PHY.
			 */
			if (ctc)
				regval |= TXCTE;
			else
				return -ENODEV;
		}
		if (of_property_present(oa_node, "oa-rxcte")) {
			/* Return error if the rx cut through mode is configured
			 * but it is not supported by MAC-PHY.
			 */
			if (ctc)
				regval |= RXCTE;
			else
				return -ENODEV;
		}
		if (of_property_present(oa_node, "oa-prote")) {
			regval |= PROTE;
			tc6->prote = true;
		}
		if (of_property_present(oa_node, "oa-dprac")) {
			/* Return error if the direct phy register access mode
			 * is configured but it is not supported by MAC-PHY.
			 */
			if (dprac)
				tc6->dprac = true;
			else
				return -ENODEV;
		}
		if (of_property_present(oa_node, "oa-iprac")) {
			/* Return error if the indirect phy register access mode
			 * is configured but it is not supported by MAC-PHY.
			 */
			if (iprac)
				tc6->iprac = true;
			else
				return -ENODEV;
		}
	} else {
		tc6->cps = OA_TC6_MAX_CPS;
	}

	regval |= FIELD_PREP(CPS, ilog2(tc6->cps) / ilog2(2)) | SYNC;

	return oa_tc6_perform_ctrl(tc6, CONFIG0, &regval, 1, true, false);
}

static int oa_tc6_sw_reset(struct oa_tc6 *tc6)
{
	u32 regval;
	int ret;

	/* Perform software reset with both protected and unprotected control
	 * commands because the driver doesn't know the current status of the
	 * MAC-PHY.
	 */
	regval = SWRESET;
	ret = oa_tc6_perform_ctrl(tc6, RESET, &regval, 1, true, true);
	if (ret)
		return ret;

	ret = oa_tc6_perform_ctrl(tc6, RESET, &regval, 1, true, false);
	if (ret)
		return ret;

	/* The chip completes a reset in 3us, we might get here earlier than
	 * that, as an added margin we'll conditionally sleep 5us.
	 */
	udelay(5);

	ret = oa_tc6_perform_ctrl(tc6, STATUS0, &regval, 1, false, false);
	if (ret)
		return ret;

	/* Check for reset complete interrupt status */
	if (regval & RESETC) {
		regval = RESETC;
		/* SPI host should write RESETC bit with one to
		 * clear the reset interrupt status.
		 */
		ret = oa_tc6_perform_ctrl(tc6, STATUS0, &regval, 1, true, false);
		if (ret)
			return ret;
	} else {
		return -ENODEV;
	}

	return 0;
}

/**
 * oa_tc6_write_register - function for writing a MACPHY register.
 * @tc6: oa_tc6 struct.
 * @addr: register address of the MACPHY to be written.
 * @val: value to be written in the @addr register address of the MACPHY.
 *
 * Returns 0 on success otherwise failed.
 */
int oa_tc6_write_register(struct oa_tc6 *tc6, u32 addr, u32 val)
{
	return oa_tc6_perform_ctrl(tc6, addr, &val, 1, true, tc6->prote);
}
EXPORT_SYMBOL_GPL(oa_tc6_write_register);

/**
 * oa_tc6_read_register - function for reading a MACPHY register.
 * @tc6: oa_tc6 struct.
 * @addr: register address of the MACPHY to be read.
 * @val: value read from the @addr register address of the MACPHY.
 *
 * Returns 0 on success otherwise failed.
 */
int oa_tc6_read_register(struct oa_tc6 *tc6, u32 addr, u32 *val)
{
	return oa_tc6_perform_ctrl(tc6, addr, val, 1, false, tc6->prote);
}
EXPORT_SYMBOL_GPL(oa_tc6_read_register);

/**
 * oa_tc6_write_registers - function for writing multiple consecutive registers.
 * @tc6: oa_tc6 struct.
 * @addr: address of the first register to be written in the MACPHY.
 * @val: values to be written from the starting register address @addr.
 * @len: number of consecutive registers to be written from @addr.
 *
 * Maximum of 128 consecutive registers can be written starting at @addr.
 *
 * Returns 0 on success otherwise failed.
 */
int oa_tc6_write_registers(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len)
{
	return oa_tc6_perform_ctrl(tc6, addr, val, len, true, tc6->prote);
}
EXPORT_SYMBOL_GPL(oa_tc6_write_registers);

/**
 * oa_tc6_read_registers - function for reading multiple consecutive registers.
 * @tc6: oa_tc6 struct.
 * @addr: address of the first register to be read in the MACPHY.
 * @val: values to be read from the starting register address @addr.
 *
 * Maximum of 128 consecutive registers can be read starting at @addr.
 *
 * Returns 0 on success otherwise failed.
 */
int oa_tc6_read_registers(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len)
{
	return oa_tc6_perform_ctrl(tc6, addr, val, len, false, tc6->prote);
}
EXPORT_SYMBOL_GPL(oa_tc6_read_registers);

static void oa_tc6_handle_link_change(struct net_device *netdev)
{
	phy_print_status(netdev->phydev);
}

static int oa_tc6_mdiobus_read(struct mii_bus *bus, int phy_id, int idx)
{
	struct oa_tc6 *tc6 = bus->priv;
	u32 regval;
	bool ret;

	ret = oa_tc6_read_register(tc6, 0xFF00 | (idx & 0xFF), &regval);
	if (ret)
		return -ENODEV;

	return regval;
}

static int oa_tc6_mdiobus_write(struct mii_bus *bus, int phy_id, int idx,
				u16 val)
{
	struct oa_tc6 *tc6 = bus->priv;

	return oa_tc6_write_register(tc6, 0xFF00 | (idx & 0xFF), val);
}

static int oa_tc6_phy_init(struct oa_tc6 *tc6)
{
	int ret;

	if (tc6->dprac) {
		tc6->mdiobus = mdiobus_alloc();
		if (!tc6->mdiobus) {
			netdev_err(tc6->netdev, "MDIO bus alloc failed\n");
			return -ENODEV;
		}

		tc6->mdiobus->phy_mask = ~(u32)BIT(1);
		tc6->mdiobus->priv = tc6;
		tc6->mdiobus->read = oa_tc6_mdiobus_read;
		tc6->mdiobus->write = oa_tc6_mdiobus_write;
		tc6->mdiobus->name = "oa-tc6-mdiobus";
		tc6->mdiobus->parent = tc6->dev;

		snprintf(tc6->mdiobus->id, ARRAY_SIZE(tc6->mdiobus->id), "%s",
			 dev_name(&tc6->spi->dev));

		ret = mdiobus_register(tc6->mdiobus);
		if (ret) {
			netdev_err(tc6->netdev, "Could not register MDIO bus\n");
			mdiobus_free(tc6->mdiobus);
			return ret;
		}

		tc6->phydev = phy_find_first(tc6->mdiobus);
		if (!tc6->phydev) {
			netdev_err(tc6->netdev, "No PHY found\n");
			mdiobus_unregister(tc6->mdiobus);
			mdiobus_free(tc6->mdiobus);
			return -ENODEV;
		}

		tc6->phydev->is_internal = true;
		ret = phy_connect_direct(tc6->netdev, tc6->phydev,
					 &oa_tc6_handle_link_change,
					 PHY_INTERFACE_MODE_INTERNAL);
		if (ret) {
			netdev_err(tc6->netdev, "Can't attach PHY to %s\n",
				   tc6->mdiobus->id);
			mdiobus_unregister(tc6->mdiobus);
			mdiobus_free(tc6->mdiobus);
			return ret;
		}

		phy_attached_info(tc6->netdev->phydev);

		return ret;
	} else if (tc6->iprac) {
		// To be implemented. Currently returns -ENODEV.
		return -ENODEV;
	} else {
		return -ENODEV;
	}
	return 0;
}

static int oa_tc6_process_exst(struct oa_tc6 *tc6)
{
	u32 regval;
	int ret;

	ret = oa_tc6_read_register(tc6, STATUS0, &regval);
	if (ret)
		return ret;

	if (regval & TXPE)
		net_err_ratelimited("%s: Transmit protocol error\n",
				    tc6->netdev->name);

	if (regval & TXBOE)
		net_err_ratelimited("%s: Transmit buffer overflow\n",
				    tc6->netdev->name);

	if (regval & TXBUE)
		net_err_ratelimited("%s: Transmit buffer underflow\n",
				    tc6->netdev->name);

	if (regval & RXBOE)
		net_err_ratelimited("%s: Receive buffer overflow\n",
				    tc6->netdev->name);

	if (regval & LOFE)
		net_err_ratelimited("%s: Loss of frame\n", tc6->netdev->name);

	if (regval & HDRE)
		net_err_ratelimited("%s: Header error\n", tc6->netdev->name);

	if (regval & TXFCSE)
		net_err_ratelimited("%s: Tx Frame Check Seq Error\n",
				    tc6->netdev->name);

	return oa_tc6_write_register(tc6, STATUS0, regval);
}

static void oa_tc6_rx_eth_ready(struct oa_tc6 *tc6)
{
	struct sk_buff *skb;

	/* Send the received ethernet packet to network layer */
	skb = netdev_alloc_skb(tc6->netdev, tc6->rxd_bytes + NET_IP_ALIGN);
	if (!skb) {
		tc6->netdev->stats.rx_dropped++;
		netdev_dbg(tc6->netdev, "Out of memory for rx'd frame");
	} else {
		skb_reserve(skb, NET_IP_ALIGN);
		memcpy(skb_put(skb, tc6->rxd_bytes), &tc6->eth_rx_buf[0],
		       tc6->rxd_bytes);
		skb->protocol = eth_type_trans(skb, tc6->netdev);
		tc6->netdev->stats.rx_packets++;
		tc6->netdev->stats.rx_bytes += tc6->rxd_bytes;
		/* 0 for NET_RX_SUCCESS and 1 for NET_RX_DROP */
		if (netif_rx(skb))
			tc6->netdev->stats.rx_dropped++;
	}
}

static void oa_tc6_rx_eth_complete2(struct oa_tc6 *tc6, u8 *payload, u32 ftr)
{
	u16 ebo;

	if (FIELD_GET(DATA_FTR_EV, ftr))
		ebo = FIELD_GET(DATA_FTR_EBO, ftr) + 1;
	else
		ebo = tc6->cps;

	memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes], &payload[0], ebo);
	tc6->rxd_bytes += ebo;
	if (FIELD_GET(DATA_FTR_EV, ftr)) {
		/* If EV set then send the received ethernet frame to n/w */
		oa_tc6_rx_eth_ready(tc6);
		tc6->rxd_bytes = 0;
		tc6->rx_eth_started = false;
	}
}

static void oa_tc6_rx_eth_complete1(struct oa_tc6 *tc6, u8 *payload, u32 ftr)
{
	u16 ebo;
	u16 sbo;

	sbo = FIELD_GET(DATA_FTR_SWO, ftr) * 4;
	ebo = FIELD_GET(DATA_FTR_EBO, ftr) + 1;

	if (ebo <= sbo) {
		memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes], &payload[0], ebo);
		tc6->rxd_bytes += ebo;
		oa_tc6_rx_eth_ready(tc6);
		tc6->rxd_bytes = 0;
		memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes], &payload[sbo],
		       tc6->cps - sbo);
		tc6->rxd_bytes += (tc6->cps - sbo);
	} else {
		memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes], &payload[sbo],
		       ebo - sbo);
		tc6->rxd_bytes += (ebo - sbo);
		oa_tc6_rx_eth_ready(tc6);
		tc6->rxd_bytes = 0;
	}
}

static void oa_tc6_start_rx_eth(struct oa_tc6 *tc6, u8 *payload, u32 ftr)
{
	u16 sbo;

	tc6->rxd_bytes = 0;
	tc6->rx_eth_started = true;
	sbo = FIELD_GET(DATA_FTR_SWO, ftr) * 4;
	memcpy(&tc6->eth_rx_buf[tc6->rxd_bytes], &payload[sbo], tc6->cps - sbo);
	tc6->rxd_bytes += (tc6->cps - sbo);
}

static u32 oa_tc6_get_footer(struct oa_tc6 *tc6, u8 *buf, u8 cp_num)
{
	__be32 ftr;

	ftr = *(__be32 *)&buf[tc6->cps + (cp_num * (tc6->cps + TC6_FTR_SIZE))];

	return be32_to_cpu(ftr);
}

static void oa_tc6_update_txc_rca(struct oa_tc6 *tc6, u32 ftr)
{
	tc6->txc = FIELD_GET(DATA_FTR_TXC, ftr);
	tc6->rca = FIELD_GET(DATA_FTR_RCA, ftr);
}

static int oa_tc6_check_ftr_errors(struct oa_tc6 *tc6, u32 ftr)
{
	/* Check for footer parity error */
	if (oa_tc6_get_parity(ftr)) {
		net_err_ratelimited("%s: Footer parity error\n",
				    tc6->netdev->name);
		return FTR_ERR;
	}
	/* If EXST set in the footer then read STS0 register to get the
	 * status information.
	 */
	if (FIELD_GET(DATA_FTR_EXST, ftr)) {
		if (oa_tc6_process_exst(tc6))
			net_err_ratelimited("%s: Failed to process EXST\n",
					    tc6->netdev->name);
		return FTR_ERR;
	}
	if (FIELD_GET(DATA_FTR_HDRB, ftr)) {
		net_err_ratelimited("%s: Footer eeceived header bad\n",
				    tc6->netdev->name);
		return FTR_ERR;
	}
	if (!FIELD_GET(DATA_FTR_SYNC, ftr)) {
		net_err_ratelimited("%s: Footer configuration unsync\n",
				    tc6->netdev->name);
		return FTR_ERR;
	}
	return FTR_OK;
}

static void oa_tc6_drop_rx_eth(struct oa_tc6 *tc6)
{
	tc6->rxd_bytes = 0;
	tc6->rx_eth_started = false;
	tc6->netdev->stats.rx_dropped++;
	net_err_ratelimited("%s: Footer frame drop\n",
			    tc6->netdev->name);
}

static int oa_tc6_process_rx_chunks(struct oa_tc6 *tc6, u8 *buf, u16 len)
{
	u8 cp_count;
	u8 *payload;
	u32 ftr;
	int ret;

	/* Calculate the number of chunks received */
	cp_count = len / (tc6->cps + TC6_FTR_SIZE);

	for (u8 i = 0; i < cp_count; i++) {
		/* Get the footer and payload */
		ftr = oa_tc6_get_footer(tc6, buf, i);
		payload = &buf[(i * (tc6->cps + TC6_FTR_SIZE))];
		/* Check for footer errors */
		ret = oa_tc6_check_ftr_errors(tc6, ftr);
		if (ret) {
			if (tc6->rx_eth_started)
				oa_tc6_drop_rx_eth(tc6);
			return ret;
		}
		/* If Frame Drop is set, indicates that the MAC has detected a
		 * condition for which the SPI host should drop the received
		 * ethernet frame.
		 */
		if (FIELD_GET(DATA_FTR_FD, ftr) && FIELD_GET(DATA_FTR_EV, ftr)) {
			if (tc6->rx_eth_started)
				oa_tc6_drop_rx_eth(tc6);

			if (FIELD_GET(DATA_FTR_SV, ftr)) {
				oa_tc6_start_rx_eth(tc6, payload, ftr);
				oa_tc6_update_txc_rca(tc6, ftr);
			}
			continue;
		}
		/* Check for data valid */
		if (FIELD_GET(DATA_FTR_DV, ftr)) {
			/* Check whether both start valid and end valid are in a
			 * single chunk payload means a single chunk payload may
			 * contain an entire ethernet frame.
			 */
			if (FIELD_GET(DATA_FTR_SV, ftr) &&
			    FIELD_GET(DATA_FTR_EV, ftr)) {
				oa_tc6_rx_eth_complete1(tc6, payload, ftr);
				oa_tc6_update_txc_rca(tc6, ftr);
				continue;
			}
			/* Check for start valid to start capturing the incoming
			 * ethernet frame.
			 */
			if (FIELD_GET(DATA_FTR_SV, ftr) && !tc6->rx_eth_started) {
				oa_tc6_start_rx_eth(tc6, payload, ftr);
				oa_tc6_update_txc_rca(tc6, ftr);
				continue;
			}

			/* Check for end valid and calculate the copy length */
			if (tc6->rx_eth_started)
				oa_tc6_rx_eth_complete2(tc6, payload, ftr);
		}
		oa_tc6_update_txc_rca(tc6, ftr);
	}
	return FTR_OK;
}

static void oa_tc6_phy_exit(struct oa_tc6 *tc6)
{
	phy_disconnect(tc6->phydev);
	mdiobus_unregister(tc6->mdiobus);
	mdiobus_free(tc6->mdiobus);
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
		*(__be32 *)&buf[i * (tc6->cps + TC6_HDR_SIZE)] = cpu_to_be32(hdr);
		/* Copy the ethernet frame in the chunk payload section */
		memcpy(&buf[TC6_HDR_SIZE + (i * (tc6->cps + TC6_HDR_SIZE))],
		       &skb->data[copied_bytes - copy_len], copy_len);
	}
}

static u16 oa_tc6_calculate_tx_len(struct oa_tc6 *tc6)
{
	/* If the available txc is greater than the txc needed (calculated from
	 * the tx ethernet frame then the tx length can be txc needed. Else the
	 * tx length can be available txc and the remaining needed txc will be
	 * updated either in the footer of the current transfer or through the
	 * interrupt.
	 */
	if (tc6->txc >= tc6->txc_needed)
		return tc6->txc_needed * (tc6->cps + TC6_HDR_SIZE);
	else
		return tc6->txc * (tc6->cps + TC6_HDR_SIZE);
}

static u16 oa_tc6_calculate_rca_len(struct oa_tc6 *tc6, u16 tx_len)
{
	u16 rca_needed = 0;
	u16 tx_txc;

	/* If tx eth frame and rca are available at the same time then check
	 * whether the rca is less than the needed txc for the tx eth frame. If
	 * not then add additional empty chunks along with the tx chunks to get
	 * all the rca.
	 */
	if (tc6->tx_flag && tc6->txc) {
		tx_txc = tc6->txc_needed - (tx_len / (tc6->cps + TC6_HDR_SIZE));
		if (tx_txc < tc6->rca)
			rca_needed = tc6->rca - tx_txc;
	} else {
		/* Add only empty chunks for rca if there is no tx chunks
		 * available to transmit.
		 */
		rca_needed = tc6->rca;
	}
	return oa_tc6_prepare_empty_chunk(tc6, &tc6->spi_tx_buf[tx_len],
					  rca_needed);
}

static void oa_tc6_tx_eth_complete(struct oa_tc6 *tc6)
{
	tc6->netdev->stats.tx_packets++;
	tc6->netdev->stats.tx_bytes += tc6->tx_skb->len;
	dev_kfree_skb(tc6->tx_skb);
	tc6->tx_pos = 0;
	tc6->tx_skb = NULL;
	tc6->tx_flag = false;
	if (netif_queue_stopped(tc6->netdev))
		netif_wake_queue(tc6->netdev);
}

static int oa_tc6_perform_spi_xfer(struct oa_tc6 *tc6)
{
	bool do_tx_again;
	u16 total_len;
	u16 rca_len;
	u16 tx_len;
	int ret;

	do {
		do_tx_again = false;
		rca_len = 0;
		tx_len = 0;

		/* In case of an interrupt, perform an empty chunk transfer to
		 * know the purpose of the interrupt. Interrupt may occur in
		 * case of RCA (Receive Chunk Available) and TXC (Transmit
		 * Credit Count). Both will occur if they are not indicated
		 * through the previous footer.
		 */
		if (tc6->int_flag) {
			tc6->int_flag = false;
			total_len = oa_tc6_prepare_empty_chunk(tc6,
							       tc6->spi_tx_buf,
							       1);
		} else {
			/* Calculate the transfer length */
			if (tc6->tx_flag && tc6->txc) {
				tx_len = oa_tc6_calculate_tx_len(tc6);
				memcpy(&tc6->spi_tx_buf[0],
				       &tc6->eth_tx_buf[tc6->tx_pos], tx_len);
			}

			if (tc6->rca)
				rca_len = oa_tc6_calculate_rca_len(tc6, tx_len);

			total_len = tx_len + rca_len;
		}
		ret = oa_tc6_spi_transfer(tc6->spi, tc6->spi_tx_buf,
					  tc6->spi_rx_buf, total_len);
		if (ret)
			return ret;
		/* Process the rxd chunks to get the ethernet frame or status */
		ret = oa_tc6_process_rx_chunks(tc6, tc6->spi_rx_buf, total_len);
		if (ret)
			return ret;
		if (tc6->tx_flag) {
			tc6->tx_pos += tx_len;
			tc6->txc_needed = tc6->txc_needed -
					  (tx_len / (tc6->cps + TC6_HDR_SIZE));
			/* If the complete ethernet frame is transmitted then
			 * return the skb and update the details to n/w layer.
			 */
			if (!tc6->txc_needed)
				oa_tc6_tx_eth_complete(tc6);
			else if (tc6->txc)
				/* If txc is available again and updated from
				 * the previous footer then perform tx again.
				 */
				do_tx_again = true;
		}

		/* If rca is updated from the previous footer then perform empty
		 * tx to receive ethernet frame.
		 */
		if (tc6->rca)
			do_tx_again = true;
	} while (do_tx_again);

	return 0;
}

/* MAC-PHY interrupt handler */
static irqreturn_t macphy_irq_handler(int irq, void *dev_id)
{
	struct oa_tc6 *tc6 = dev_id;

	tc6->int_flag = true;
	mutex_lock(&tc6->lock);
	if (oa_tc6_perform_spi_xfer(tc6))
		net_err_ratelimited("%s: SPI transfer failed\n",
				    tc6->netdev->name);
	mutex_unlock(&tc6->lock);

	return IRQ_HANDLED;
}

/* Workqueue to perform SPI transfer */
static void tc6_tx_work_handler(struct work_struct *work)
{
	struct oa_tc6 *tc6 = container_of(work, struct oa_tc6, tx_work);

	mutex_lock(&tc6->lock);
	if (oa_tc6_perform_spi_xfer(tc6))
		net_err_ratelimited("%s: SPI transfer failed\n",
				    tc6->netdev->name);
	mutex_unlock(&tc6->lock);
}

/**
 * oa_tc6_send_eth_pkt - function for sending the tx ethernet frame.
 * @tc6: oa_tc6 struct.
 * @skb: socket buffer in which the ethernet frame is stored.
 *
 * As this is called from atomic context, work queue is used here because the
 * spi_sync will block for the transfer completion.
 *
 * Returns NETDEV_TX_OK if the tx work is scheduled or NETDEV_TX_BUSY if the
 * previous enqueued tx skb is in progress.
 */
netdev_tx_t oa_tc6_send_eth_pkt(struct oa_tc6 *tc6, struct sk_buff *skb)
{
	if (tc6->tx_flag) {
		netif_stop_queue(tc6->netdev);
		return NETDEV_TX_BUSY;
	}

	tc6->tx_skb = skb;
	/* Prepare tx chunks using the tx ethernet frame */
	oa_tc6_prepare_tx_chunks(tc6, tc6->eth_tx_buf, skb);

	tc6->tx_flag = true;
	schedule_work(&tc6->tx_work);

	return NETDEV_TX_OK;
}
EXPORT_SYMBOL_GPL(oa_tc6_send_eth_pkt);

/**
 * oa_tc6_init - allocates and intializes oa_tc6 structure.
 * @spi: device with which data will be exchanged.
 * @netdev: network device to use.
 * @config_cps_buf: function pointer passed by MAC driver to be called for
 * configuring cps buffer size. Queue buffer size in the MAC has to be configured
 * according to the cps.
 *
 * Returns pointer reference to the oa_tc6 structure if all the memory
 * allocation success otherwise NULL.
 */
struct oa_tc6 *oa_tc6_init(struct spi_device *spi, struct net_device *netdev,
			   int (*config_cps_buf)(void *tc6, u32 cps))
{
	struct oa_tc6 *tc6;
	int ret;

	tc6 = devm_kzalloc(&spi->dev, sizeof(*tc6), GFP_KERNEL);
	if (!tc6)
		return NULL;

	/* Allocate memory for the control tx buffer used for SPI transfer. */
	tc6->ctrl_tx_buf = devm_kzalloc(&spi->dev, TC6_CTRL_BUF_SIZE, GFP_KERNEL);
	if (!tc6->ctrl_tx_buf)
		return NULL;

	/* Allocate memory for the control rx buffer used for SPI transfer. */
	tc6->ctrl_rx_buf = devm_kzalloc(&spi->dev, TC6_CTRL_BUF_SIZE, GFP_KERNEL);
	if (!tc6->ctrl_rx_buf)
		return NULL;

	/* Allocate memory for the tx buffer used for SPI transfer. */
	tc6->spi_tx_buf = devm_kzalloc(&spi->dev, ETH_LEN + (OA_TC6_MAX_CPS * TC6_HDR_SIZE),
				       GFP_KERNEL);
	if (!tc6->spi_tx_buf)
		return NULL;

	/* Allocate memory for the rx buffer used for SPI transfer. */
	tc6->spi_rx_buf = devm_kzalloc(&spi->dev, ETH_LEN + (OA_TC6_MAX_CPS * TC6_FTR_SIZE),
				       GFP_KERNEL);
	if (!tc6->spi_rx_buf)
		return NULL;

	/* Allocate memory for the tx ethernet chunks to transfer on SPI. */
	tc6->eth_tx_buf = devm_kzalloc(&spi->dev, ETH_LEN + (OA_TC6_MAX_CPS * TC6_HDR_SIZE),
				       GFP_KERNEL);
	if (!tc6->eth_tx_buf)
		return NULL;

	/* Allocate memory for the rx ethernet packet. */
	tc6->eth_rx_buf = devm_kzalloc(&spi->dev, ETH_LEN + (OA_TC6_MAX_CPS * TC6_FTR_SIZE),
				       GFP_KERNEL);
	if (!tc6->eth_rx_buf)
		return NULL;

	tc6->spi = spi;
	tc6->netdev = netdev;
	SET_NETDEV_DEV(netdev, &spi->dev);
	tc6->config_cps_buf = config_cps_buf;
	/* Set the SPI controller to pump at realtime priority */
	spi->rt = true;
	spi_setup(spi);

	/* Perform MAC-PHY software reset */
	if (oa_tc6_sw_reset(tc6)) {
		dev_err(&spi->dev, "MAC-PHY software reset failed\n");
		return NULL;
	}

	/* Perform OA parameters and MAC-PHY configuration */
	if (oa_tc6_configure(tc6)) {
		dev_err(&spi->dev, "OA and MAC-PHY configuration failed\n");
		return NULL;
	}

	/* Initialize PHY */
	if (oa_tc6_phy_init(tc6)) {
		dev_err(&spi->dev, "PHY initialization failed\n");
		return NULL;
	}
	mutex_init(&tc6->lock);
	INIT_WORK(&tc6->tx_work, tc6_tx_work_handler);
	/* Register MAC-PHY interrupt service routine */
	ret = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
					macphy_irq_handler, IRQF_ONESHOT,
					"macphy int", tc6);
	if (ret) {
		dev_err(&spi->dev, "Error attaching macphy irq %d\n", ret);
		return NULL;
	}

	return tc6;
}
EXPORT_SYMBOL_GPL(oa_tc6_init);

/**
 * oa_tc6_exit - exit function.
 * @tc6: oa_tc6 struct.
 *
 */
void oa_tc6_exit(struct oa_tc6 *tc6)
{
	devm_free_irq(&tc6->spi->dev, tc6->spi->irq, tc6);
	oa_tc6_phy_exit(tc6);
}
EXPORT_SYMBOL_GPL(oa_tc6_exit);

MODULE_DESCRIPTION("OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface Lib");
MODULE_AUTHOR("Parthiban Veerasooran <parthiban.veerasooran@microchip.com>");
MODULE_LICENSE("GPL");
