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
	struct net_device *netdev;
	struct phy_device *phydev;
	struct mii_bus *mdiobus;
	struct spi_device *spi;
	struct device *dev;
	u8 *ctrl_tx_buf;
	u8 *ctrl_rx_buf;
	bool dprac;
	bool iprac;
	bool prote;
	u32 cps;
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

static void oa_tc6_phy_exit(struct oa_tc6 *tc6)
{
	phy_disconnect(tc6->phydev);
	mdiobus_unregister(tc6->mdiobus);
	mdiobus_free(tc6->mdiobus);
}

/**
 * oa_tc6_init - allocates and intializes oa_tc6 structure.
 * @spi: device with which data will be exchanged.
 * @netdev: network device to use.
 *
 * Returns pointer reference to the oa_tc6 structure if all the memory
 * allocation success otherwise NULL.
 */
struct oa_tc6 *oa_tc6_init(struct spi_device *spi, struct net_device *netdev)
{
	struct oa_tc6 *tc6;

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

	tc6->spi = spi;
	tc6->netdev = netdev;
	SET_NETDEV_DEV(netdev, &spi->dev);

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
	oa_tc6_phy_exit(tc6);
}
EXPORT_SYMBOL_GPL(oa_tc6_exit);

MODULE_DESCRIPTION("OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface Lib");
MODULE_AUTHOR("Parthiban Veerasooran <parthiban.veerasooran@microchip.com>");
MODULE_LICENSE("GPL");
