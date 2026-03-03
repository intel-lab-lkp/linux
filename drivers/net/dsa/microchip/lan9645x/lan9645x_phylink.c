// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2026 Microchip Technology Inc.
 */

#include <linux/phy.h>
#include <linux/phy/phy.h>

#include "lan9645x_main.h"

void lan9645x_phylink_get_caps(struct lan9645x *lan9645x, int port,
			       struct phylink_config *c)
{
	c->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE | MAC_10 |
			      MAC_100 | MAC_1000FD | MAC_2500FD;

	switch (port) {
	case 0 ... 3:
		__set_bit(PHY_INTERFACE_MODE_GMII, c->supported_interfaces);
		break;
	case 4:
		__set_bit(PHY_INTERFACE_MODE_GMII, c->supported_interfaces);
		phy_interface_set_rgmii(c->supported_interfaces);
		break;
	case 5 ... 6:
		/* SerDes ports: QSGMII/SGMII/1000BASEX/2500BASEX modes
		 * require PCS support which is not yet implemented.
		 */
		break;
	case 7 ... 8:
		/* QSGMII mode on ports 7-8 requires SerDes PCS support,
		 * which is not yet implemented.
		 */
		phy_interface_set_rgmii(c->supported_interfaces);
		break;
	default:
		break;
	}
}

static int lan9645x_rgmii_setup(struct lan9645x *lan9645x, int port, int speed,
				phy_interface_t mode)
{
	bool tx_delay = false, rx_delay = false;
	u32 rx_idx, tx_idx;
	u8 tx_clk;
	int idx;

	/* Port 4 or 7 is RGMII_0 and port 8 is RGMII_1 */
	idx = port == 8 ? 1 : 0;

	if (!phy_interface_mode_is_rgmii(mode))
		return 0;

	tx_clk = speed == SPEED_1000 ? 1 :
		 speed == SPEED_100  ? 2 :
		 speed == SPEED_10   ? 3 : 0;

	lan_rmw(HSIO_RGMII_CFG_RGMII_RX_RST_SET(0) |
		HSIO_RGMII_CFG_RGMII_TX_RST_SET(0) |
		HSIO_RGMII_CFG_TX_CLK_CFG_SET(tx_clk),
		HSIO_RGMII_CFG_RGMII_RX_RST |
		HSIO_RGMII_CFG_RGMII_TX_RST |
		HSIO_RGMII_CFG_TX_CLK_CFG,
		lan9645x, HSIO_RGMII_CFG(idx));

	/* We configure delays on the MAC side. When the PHY is not responsible
	 * for delays, the MAC is, which is why RGMII_TXID results in
	 * rx_delay=true.
	 */
	if (mode == PHY_INTERFACE_MODE_RGMII ||
	    mode == PHY_INTERFACE_MODE_RGMII_TXID)
		rx_delay = true;

	if (mode == PHY_INTERFACE_MODE_RGMII ||
	    mode == PHY_INTERFACE_MODE_RGMII_RXID)
		tx_delay = true;

	/* Setup DLL configuration. Register layout:
	 * 0:        RGMII_0_RX
	 * 1:        RGMII_0_TX
	 * 2:        RGMII_1_RX
	 * 3:        RGMII_1_TX
	 * ...
	 * (N<<1)    RGMII_N_RX,
	 * (N<<1)+1: RGMII_N_TX,
	 */
	rx_idx = idx << 1;
	tx_idx = rx_idx + 1;

	/* Enable DLL in RGMII clock paths, deassert DLL reset, and start the
	 * delay tune FSM.
	 */
	lan_rmw(HSIO_DLL_CFG_DLL_CLK_ENA_SET(1) |
		HSIO_DLL_CFG_DLL_RST_SET(0) |
		HSIO_DLL_CFG_DLL_ENA_SET(rx_delay) |
		HSIO_DLL_CFG_DELAY_ENA_SET(rx_delay),
		HSIO_DLL_CFG_DLL_CLK_ENA |
		HSIO_DLL_CFG_DLL_RST |
		HSIO_DLL_CFG_DLL_ENA |
		HSIO_DLL_CFG_DELAY_ENA,
		lan9645x, HSIO_DLL_CFG(rx_idx));

	lan_rmw(HSIO_DLL_CFG_DLL_CLK_ENA_SET(1) |
		HSIO_DLL_CFG_DLL_RST_SET(0) |
		HSIO_DLL_CFG_DLL_ENA_SET(tx_delay) |
		HSIO_DLL_CFG_DELAY_ENA_SET(tx_delay),
		HSIO_DLL_CFG_DLL_CLK_ENA |
		HSIO_DLL_CFG_DLL_RST |
		HSIO_DLL_CFG_DLL_ENA |
		HSIO_DLL_CFG_DELAY_ENA,
		lan9645x, HSIO_DLL_CFG(tx_idx));

	return 0;
}

