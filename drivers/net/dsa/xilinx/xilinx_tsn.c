// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/if_bridge.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <net/dsa.h>

#include "xilinx_tsn.h"

#define TSN_MDIO_MAX_FREQ_HZ		2500000
#define TSN_MDIO_READY_TIMEOUT_US	20000

static void sw_iow(struct xlnx_tsn *sw, u32 off, u32 val)
{
	iowrite32(val, sw->sw_base + off);
}

static u32 sw_ior(struct xlnx_tsn *sw, u32 off)
{
	return ioread32(sw->sw_base + off);
}

static int xlnx_tsn_switch_status_ready(struct xlnx_tsn *sw)
{
	u32 reg;

	return readl_poll_timeout(sw->sw_base + TSN_SW_STATUS_OFFSET, reg,
				  reg & TSN_SW_STATUS_READY,
				  TSN_SW_POLL_DELAY_US, TSN_SW_POLL_TIMEOUT_US);
}

static int xlnx_tsn_port_state_bits(int port, u32 *mask, u32 *chg_bit)
{
	switch (port) {
	case XLNX_TSN_CPU_PORT:
		*mask = EP_PORT_STATUS_MASK;
		*chg_bit = EP_PORT_STATUS_CHG_BIT;
		return 0;
	case XLNX_TSN_PORT_MAC1:
		*mask = MAC1_PORT_STATUS_MASK;
		*chg_bit = MAC1_PORT_STATUS_CHG_BIT;
		return 0;
	case XLNX_TSN_PORT_MAC2:
		*mask = MAC2_PORT_STATUS_MASK;
		*chg_bit = MAC2_PORT_STATUS_CHG_BIT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int xlnx_tsn_set_port_state(struct xlnx_tsn *sw, int port,
				   enum tsn_port_state state)
{
	u32 chg_bit, mask, val, reg;
	int err;

	err = xlnx_tsn_port_state_bits(port, &mask, &chg_bit);
	if (err)
		return err;

	err = xlnx_tsn_switch_status_ready(sw);
	if (err) {
		dev_err(sw->dev, "port %d: switch not ready for state change\n",
			port);
		return err;
	}

	/* Bit won't re-arm if a previous change never cleared it. */
	val = sw_ior(sw, TSN_PORT_STATE_CTRL_OFFSET);
	if (val & chg_bit) {
		dev_err(sw->dev, "port %d: previous state change still pending\n",
			port);
		return -EBUSY;
	}

	val &= ~mask;
	val |= (state << __ffs(mask)) & mask;
	val |= chg_bit;
	sw_iow(sw, TSN_PORT_STATE_CTRL_OFFSET, val);

	err = readl_poll_timeout(sw->sw_base + TSN_PORT_STATE_CTRL_OFFSET, reg,
				 !(reg & chg_bit), TSN_SW_POLL_DELAY_US,
				 TSN_SW_POLL_TIMEOUT_US);
	if (err) {
		dev_err(sw->dev, "port %d: state change ack timed out\n", port);
		return -ETIMEDOUT;
	}

	return 0;
}

static int xlnx_tsn_mdio_wait_ready(struct xlnx_tsn_mac *m)
{
	u32 val;

	return readl_poll_timeout(m->regs + TSN_MDIO_MCR_OFFSET, val,
				  val & TSN_MDIO_MCR_READY, 1,
				  TSN_MDIO_READY_TIMEOUT_US);
}

static int xlnx_tsn_mdio_read(struct mii_bus *bus, int phy_id, int reg)
{
	struct xlnx_tsn_mac *m = bus->priv;
	int ret;

	ret = xlnx_tsn_mdio_wait_ready(m);
	if (ret < 0)
		return ret;

	mac_iow(m, TSN_MDIO_MCR_OFFSET,
		FIELD_PREP(TSN_MDIO_MCR_PHYAD_MASK, phy_id) |
		FIELD_PREP(TSN_MDIO_MCR_REGAD_MASK, reg) |
		TSN_MDIO_MCR_INITIATE | TSN_MDIO_MCR_OP_READ);

	ret = xlnx_tsn_mdio_wait_ready(m);
	if (ret < 0)
		return ret;

	return FIELD_GET(TSN_MDIO_MRD_MASK,
			 mac_ior(m, TSN_MDIO_MRD_OFFSET));
}

static int xlnx_tsn_mdio_write(struct mii_bus *bus, int phy_id, int reg,
			       u16 val)
{
	struct xlnx_tsn_mac *m = bus->priv;
	int ret;

	ret = xlnx_tsn_mdio_wait_ready(m);
	if (ret < 0)
		return ret;

	mac_iow(m, TSN_MDIO_MWD_OFFSET, val);
	mac_iow(m, TSN_MDIO_MCR_OFFSET,
		FIELD_PREP(TSN_MDIO_MCR_PHYAD_MASK, phy_id) |
		FIELD_PREP(TSN_MDIO_MCR_REGAD_MASK, reg) |
		TSN_MDIO_MCR_INITIATE | TSN_MDIO_MCR_OP_WRITE);

	return xlnx_tsn_mdio_wait_ready(m);
}

/* Round up so the MDC frequency stays at or below TSN_MDIO_MAX_FREQ_HZ,
 * then clamp to the 6-bit field maximum so the value stays within the
 * field and does not corrupt TSN_MDIO_MC_MDIOEN.
 */
static u32 xlnx_tsn_mdio_clk_div(struct xlnx_tsn *sw, unsigned long host_hz)
{
	u32 div;

	if (!host_hz) {
		dev_warn(sw->dev,
			 "s_axi clock rate unknown; clamping MDIO divisor to max\n");
		return TSN_MDIO_MC_CLOCK_DIVIDE_MAX;
	}

	div = DIV_ROUND_UP(host_hz, TSN_MDIO_MAX_FREQ_HZ * 2) - 1;

	/* HW ignores MDIO Enable when Clock Divide is 0 */
	if (!div)
		div = 1;

	if (div > TSN_MDIO_MC_CLOCK_DIVIDE_MAX) {
		dev_warn(sw->dev,
			 "MDIO divisor %u exceeds max %u, clamping\n",
			 div, TSN_MDIO_MC_CLOCK_DIVIDE_MAX);
		div = TSN_MDIO_MC_CLOCK_DIVIDE_MAX;
	}

	return div;
}

static int xlnx_tsn_mdio_register_one(struct xlnx_tsn *sw, int port,
				      const char *child_name,
				      unsigned long host_hz)
{
	struct xlnx_tsn_mac *m = &sw->mac[port];
	struct device_node *mdio_np;
	struct mii_bus *bus;
	int ret;

	mdio_np = of_get_child_by_name(sw->dev->of_node, child_name);
	if (!mdio_np)
		return 0;

	bus = devm_mdiobus_alloc(sw->dev);
	if (!bus) {
		of_node_put(mdio_np);
		return -ENOMEM;
	}

	snprintf(bus->id, MII_BUS_ID_SIZE, "%s:%s",
		 dev_name(sw->dev), child_name);
	bus->name = "Xilinx TSN MDIO";
	bus->priv = m;
	bus->parent = sw->dev;
	bus->read = xlnx_tsn_mdio_read;
	bus->write = xlnx_tsn_mdio_write;

	mac_iow(m, TSN_MDIO_MC_OFFSET,
		xlnx_tsn_mdio_clk_div(sw, host_hz) | TSN_MDIO_MC_MDIOEN);

	ret = xlnx_tsn_mdio_wait_ready(m);
	if (ret) {
		dev_err(sw->dev, "%s: MDIO controller not ready: %d\n",
			child_name, ret);
		goto err_put_np;
	}

	ret = of_mdiobus_register(bus, mdio_np);
	if (ret) {
		dev_err(sw->dev, "%s: failed to register MDIO bus: %d\n",
			child_name, ret);
		goto err_put_np;
	}

	m->mii_bus = bus;
	of_node_put(mdio_np);
	return 0;

err_put_np:
	of_node_put(mdio_np);
	return ret;
}

static void xlnx_tsn_mdio_unregister_all(struct xlnx_tsn *sw)
{
	int port;

	for (port = XLNX_TSN_PORT_MAC1; port <= XLNX_TSN_PORT_MAC2; port++) {
		struct xlnx_tsn_mac *m = &sw->mac[port];

		if (m->mii_bus) {
			mdiobus_unregister(m->mii_bus);
			m->mii_bus = NULL;
		}

		/* clear the enable bit even when no bus was registered (failed probe) */
		mac_iow(m, TSN_MDIO_MC_OFFSET, 0);
	}
}

static int xlnx_tsn_mdio_register_all(struct xlnx_tsn *sw)
{
	unsigned long host_hz;
	struct clk *s_axi;
	int ret;

	/* per-MAC MDIO divisor comes from the wrapper node's s_axi
	 * clock
	 */
	s_axi = clk_get(sw->dev->parent, "s_axi");
	if (IS_ERR(s_axi))
		return dev_err_probe(sw->dev, PTR_ERR(s_axi),
				     "failed to get s_axi clock\n");

	host_hz = clk_get_rate(s_axi);
	clk_put(s_axi);

	ret = xlnx_tsn_mdio_register_one(sw, XLNX_TSN_PORT_MAC1, "mdio-mac1",
					 host_hz);
	if (ret)
		goto err_unregister;

	ret = xlnx_tsn_mdio_register_one(sw, XLNX_TSN_PORT_MAC2, "mdio-mac2",
					 host_hz);
	if (ret)
		goto err_unregister;

	return 0;

err_unregister:
	xlnx_tsn_mdio_unregister_all(sw);
	return ret;
}

static enum dsa_tag_protocol xlnx_tsn_get_tag_protocol(struct dsa_switch *ds,
						       int port,
						       enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_XLNX_TSN;
}

static void xlnx_tsn_port_stp_state_set(struct dsa_switch *ds, int port,
					u8 state)
{
	struct xlnx_tsn *sw = ds->priv;
	enum tsn_port_state hw_state;

	switch (state) {
	case BR_STATE_DISABLED:
		hw_state = TSN_PORT_STATE_DISABLED;
		break;
	case BR_STATE_BLOCKING:
		hw_state = TSN_PORT_STATE_BLOCKING;
		break;
	case BR_STATE_LISTENING:
		hw_state = TSN_PORT_STATE_LISTENING;
		break;
	case BR_STATE_LEARNING:
		hw_state = TSN_PORT_STATE_LEARNING;
		break;
	case BR_STATE_FORWARDING:
		hw_state = TSN_PORT_STATE_FORWARDING;
		break;
	default:
		dev_warn(sw->dev, "port %d: unsupported STP state %u\n",
			 port, state);
		return;
	}

	xlnx_tsn_set_port_state(sw, port, hw_state);
}

static void xlnx_tsn_phylink_get_caps(struct dsa_switch *ds, int port,
				      struct phylink_config *config)
{
	if (port == XLNX_TSN_CPU_PORT) {
		config->mac_capabilities = MAC_1000FD;
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		return;
	}

	/* The MAC's speed-config field only encodes 100 / 1000.
	 * Half-duplex and 10 Mbps are not supported.
	 */
	config->mac_capabilities = MAC_100FD | MAC_1000FD;
	phy_interface_set_rgmii(config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
}

static void xlnx_tsn_mac_config(struct phylink_config *config,
				unsigned int mode,
				const struct phylink_link_state *state)
{
	/* Interface mode (RGMII / SGMII / GMII) is fixed at IP synthesis
	 * time. There is no runtime register to program it here.
	 */
}

static void xlnx_tsn_mac_link_down(struct phylink_config *config,
				   unsigned int mode,
				   phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct xlnx_tsn *sw = dp->ds->priv;
	struct xlnx_tsn_mac *m;
	u32 rcw1, tc;

	m = &sw->mac[dp->index];

	/* The CPU port has no switch-side MAC registers. Nothing to
	 * tear down here (see xlnx_tsn_mac_link_up).
	 */
	if (dp->index == XLNX_TSN_CPU_PORT)
		return;

	tc = mac_ior(m, TSN_TC_OFFSET) & ~TSN_TC_TX_EN;
	mac_iow(m, TSN_TC_OFFSET, tc);

	rcw1 = mac_ior(m, TSN_RCW1_OFFSET) & ~TSN_RCW1_RX_EN;
	mac_iow(m, TSN_RCW1_OFFSET, rcw1);
}

static void xlnx_tsn_mac_link_up(struct phylink_config *config,
				 struct phy_device *phy, unsigned int mode,
				 phy_interface_t interface, int speed,
				 int duplex, bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct xlnx_tsn *sw = dp->ds->priv;
	u32 speed_cfg, rcw1, tc;
	struct xlnx_tsn_mac *m;

	m = &sw->mac[dp->index];

	/* The CPU port is the internal endpoint MAC. It has no switch-side
	 * MAC registers and is managed by the endpoint driver, not here.
	 */
	if (dp->index == XLNX_TSN_CPU_PORT)
		return;

	speed_cfg = mac_ior(m, TSN_SPEED_CFG_OFFSET) & ~TSN_SPEED_CFG_MASK;
	switch (speed) {
	case SPEED_1000:
		speed_cfg |= TSN_SPEED_CFG_1000;
		break;
	case SPEED_100:
		speed_cfg |= TSN_SPEED_CFG_100;
		break;
	default:
		dev_warn(sw->dev,
			 "port %d: unsupported link speed %d Mbps\n",
			 dp->index, speed);
		return;
	}
	mac_iow(m, TSN_SPEED_CFG_OFFSET, speed_cfg);

	rcw1 = mac_ior(m, TSN_RCW1_OFFSET) | TSN_RCW1_RX_EN;
	mac_iow(m, TSN_RCW1_OFFSET, rcw1);

	tc = mac_ior(m, TSN_TC_OFFSET) | TSN_TC_TX_EN;
	mac_iow(m, TSN_TC_OFFSET, tc);
}

static const struct phylink_mac_ops xlnx_tsn_phylink_mac_ops = {
	.mac_config	= xlnx_tsn_mac_config,
	.mac_link_up	= xlnx_tsn_mac_link_up,
	.mac_link_down	= xlnx_tsn_mac_link_down,
};

static int xlnx_tsn_setup(struct dsa_switch *ds)
{
	struct xlnx_tsn *sw = ds->priv;
	struct dsa_port *dp;
	int ret;

	if (!dsa_is_user_port(ds, XLNX_TSN_PORT_MAC1) ||
	    !dsa_is_user_port(ds, XLNX_TSN_PORT_MAC2))
		return dev_err_probe(sw->dev, -EINVAL,
				     "both MAC1 and MAC2 must be enabled as switch ports\n");

	/* CPU port stays in FORWARDING so host traffic always flows.
	 * User ports start in DISABLED and transition from there under
	 * bridge STP control.
	 */
	ret = xlnx_tsn_set_port_state(sw, XLNX_TSN_CPU_PORT,
				      TSN_PORT_STATE_FORWARDING);
	if (ret)
		return ret;

	dsa_switch_for_each_user_port(dp, ds) {
		ret = xlnx_tsn_set_port_state(sw, dp->index,
					      TSN_PORT_STATE_DISABLED);
		if (ret)
			return ret;
	}

	return xlnx_tsn_mdio_register_all(sw);
}

static void xlnx_tsn_teardown(struct dsa_switch *ds)
{
	struct xlnx_tsn *sw = ds->priv;
	struct dsa_port *dp;

	xlnx_tsn_mdio_unregister_all(sw);

	dsa_switch_for_each_user_port(dp, ds)
		xlnx_tsn_set_port_state(sw, dp->index, TSN_PORT_STATE_DISABLED);

	xlnx_tsn_set_port_state(sw, XLNX_TSN_CPU_PORT, TSN_PORT_STATE_DISABLED);
}

static const struct dsa_switch_ops xlnx_tsn_switch_ops = {
	.get_tag_protocol	= xlnx_tsn_get_tag_protocol,
	.setup			= xlnx_tsn_setup,
	.teardown		= xlnx_tsn_teardown,
	.port_stp_state_set	= xlnx_tsn_port_stp_state_set,
	.phylink_get_caps	= xlnx_tsn_phylink_get_caps,
};

static int xlnx_tsn_map_reg(struct platform_device *pdev, const char *name,
			    void __iomem **out)
{
	void __iomem *base;

	base = devm_platform_ioremap_resource_byname(pdev, name);
	if (IS_ERR(base))
		return dev_err_probe(&pdev->dev, PTR_ERR(base),
				     "failed to map %s reg window\n", name);

	*out = base;
	return 0;
}

static int xlnx_tsn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dsa_switch *ds;
	struct xlnx_tsn *sw;
	int ret;

	sw = devm_kzalloc(dev, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return -ENOMEM;

	sw->dev = dev;
	sw->mac[XLNX_TSN_PORT_MAC1].sw = sw;
	sw->mac[XLNX_TSN_PORT_MAC2].sw = sw;

	ret = xlnx_tsn_map_reg(pdev, "switch", &sw->sw_base);
	if (ret)
		return ret;

	ret = xlnx_tsn_map_reg(pdev, "mac1",
			       &sw->mac[XLNX_TSN_PORT_MAC1].regs);
	if (ret)
		return ret;

	ret = xlnx_tsn_map_reg(pdev, "mac2",
			       &sw->mac[XLNX_TSN_PORT_MAC2].regs);
	if (ret)
		return ret;

	ds = &sw->ds;
	ds->dev = dev;
	ds->num_ports = XLNX_TSN_NUM_PORTS;
	ds->ops = &xlnx_tsn_switch_ops;
	ds->phylink_mac_ops = &xlnx_tsn_phylink_mac_ops;
	ds->priv = sw;

	platform_set_drvdata(pdev, sw);

	return dsa_register_switch(ds);
}

static void xlnx_tsn_remove(struct platform_device *pdev)
{
	struct xlnx_tsn *sw = platform_get_drvdata(pdev);

	if (!sw)
		return;

	dsa_unregister_switch(&sw->ds);
}

static void xlnx_tsn_shutdown(struct platform_device *pdev)
{
	struct xlnx_tsn *sw = platform_get_drvdata(pdev);

	if (!sw)
		return;

	dsa_switch_shutdown(&sw->ds);
	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id xlnx_tsn_of_match[] = {
	{ .compatible = "xlnx,tsn-switch" },
	{ }
};
MODULE_DEVICE_TABLE(of, xlnx_tsn_of_match);

static struct platform_driver xlnx_tsn_driver = {
	.driver = {
		.name		= "xlnx-tsn-switch",
		.of_match_table	= xlnx_tsn_of_match,
	},
	.probe		= xlnx_tsn_probe,
	.remove		= xlnx_tsn_remove,
	.shutdown	= xlnx_tsn_shutdown,
};
module_platform_driver(xlnx_tsn_driver);

MODULE_AUTHOR("Nagadheeraj Rottela <nagadheeraj.rottela@amd.com>");
MODULE_DESCRIPTION("AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver");
MODULE_LICENSE("GPL");
