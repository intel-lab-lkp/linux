// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <net/dsa.h>

#include "xilinx_tsn.h"

static enum dsa_tag_protocol xlnx_tsn_get_tag_protocol(struct dsa_switch *ds,
						       int port,
						       enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_XLNX_TSN;
}

static int xlnx_tsn_setup(struct dsa_switch *ds)
{
	return 0;
}

static void xlnx_tsn_teardown(struct dsa_switch *ds)
{
}

static const struct dsa_switch_ops xlnx_tsn_switch_ops = {
	.get_tag_protocol	= xlnx_tsn_get_tag_protocol,
	.setup			= xlnx_tsn_setup,
	.teardown		= xlnx_tsn_teardown,
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