static void lan9645x_phylink_mac_config(struct lan9645x *lan9645x, int port,
					unsigned int mode,
					const struct phylink_link_state *state)
{
	if (phy_interface_mode_is_rgmii(state->interface))
		lan9645x_rgmii_setup(lan9645x, port, state->speed,
				     state->interface);
}

static int lan9645x_phylink_mac_prepare(struct lan9645x *lan9645x, int port,
					unsigned int mode,
					phy_interface_t iface)
{
	switch (port) {
	case 0 ... 3:
		lan_rmw(HSIO_HW_CFG_GMII_ENA_SET(BIT(port)),
			HSIO_HW_CFG_GMII_ENA_SET(BIT(port)), lan9645x,
			HSIO_HW_CFG);
		break;
	case 4:
		lan_rmw(HSIO_HW_CFG_GMII_ENA_SET(BIT(port)),
			HSIO_HW_CFG_GMII_ENA_SET(BIT(port)), lan9645x,
			HSIO_HW_CFG);

		if (phy_interface_mode_is_rgmii(iface))
			lan_rmw(HSIO_HW_CFG_RGMII_0_CFG_SET(1),
				HSIO_HW_CFG_RGMII_0_CFG,
				lan9645x, HSIO_HW_CFG);

		break;
	case 7 ... 8:
		lan_rmw(HSIO_HW_CFG_GMII_ENA_SET(BIT(port)),
			HSIO_HW_CFG_GMII_ENA_SET(BIT(port)), lan9645x,
			HSIO_HW_CFG);
		break;
	default:
		/* Ports 5-6 are SerDes-only and need PCS support (not yet
		 * implemented). They are excluded from phylink_get_caps.
		 */
		return -EINVAL;
	}

	return 0;
}

static int lan9645x_port_is_cuphy(struct lan9645x *lan9645x, int port,
				  phy_interface_t interface)
{
	return port >= 0 && port <= 4 && interface == PHY_INTERFACE_MODE_GMII;
}

