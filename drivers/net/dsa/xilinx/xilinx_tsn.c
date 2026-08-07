// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver.
 */

#include <linux/bitfield.h>
#include <linux/if_bridge.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <net/dsa.h>

#include "xilinx_tsn.h"

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

	return 0;
}

static void xlnx_tsn_teardown(struct dsa_switch *ds)
{
	struct xlnx_tsn *sw = ds->priv;
	struct dsa_port *dp;

	dsa_switch_for_each_user_port(dp, ds)
		xlnx_tsn_set_port_state(sw, dp->index, TSN_PORT_STATE_DISABLED);

	xlnx_tsn_set_port_state(sw, XLNX_TSN_CPU_PORT, TSN_PORT_STATE_DISABLED);
}

static const struct dsa_switch_ops xlnx_tsn_switch_ops = {
	.get_tag_protocol	= xlnx_tsn_get_tag_protocol,
	.setup			= xlnx_tsn_setup,
	.teardown		= xlnx_tsn_teardown,
	.port_stp_state_set	= xlnx_tsn_port_stp_state_set,
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

	ret = xlnx_tsn_map_reg(pdev, "switch", &sw->sw_base);
	if (ret)
		return ret;

	ret = xlnx_tsn_map_reg(pdev, "mac1", &sw->mac_base[XLNX_TSN_PORT_MAC1]);
	if (ret)
		return ret;

	ret = xlnx_tsn_map_reg(pdev, "mac2", &sw->mac_base[XLNX_TSN_PORT_MAC2]);
	if (ret)
		return ret;

	ds = &sw->ds;
	ds->dev = dev;
	ds->num_ports = XLNX_TSN_NUM_PORTS;
	ds->ops = &xlnx_tsn_switch_ops;
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
