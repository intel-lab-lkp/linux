// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2026 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

int lan9645x_port_init(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p;

	p = lan9645x_to_port(lan9645x, port);

	/* Disable learning on port */
	lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(0),
		ANA_PORT_CFG_LEARN_ENA,
		lan9645x, ANA_PORT_CFG(p->chip_port));

	p->learn_ena = false;

	lan9645x_port_set_maxlen(lan9645x, port, ETH_DATA_LEN);

	lan9645x_phylink_port_down(lan9645x, port);

	if (phy_interface_num_ports(p->phy_mode) == 4)
		lan_rmw(DEV_CLOCK_CFG_PCS_RX_RST_SET(0) |
			DEV_CLOCK_CFG_PCS_TX_RST_SET(0),
			DEV_CLOCK_CFG_PCS_RX_RST |
			DEV_CLOCK_CFG_PCS_TX_RST,
			lan9645x, DEV_CLOCK_CFG(p->chip_port));

	/* Drop frames with multicast source address */
	lan_rmw(ANA_DROP_CFG_DROP_MC_SMAC_ENA_SET(1),
		ANA_DROP_CFG_DROP_MC_SMAC_ENA, lan9645x,
		ANA_DROP_CFG(p->chip_port));

	/* Enable receiving frames on the port, and activate auto-learning of
	 * MAC addresses.
	 */
	lan_rmw(ANA_PORT_CFG_LEARNAUTO_SET(1) |
		ANA_PORT_CFG_RECV_ENA_SET(1) |
		ANA_PORT_CFG_PORTID_VAL_SET(p->chip_port),
		ANA_PORT_CFG_LEARNAUTO |
		ANA_PORT_CFG_RECV_ENA |
		ANA_PORT_CFG_PORTID_VAL,
		lan9645x, ANA_PORT_CFG(p->chip_port));

	return 0;
}

void lan9645x_port_cpu_init(struct lan9645x *lan9645x)
{
	/* Map the 8 CPU extraction queues to CPU port 9 (datasheet is wrong) */
	lan_wr(0, lan9645x, QSYS_CPU_GROUP_MAP);

	/* Configure second cpu port (chip_port 10) for manual frame injection.
	 * The AFI can not inject frames via the NPI port, unless frame aging is
	 * disabled on frontports, so we use manual injection for AFI frames.
	 */

	/* Set min-spacing of EOF to SOF on injected frames to 0, on cpu device
	 * 1. This is required when injecting with IFH.
	 * Default values emulates delay of std preamble/IFG setting on a front
	 * port.
	 */
	lan_rmw(QS_INJ_CTRL_GAP_SIZE_SET(0),
		QS_INJ_CTRL_GAP_SIZE,
		lan9645x, QS_INJ_CTRL(1));

	/* Injection: Mode: manual injection | Byte_swap */
	lan_wr(QS_INJ_GRP_CFG_MODE_SET(1) |
	       QS_INJ_GRP_CFG_BYTE_SWAP_SET(1),
	       lan9645x, QS_INJ_GRP_CFG(1));

	lan_rmw(QS_INJ_CTRL_GAP_SIZE_SET(0),
		QS_INJ_CTRL_GAP_SIZE,
		lan9645x, QS_INJ_CTRL(1));

	lan_wr(SYS_PORT_MODE_INCL_INJ_HDR_SET(1),
	       lan9645x, SYS_PORT_MODE(CPU_PORT + 1));

	/* The CPU will only use its reserved buffer in the shared queue system
	 * and none of the shared buffer space, therefore we disable resource
	 * sharing in egress direction. We must not disable resource sharing in
	 * the ingress direction, because some traffic test scenarios require
	 * loads of buffer memory for frames initiated by the CPU.
	 */
	lan_rmw(QSYS_EGR_NO_SHARING_EGR_NO_SHARING_SET(BIT(CPU_PORT)),
		QSYS_EGR_NO_SHARING_EGR_NO_SHARING_SET(BIT(CPU_PORT)),
		lan9645x, QSYS_EGR_NO_SHARING);

	/* The CPU should also discard frames forwarded to it if it has run
	 * out of the reserved buffer space. Otherwise they will be held back
	 * in the ingress queues with potential head-of-line blocking effects.
	 */
	lan_rmw(QSYS_EGR_DROP_MODE_EGRESS_DROP_MODE_SET(BIT(CPU_PORT)),
		QSYS_EGR_DROP_MODE_EGRESS_DROP_MODE_SET(BIT(CPU_PORT)),
		lan9645x, QSYS_EGR_DROP_MODE);

	lan_wr(BIT(CPU_PORT), lan9645x, ANA_PGID(PGID_CPU));

	lan_rmw(ANA_PORT_CFG_PORTID_VAL_SET(CPU_PORT) |
		ANA_PORT_CFG_RECV_ENA_SET(1),
		ANA_PORT_CFG_PORTID_VAL |
		ANA_PORT_CFG_RECV_ENA, lan9645x,
		ANA_PORT_CFG(CPU_PORT));

	/* Enable switching to/from cpu port. Keep default aging-mode. */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1) |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG_SET(1) |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE_SET(1),
		QSYS_SW_PORT_MODE_PORT_ENA |
		QSYS_SW_PORT_MODE_SCH_NEXT_CFG |
		QSYS_SW_PORT_MODE_INGRESS_DROP_MODE,
		lan9645x, QSYS_SW_PORT_MODE(CPU_PORT));

	/* Transmit cpu frames as received without any tagging, timing or other
	 * updates. This does not affect CPU-over-NPI, only manual extraction.
	 * On the NPI port we need NO_REWRITE=0 for HSR/PRP.
	 */
	lan_wr(REW_PORT_CFG_NO_REWRITE_SET(1),
	       lan9645x, REW_PORT_CFG(CPU_PORT));
}