static void lan9645x_phylink_mac_link_up(struct lan9645x *lan9645x, int port,
					 unsigned int link_an_mode,
					 phy_interface_t interface,
					 struct phy_device *phydev, int speed,
					 int duplex, bool tx_pause,
					 bool rx_pause)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	int rx_ifg1, rx_ifg2, tx_ifg, gtx_clk = 0;
	struct lan9645x_path_delay *path_delay;
	int gspeed = LAN9645X_SPEED_DISABLED;
	int mode = 0;
	int fc_spd;

	/* Configure speed for RGMII modules. */
	if (phy_interface_mode_is_rgmii(interface))
		lan9645x_rgmii_setup(lan9645x, port, speed, interface);

	if (duplex == DUPLEX_FULL) {
		mode |= DEV_MAC_MODE_CFG_FDX_ENA_SET(1);
		rx_ifg2 = DEV_MAC_IFG_CFG_RX_IFG2_SET(0x2);
		tx_ifg = DEV_MAC_IFG_CFG_TX_IFG_SET(0x5);

	} else {
		rx_ifg2 = DEV_MAC_IFG_CFG_RX_IFG2_SET(0x2);
		tx_ifg = DEV_MAC_IFG_CFG_TX_IFG_SET(0x6);
	}

	switch (speed) {
	case SPEED_10:
		rx_ifg1 = DEV_MAC_IFG_CFG_RX_IFG1_SET(0x2);
		gspeed = LAN9645X_SPEED_10;
		break;
	case SPEED_100:
		rx_ifg1 = DEV_MAC_IFG_CFG_RX_IFG1_SET(0x1);
		gspeed = LAN9645X_SPEED_100;
		break;
	case SPEED_1000:
		gspeed = LAN9645X_SPEED_1000;
		mode |= DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);
		mode |= DEV_MAC_MODE_CFG_FDX_ENA_SET(1);
		rx_ifg1 = DEV_MAC_IFG_CFG_RX_IFG1_SET(0x1);
		rx_ifg2 = DEV_MAC_IFG_CFG_RX_IFG2_SET(0x2);
		tx_ifg = DEV_MAC_IFG_CFG_TX_IFG_SET(0x6);
		gtx_clk = 1;
		break;
	case SPEED_2500:
		gspeed = LAN9645X_SPEED_2500;
		mode |= DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);
		mode |= DEV_MAC_MODE_CFG_FDX_ENA_SET(1);
		rx_ifg1 = DEV_MAC_IFG_CFG_RX_IFG1_SET(0x1);
		rx_ifg2 = DEV_MAC_IFG_CFG_RX_IFG2_SET(0x2);
		tx_ifg = DEV_MAC_IFG_CFG_TX_IFG_SET(0x6);
		break;
	default:
		dev_err(lan9645x->dev, "Unsupported speed on port %d: %d\n",
			p->chip_port, speed);
		return;
	}

	p->speed = gspeed;
	p->duplex = duplex;
	fc_spd = lan9645x_speed_fc_enc(p->speed);

	if (phy_interface_num_ports(interface) == 4 ||
	    interface == PHY_INTERFACE_MODE_SGMII)
		mode |= DEV_MAC_MODE_CFG_GIGA_MODE_ENA_SET(1);

	lan_rmw(mode,
		DEV_MAC_MODE_CFG_FDX_ENA |
		DEV_MAC_MODE_CFG_GIGA_MODE_ENA,
		lan9645x, DEV_MAC_MODE_CFG(p->chip_port));

	lan_rmw(tx_ifg | rx_ifg1 | rx_ifg2,
		DEV_MAC_IFG_CFG_TX_IFG |
		DEV_MAC_IFG_CFG_RX_IFG1 |
		DEV_MAC_IFG_CFG_RX_IFG2,
		lan9645x, DEV_MAC_IFG_CFG(p->chip_port));

	lan_rmw(DEV_MAC_HDX_CFG_SEED_SET(p->chip_port) |
		DEV_MAC_HDX_CFG_SEED_LOAD_SET(1),
		DEV_MAC_HDX_CFG_SEED |
		DEV_MAC_HDX_CFG_SEED_LOAD, lan9645x,
		DEV_MAC_HDX_CFG(p->chip_port));

	if (lan9645x_port_is_cuphy(lan9645x, port, interface)) {
		lan_rmw(CHIP_TOP_CUPHY_PORT_CFG_GTX_CLK_ENA_SET(gtx_clk),
			CHIP_TOP_CUPHY_PORT_CFG_GTX_CLK_ENA, lan9645x,
			CHIP_TOP_CUPHY_PORT_CFG(p->chip_port));
	}

	lan_rmw(DEV_MAC_HDX_CFG_SEED_LOAD_SET(0),
		DEV_MAC_HDX_CFG_SEED_LOAD, lan9645x,
		DEV_MAC_HDX_CFG(p->chip_port));

	/* Set PFC link speed and enable map */
	lan_rmw(ANA_PFC_CFG_FC_LINK_SPEED_SET(fc_spd) |
		ANA_PFC_CFG_RX_PFC_ENA_SET(0),
		ANA_PFC_CFG_FC_LINK_SPEED |
		ANA_PFC_CFG_RX_PFC_ENA,
		lan9645x, ANA_PFC_CFG(p->chip_port));

	lan_rmw(DEV_PCS1G_CFG_PCS_ENA_SET(1),
		DEV_PCS1G_CFG_PCS_ENA, lan9645x,
		DEV_PCS1G_CFG(p->chip_port));

	lan_rmw(DEV_PCS1G_SD_CFG_SD_ENA_SET(0),
		DEV_PCS1G_SD_CFG_SD_ENA,
		lan9645x, DEV_PCS1G_SD_CFG(p->chip_port));

	lan_rmw(SYS_PAUSE_CFG_PAUSE_ENA_SET(1),
		SYS_PAUSE_CFG_PAUSE_ENA,
		lan9645x, SYS_PAUSE_CFG(p->chip_port));

	/* Set SMAC of Pause frame (00:00:00:00:00:00) */
	lan_wr(0, lan9645x, DEV_FC_MAC_LOW_CFG(p->chip_port));
	lan_wr(0, lan9645x, DEV_FC_MAC_HIGH_CFG(p->chip_port));

	/* Flow control */
	lan_rmw(SYS_MAC_FC_CFG_FC_LINK_SPEED_SET(fc_spd) |
		SYS_MAC_FC_CFG_FC_LATENCY_CFG_SET(0x7) |
		SYS_MAC_FC_CFG_ZERO_PAUSE_ENA_SET(1) |
		SYS_MAC_FC_CFG_PAUSE_VAL_CFG_SET(0xffff) |
		SYS_MAC_FC_CFG_RX_FC_ENA_SET(rx_pause ? 1 : 0) |
		SYS_MAC_FC_CFG_TX_FC_ENA_SET(tx_pause ? 1 : 0),
		SYS_MAC_FC_CFG_FC_LINK_SPEED |
		SYS_MAC_FC_CFG_FC_LATENCY_CFG |
		SYS_MAC_FC_CFG_ZERO_PAUSE_ENA |
		SYS_MAC_FC_CFG_PAUSE_VAL_CFG |
		SYS_MAC_FC_CFG_RX_FC_ENA |
		SYS_MAC_FC_CFG_TX_FC_ENA,
		lan9645x, SYS_MAC_FC_CFG(p->chip_port));

	list_for_each_entry(path_delay, &p->path_delays, list) {
		if (path_delay->speed == speed) {
			lan_wr(path_delay->rx_delay + p->rx_delay,
			       lan9645x, SYS_PTP_RXDLY_CFG(p->chip_port));
			lan_wr(path_delay->tx_delay,
			       lan9645x, SYS_PTP_TXDLY_CFG(p->chip_port));
		}
	}

	/* Enable MAC module */
	lan_wr(DEV_MAC_ENA_CFG_RX_ENA_SET(1) |
	       DEV_MAC_ENA_CFG_TX_ENA_SET(1),
	       lan9645x, DEV_MAC_ENA_CFG(p->chip_port));

	/* port _must_ be taken out of reset before MAC. */
	lan_rmw(DEV_CLOCK_CFG_PORT_RST_SET(0),
		DEV_CLOCK_CFG_PORT_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	/* Take out the clock from reset. Note this write will set all these
	 * fields to zero:
	 *
	 * DEV_CLOCK_CFG[*].MAC_TX_RST
	 * DEV_CLOCK_CFG[*].MAC_RX_RST
	 * DEV_CLOCK_CFG[*].PCS_TX_RST
	 * DEV_CLOCK_CFG[*].PCS_RX_RST
	 * DEV_CLOCK_CFG[*].PORT_RST
	 * DEV_CLOCK_CFG[*].PHY_RST
	 *
	 * Note link_down will assert PORT_RST, MAC_RX_RST and MAC_TX_RST, so
	 * we are effectively taking the mac tx/rx clocks out of reset.
	 *
	 * This linkspeed field has a slightly different encoding from others:
	 *
	 * - 0 is no-link
	 * - 1 is both 2500/1000
	 * - 2 is 100mbit
	 * - 3 is 10mbit
	 *
	 */
	lan_wr(DEV_CLOCK_CFG_LINK_SPEED_SET(fc_spd == 0 ? 1 : fc_spd),
	       lan9645x,
	       DEV_CLOCK_CFG(p->chip_port));

	/* Core: Enable port for frame transfer */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1) |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG_SET(1) |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE_SET(1) |
		QSYS_SW_PORT_MODE_TX_PFC_ENA_SET(0),
		QSYS_SW_PORT_MODE_PORT_ENA |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE |
		QSYS_SW_PORT_MODE_TX_PFC_ENA,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));

	lan_rmw(AFI_PORT_CFG_FC_SKIP_TTI_INJ_SET(0) |
		AFI_PORT_CFG_FRM_OUT_MAX_SET(16),
		AFI_PORT_CFG_FC_SKIP_TTI_INJ |
		AFI_PORT_CFG_FRM_OUT_MAX,
		lan9645x, AFI_PORT_CFG(p->chip_port));
}

void lan9645x_phylink_port_down(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);
	u32 val;

	/* 0.5: Disable any AFI */
	lan_rmw(AFI_PORT_CFG_FC_SKIP_TTI_INJ_SET(1) |
		AFI_PORT_CFG_FRM_OUT_MAX_SET(0),
		AFI_PORT_CFG_FC_SKIP_TTI_INJ |
		AFI_PORT_CFG_FRM_OUT_MAX,
		lan9645x, AFI_PORT_CFG(p->chip_port));

	/* wait for reg afi_port_frm_out to become 0 for the port */
	if (lan9645x_rd_poll_slow(lan9645x, AFI_PORT_FRM_OUT(p->chip_port),
				  val,
				  !AFI_PORT_FRM_OUT_FRM_OUT_CNT_GET(val)))
		dev_err(lan9645x->dev, "AFI timeout chip port %u",
			p->chip_port);

	/* 2: Disable MAC frame reception */
	lan_rmw(DEV_MAC_ENA_CFG_RX_ENA_SET(0),
		DEV_MAC_ENA_CFG_RX_ENA,
		lan9645x, DEV_MAC_ENA_CFG(p->chip_port));

	/* 1: Reset the PCS Rx clock domain  */
	lan_rmw(DEV_CLOCK_CFG_PCS_RX_RST_SET(1),
		DEV_CLOCK_CFG_PCS_RX_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	p->speed = LAN9645X_SPEED_DISABLED;
	p->duplex = DUPLEX_UNKNOWN;

	/* 3: Disable traffic being sent to or from switch port */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(0),
		QSYS_SW_PORT_MODE_PORT_ENA,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));

	/* 4: Disable dequeuing from the egress queues  */
	lan_rmw(QSYS_PORT_MODE_DEQUEUE_DIS_SET(1),
		QSYS_PORT_MODE_DEQUEUE_DIS,
		lan9645x, QSYS_PORT_MODE(p->chip_port));

	/* 5: Disable Flowcontrol */
	lan_rmw(SYS_PAUSE_CFG_PAUSE_ENA_SET(0),
		SYS_PAUSE_CFG_PAUSE_ENA,
		lan9645x, SYS_PAUSE_CFG(p->chip_port));

	/* 5.1: Disable PFC */
	lan_rmw(QSYS_SW_PORT_MODE_TX_PFC_ENA_SET(0),
		QSYS_SW_PORT_MODE_TX_PFC_ENA,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));

	/* 6: Wait a worst case time 8ms (10K jumbo/10Mbit) */
	usleep_range(8 * USEC_PER_MSEC, 9 * USEC_PER_MSEC);

	/* 7: Disable HDX backpressure. */
	lan_rmw(SYS_FRONT_PORT_MODE_HDX_MODE_SET(0),
		SYS_FRONT_PORT_MODE_HDX_MODE,
		lan9645x, SYS_FRONT_PORT_MODE(p->chip_port));

	/* 8: Flush the queues associated with the port */
	lan_rmw(QSYS_SW_PORT_MODE_AGING_MODE_SET(3),
		QSYS_SW_PORT_MODE_AGING_MODE,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));

	/* 9: Enable dequeuing from the egress queues */
	lan_rmw(QSYS_PORT_MODE_DEQUEUE_DIS_SET(0),
		QSYS_PORT_MODE_DEQUEUE_DIS,
		lan9645x, QSYS_PORT_MODE(p->chip_port));

	/* 10: Wait until flushing is complete */
	if (lan9645x_rd_poll_slow(lan9645x, QSYS_SW_STATUS(p->chip_port),
				  val, !QSYS_SW_STATUS_EQ_AVAIL_GET(val)))
		dev_err(lan9645x->dev, "Flush timeout chip port %u", port);

	/* 11: Disable MAC tx */
	lan_rmw(DEV_MAC_ENA_CFG_TX_ENA_SET(0),
		DEV_MAC_ENA_CFG_TX_ENA,
		lan9645x, DEV_MAC_ENA_CFG(p->chip_port));

	/* 12: Reset the Port and MAC clock domains */
	lan_rmw(DEV_CLOCK_CFG_PORT_RST_SET(1),
		DEV_CLOCK_CFG_PORT_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	/* Wait before resetting MAC clock domains. */
	usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);

	lan_rmw(DEV_CLOCK_CFG_MAC_TX_RST_SET(1) |
		DEV_CLOCK_CFG_MAC_RX_RST_SET(1) |
		DEV_CLOCK_CFG_PORT_RST_SET(1),
		DEV_CLOCK_CFG_MAC_TX_RST |
		DEV_CLOCK_CFG_MAC_RX_RST |
		DEV_CLOCK_CFG_PORT_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));

	/* 13: Clear flushing */
	lan_rmw(QSYS_SW_PORT_MODE_AGING_MODE_SET(1),
		QSYS_SW_PORT_MODE_AGING_MODE,
		lan9645x, QSYS_SW_PORT_MODE(p->chip_port));
}