void lan9645x_port_set_tail_drop_wm(struct lan9645x *lan9645x)
{
	int shared_per_port;
	int port;

	/* Configure tail dropping watermark */
	shared_per_port =
		lan9645x->shared_queue_sz / (lan9645x->num_phys_ports + 1);

	/* The total memory size is diveded by number of front ports plus CPU
	 * port.
	 */
	lan9645x_for_each_chipport(lan9645x, port)
		lan_wr(lan9645x_wm_enc(shared_per_port), lan9645x,
		       SYS_ATOP(port));

	/* Tail dropping active based only on per port ATOP wm */
	lan_wr(lan9645x_wm_enc(lan9645x->shared_queue_sz), lan9645x,
	       SYS_ATOP_TOT_CFG);
}

int lan9645x_port_set_maxlen(struct lan9645x *lan9645x, int port, size_t sdu)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	int maxlen = sdu + ETH_HLEN + ETH_FCS_LEN;

	if (port == lan9645x->npi) {
		maxlen += LAN9645X_IFH_LEN;
		maxlen += LAN9645X_LONG_PREFIX_LEN;
	}

	lan_wr(DEV_MAC_MAXLEN_CFG_MAX_LEN_SET(maxlen), lan9645x,
	       DEV_MAC_MAXLEN_CFG(p->chip_port));

	/* Set Pause WM hysteresis */
	lan_rmw(SYS_PAUSE_CFG_PAUSE_STOP_SET(lan9645x_wm_enc(4 * maxlen)) |
		SYS_PAUSE_CFG_PAUSE_START_SET(lan9645x_wm_enc(6 * maxlen)),
		SYS_PAUSE_CFG_PAUSE_START |
		SYS_PAUSE_CFG_PAUSE_STOP,
		lan9645x,
		SYS_PAUSE_CFG(p->chip_port));

	return 0;
}

static int lan9645x_port_setup_leds(struct lan9645x *lan9645x,
				    struct fwnode_handle *portnp, int port)
{
	u32 drive_mode;
	int err;

	err = fwnode_property_read_u32(portnp, "microchip,led-drive-mode",
				       &drive_mode);
	if (err)
		return err;

	lan_rmw(CHIP_TOP_CUPHY_LED_CFG_LED_DRIVE_MODE_SET(drive_mode),
		CHIP_TOP_CUPHY_LED_CFG_LED_DRIVE_MODE, lan9645x,
		CHIP_TOP_CUPHY_LED_CFG(port));

	return 0;
}

static int lan9645x_port_parse_delays(struct lan9645x_port *port,
				      struct fwnode_handle *portnp)
{
	struct fwnode_handle *delay;
	int err;

	INIT_LIST_HEAD(&port->path_delays);

	fwnode_for_each_available_child_node(portnp, delay) {
		struct lan9645x_path_delay *path_delay;
		s32 tx_delay;
		s32 rx_delay;
		u32 speed;

		err = fwnode_property_read_u32(delay, "speed", &speed);
		if (err)
			return err;

		err = fwnode_property_read_u32(delay, "rx_delay", &rx_delay);
		if (err)
			return err;

		err = fwnode_property_read_u32(delay, "tx_delay", &tx_delay);
		if (err)
			return err;

		path_delay = devm_kzalloc(port->lan9645x->dev,
					  sizeof(*path_delay), GFP_KERNEL);
		if (!path_delay)
			return -ENOMEM;

		path_delay->rx_delay = rx_delay;
		path_delay->tx_delay = tx_delay;
		path_delay->speed = speed;
		list_add_tail(&path_delay->list, &port->path_delays);
	}

	return 0;
}

int lan9645x_port_parse_ports_node(struct lan9645x *lan9645x)
{
	struct fwnode_handle *ports, *portnp;
	struct device *dev = lan9645x->dev;
	int max_ports, num_ports = 0;
	int err = 0;

	max_ports = NUM_PHYS_PORTS - lan9645x->num_port_dis;

	ports = device_get_named_child_node(dev, "ethernet-ports");
	if (!ports)
		ports = device_get_named_child_node(dev, "ports");
	if (!ports) {
		dev_err(dev, "no ethernet-ports or ports child found\n");
		return -ENODEV;
	}

	fwnode_for_each_available_child_node(ports, portnp) {
		int phy_mode;
		u32 p;

		num_ports++;

		if (num_ports > max_ports) {
			dev_err(dev,
				"Too many ports in device tree. Max ports supported by SKU: %d\n",
				max_ports);
			err = -ENODEV;
			goto err_free_ports;
		}

		if (fwnode_property_read_u32(portnp, "reg", &p)) {
			dev_err(dev, "Port number not defined in device tree (property \"reg\")\n");
			err = -ENODEV;
			fwnode_handle_put(portnp);
			goto err_free_ports;
		}

		if (p >= lan9645x->num_phys_ports) {
			dev_err(dev,
				"Port number in device tree is invalid %u (property \"reg\")\n",
				p);
			err = -ENODEV;
			fwnode_handle_put(portnp);
			goto err_free_ports;
		}

		phy_mode = fwnode_get_phy_mode(portnp);
		if (phy_mode < 0) {
			dev_err(dev, "Failed to read phy-mode for port %u", p);
			err = -ENODEV;
			fwnode_handle_put(portnp);
			goto err_free_ports;
		}

		lan9645x->ports[p]->phy_mode = phy_mode;
		lan9645x_port_parse_delays(lan9645x->ports[p], portnp);
		lan9645x_port_setup_leds(lan9645x, portnp, p);
	}

err_free_ports:
	fwnode_handle_put(ports);
	return err;
}