static void lan9645x_phylink_mac_link_down(struct lan9645x *lan9645x, int port,
					   unsigned int link_an_mode,
					   phy_interface_t interface)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	lan9645x_phylink_port_down(lan9645x, port);

	/* 14: Take PCS out of reset */
	lan_rmw(DEV_CLOCK_CFG_PCS_RX_RST_SET(0) |
		DEV_CLOCK_CFG_PCS_TX_RST_SET(0),
		DEV_CLOCK_CFG_PCS_RX_RST |
		DEV_CLOCK_CFG_PCS_TX_RST,
		lan9645x, DEV_CLOCK_CFG(p->chip_port));
}

static struct lan9645x_port *
lan9645x_phylink_config_to_port(struct phylink_config *config)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);

	return lan9645x_to_port(dp->ds->priv, dp->index);
}

static void
lan9645x_port_phylink_mac_config(struct phylink_config *config,
				 unsigned int mode,
				 const struct phylink_link_state *state)
{
	struct lan9645x_port *p = lan9645x_phylink_config_to_port(config);

	lan9645x_phylink_mac_config(p->lan9645x, p->chip_port, mode, state);
}

static void lan9645x_port_phylink_mac_link_up(struct phylink_config *config,
					      struct phy_device *phydev,
					      unsigned int link_an_mode,
					      phy_interface_t interface,
					      int speed, int duplex,
					      bool tx_pause, bool rx_pause)
{
	struct lan9645x_port *p = lan9645x_phylink_config_to_port(config);

	lan9645x_phylink_mac_link_up(p->lan9645x, p->chip_port, link_an_mode,
				     interface, phydev, speed, duplex, tx_pause,
				     rx_pause);
}

static void lan9645x_port_phylink_mac_link_down(struct phylink_config *config,
						unsigned int link_an_mode,
						phy_interface_t interface)
{
	struct lan9645x_port *p = lan9645x_phylink_config_to_port(config);

	lan9645x_phylink_mac_link_down(p->lan9645x, p->chip_port, link_an_mode,
				       interface);
}

static int lan9645x_port_phylink_mac_prepare(struct phylink_config *config,
					     unsigned int mode,
					     phy_interface_t iface)
{
	struct lan9645x_port *p = lan9645x_phylink_config_to_port(config);

	return lan9645x_phylink_mac_prepare(p->lan9645x, p->chip_port, mode,
					    iface);
}

const struct phylink_mac_ops lan9645x_phylink_mac_ops = {
	.mac_config			= lan9645x_port_phylink_mac_config,
	.mac_link_up			= lan9645x_port_phylink_mac_link_up,
	.mac_link_down			= lan9645x_port_phylink_mac_link_down,
	.mac_prepare			= lan9645x_port_phylink_mac_prepare,
};
