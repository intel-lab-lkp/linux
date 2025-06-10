// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2021-25 Sean Anderson <sean.anderson@seco.com>
 *
 * This is the driver for the Xilinx 1G/2.5G Ethernet PCS/PMA or SGMII LogiCORE
 * IP. A typical setup will look something like
 *
 * MAC <--GMII--> PCS/PMA <--1000BASE-X--> SFP module (PMD)
 *
 * The IEEE model mostly describes this device, but the PCS layer has a
 * separate sublayer for 8b/10b en/decoding:
 *
 * - When using a device-specific transceiver (serdes), the serdes handles 8b/10b
 *   en/decoding and PMA functions. The IP implements other PCS functions.
 * - When using LVDS IO resources, the IP implements PCS and PMA functions,
 *   including 8b/10b en/decoding and (de)serialization.
 * - When using an external serdes (accessed via TBI), the IP implements all
 *   PCS functions, including 8b/10b en/decoding.
 *
 * The link to the PMD is not modeled by this driver, except for refclk. It is
 * assumed that the serdes (if present) needs no configuration, though it
 * should be fairly easy to add support. It is also possible to go from SGMII
 * to GMII (PHY mode), but this is not supported.
 *
 * This driver was written with reference to PG047:
 * https://docs.amd.com/r/en-US/pg047-gig-eth-pcs-pma
 */

#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/of.h>
#include <linux/pcs.h>
#include <linux/phylink.h>
#include <linux/property.h>

#include "../phy/phy-caps.h"

/* Vendor-specific MDIO registers */
#define XILINX_PCS_ANICR 16 /* Auto-Negotiation Interrupt Control Register */
#define XILINX_PCS_SSR   17 /* Standard Selection Register */

#define XILINX_PCS_ANICR_IE BIT(0) /* Interrupt Enable */
#define XILINX_PCS_ANICR_IS BIT(1) /* Interrupt Status */

#define XILINX_PCS_SSR_SGMII BIT(0) /* Select SGMII standard */

/**
 * struct xilinx_pcs - Private data for Xilinx PCS devices
 * @pcs: The phylink PCS
 * @mdiodev: The mdiodevice used to access the PCS
 * @refclk: The reference clock for the PMD
 * @refclk_out: Optional reference clock for other PCSs using this PCS's shared
 *              logic
 * @reset: The reset line for the PCS
 * @done: Optional GPIO for reset_done
 * @irq: IRQ, or -EINVAL if polling
 * @enabled: Set if @pcs.link_change is valid and we can call phylink_pcs_change()
 */
struct xilinx_pcs {
	struct phylink_pcs pcs;
	struct clk_hw refclk_out;
	struct clk *refclk;
	struct gpio_desc *reset, *done;
	struct mdio_device *mdiodev;
	int irq;
	bool enabled;
};

static inline struct xilinx_pcs *pcs_to_xilinx(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct xilinx_pcs, pcs);
}

static irqreturn_t xilinx_pcs_an_irq(int irq, void *dev_id)
{
	struct xilinx_pcs *xp = dev_id;

	if (mdiodev_modify_changed(xp->mdiodev, XILINX_PCS_ANICR,
				   XILINX_PCS_ANICR_IS, 0) <= 0)
		return IRQ_NONE;

	/* paired with xilinx_pcs_enable/disable; protects xp->pcs->link_change */
	if (smp_load_acquire(&xp->enabled))
		phylink_pcs_change(&xp->pcs, true);
	return IRQ_HANDLED;
}

static int xilinx_pcs_enable(struct phylink_pcs *pcs)
{
	struct xilinx_pcs *xp = pcs_to_xilinx(pcs);
	struct device *dev = &xp->mdiodev->dev;
	int ret;

	if (xp->irq < 0)
		return 0;

	ret = mdiodev_modify(xp->mdiodev, XILINX_PCS_ANICR, 0,
			     XILINX_PCS_ANICR_IE);
	if (ret)
		dev_err(dev, "could not clear IRQ enable: %d\n", ret);
	else
		/* paired with xilinx_pcs_an_irq */
		smp_store_release(&xp->enabled, true);
	return ret;
}

static void xilinx_pcs_disable(struct phylink_pcs *pcs)
{
	struct xilinx_pcs *xp = pcs_to_xilinx(pcs);
	struct device *dev = &xp->mdiodev->dev;
	int err;

	if (xp->irq < 0)
		return;

	WRITE_ONCE(xp->enabled, false);
	/* paired with xilinx_pcs_an_irq */
	smp_wmb();

	err = mdiodev_modify(xp->mdiodev, XILINX_PCS_ANICR,
			     XILINX_PCS_ANICR_IE, 0);
	if (err)
		dev_err(dev, "could not clear IRQ enable: %d\n", err);
}

static __ETHTOOL_DECLARE_LINK_MODE_MASK(half_duplex) __ro_after_init;

static int xilinx_pcs_validate(struct phylink_pcs *pcs,
			       unsigned long *supported,
			       const struct phylink_link_state *state)
{
	linkmode_andnot(supported, supported, half_duplex);
	return 0;
}

static void xilinx_pcs_get_state(struct phylink_pcs *pcs,
				 unsigned int neg_mode,
				 struct phylink_link_state *state)
{
	struct xilinx_pcs *xp = pcs_to_xilinx(pcs);

	phylink_mii_c22_pcs_get_state(xp->mdiodev, neg_mode, state);
}

static int xilinx_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			     phy_interface_t interface,
			     const unsigned long *advertising,
			     bool permit_pause_to_mac)
{
	int ret, changed = 0;
	struct xilinx_pcs *xp = pcs_to_xilinx(pcs);

	if (test_bit(PHY_INTERFACE_MODE_SGMII, pcs->supported_interfaces) &&
	    test_bit(PHY_INTERFACE_MODE_1000BASEX, pcs->supported_interfaces)) {
		u16 ssr;

		if (interface == PHY_INTERFACE_MODE_SGMII)
			ssr = XILINX_PCS_SSR_SGMII;
		else
			ssr = 0;

		changed = mdiodev_modify_changed(xp->mdiodev, XILINX_PCS_SSR,
						 XILINX_PCS_SSR_SGMII, ssr);
		if (changed < 0)
			return changed;
	}

	ret = phylink_mii_c22_pcs_config(xp->mdiodev, interface, advertising,
					 neg_mode);
	return ret ?: changed;
}

static void xilinx_pcs_an_restart(struct phylink_pcs *pcs)
{
	struct xilinx_pcs *xp = pcs_to_xilinx(pcs);

	phylink_mii_c22_pcs_an_restart(xp->mdiodev);
}

static void xilinx_pcs_link_up(struct phylink_pcs *pcs, unsigned int mode,
			       phy_interface_t interface, int speed, int duplex)
{
	int bmcr;
	struct xilinx_pcs *xp = pcs_to_xilinx(pcs);

	if (phylink_autoneg_inband(mode))
		return;

	bmcr = mdiodev_read(xp->mdiodev, MII_BMCR);
	if (bmcr < 0) {
		dev_err(&xp->mdiodev->dev, "could not read BMCR (err=%d)\n",
			bmcr);
		return;
	}

	bmcr &= ~(BMCR_SPEED1000 | BMCR_SPEED100);
	switch (speed) {
	case SPEED_2500:
	case SPEED_1000:
		bmcr |= BMCR_SPEED1000;
		break;
	case SPEED_100:
		bmcr |= BMCR_SPEED100;
		break;
	case SPEED_10:
		bmcr |= BMCR_SPEED10;
		break;
	default:
		dev_err(&xp->mdiodev->dev, "invalid speed %d\n", speed);
	}

	bmcr = mdiodev_write(xp->mdiodev, MII_BMCR, bmcr);
	if (bmcr < 0)
		dev_err(&xp->mdiodev->dev, "could not write BMCR (err=%d)\n",
			bmcr);
}

static const struct phylink_pcs_ops xilinx_pcs_ops = {
	.pcs_validate = xilinx_pcs_validate,
	.pcs_enable = xilinx_pcs_enable,
	.pcs_disable = xilinx_pcs_disable,
	.pcs_get_state = xilinx_pcs_get_state,
	.pcs_config = xilinx_pcs_config,
	.pcs_an_restart = xilinx_pcs_an_restart,
	.pcs_link_up = xilinx_pcs_link_up,
};

static const struct clk_ops xilinx_pcs_clk_ops = { };

static const phy_interface_t xilinx_pcs_interfaces[] = {
	PHY_INTERFACE_MODE_SGMII,
	PHY_INTERFACE_MODE_1000BASEX,
	PHY_INTERFACE_MODE_2500BASEX,
};

static int xilinx_pcs_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct fwnode_handle *fwnode = dev->fwnode;
	int ret, i, j, mode_count;
	struct xilinx_pcs *xp;
	const char **modes;
	u32 phy_id;

	xp = devm_kzalloc(dev, sizeof(*xp), GFP_KERNEL);
	if (!xp)
		return -ENOMEM;
	xp->mdiodev = mdiodev;
	dev_set_drvdata(dev, xp);

	xp->irq = fwnode_irq_get_byname(fwnode, "an");
	/* There's no _optional variant, so this is the best we've got */
	if (xp->irq < 0 && xp->irq != -EINVAL)
		return dev_err_probe(dev, xp->irq, "could not get IRQ\n");

	mode_count = fwnode_property_string_array_count(fwnode,
							"xlnx,pcs-modes");
	if (!mode_count)
		mode_count = -ENODATA;
	if (mode_count < 0) {
		dev_err(dev, "could not read xlnx,pcs-modes: %d", mode_count);
		return mode_count;
	}

	modes = kcalloc(mode_count, sizeof(*modes), GFP_KERNEL);
	if (!modes)
		return -ENOMEM;

	ret = fwnode_property_read_string_array(fwnode, "xlnx,pcs-modes",
						modes, mode_count);
	if (ret < 0) {
		dev_err(dev, "could not read xlnx,pcs-modes: %d\n", ret);
		kfree(modes);
		return ret;
	}

	for (i = 0; i < mode_count; i++) {
		for (j = 0; j < ARRAY_SIZE(xilinx_pcs_interfaces); j++) {
			if (!strcmp(phy_modes(xilinx_pcs_interfaces[j]), modes[i])) {
				__set_bit(xilinx_pcs_interfaces[j],
					  xp->pcs.supported_interfaces);
				goto next;
			}
		}

		dev_err(dev, "invalid pcs-mode \"%s\"\n", modes[i]);
		kfree(modes);
		return -EINVAL;
next:
		;
	}

	kfree(modes);
	if ((test_bit(PHY_INTERFACE_MODE_SGMII, xp->pcs.supported_interfaces) ||
	     test_bit(PHY_INTERFACE_MODE_1000BASEX, xp->pcs.supported_interfaces)) &&
	    test_bit(PHY_INTERFACE_MODE_2500BASEX, xp->pcs.supported_interfaces)) {
		dev_err(dev,
			"Switching from SGMII or 1000Base-X to 2500Base-X not supported\n");
		return -EINVAL;
	}

	xp->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xp->reset))
		return dev_err_probe(dev, PTR_ERR(xp->reset),
				     "could not get reset gpio\n");

	xp->done = devm_gpiod_get_optional(dev, "done", GPIOD_IN);
	if (IS_ERR(xp->done))
		return dev_err_probe(dev, PTR_ERR(xp->done),
				     "could not get done gpio\n");

	xp->refclk = devm_clk_get_optional_enabled(dev, "refclk");
	if (IS_ERR(xp->refclk))
		return dev_err_probe(dev, PTR_ERR(xp->refclk),
				     "could not get/enable reference clock\n");

	gpiod_set_value_cansleep(xp->reset, 0);
	if (xp->done) {
		if (read_poll_timeout(gpiod_get_value_cansleep, ret, ret, 1000,
				      100000, true, xp->done))
			return dev_err_probe(dev, -ETIMEDOUT,
					     "timed out waiting for reset\n");
	} else {
		/* Just wait for a while and hope we're done */
		usleep_range(50000, 100000);
	}

	if (fwnode_property_present(fwnode, "#clock-cells")) {
		const char *parent = "refclk";
		struct clk_init_data init = {
			.name = fwnode_get_name(fwnode),
			.ops = &xilinx_pcs_clk_ops,
			.parent_names = &parent,
			.num_parents = 1,
			.flags = 0,
		};

		xp->refclk_out.init = &init;
		ret = devm_clk_hw_register(dev, &xp->refclk_out);
		if (ret)
			return dev_err_probe(dev, ret,
					     "could not register refclk\n");

		ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
						  &xp->refclk_out);
		if (ret)
			return dev_err_probe(dev, ret,
					     "could not register refclk\n");
	}

	/* Sanity check */
	ret = get_phy_c22_id(mdiodev->bus, mdiodev->addr, &phy_id);
	if (ret)
		return dev_err_probe(dev, ret, "could not read id\n");
	if ((phy_id & 0xfffffff0) != 0x01740c00)
		dev_warn(dev, "unknown phy id %x\n", phy_id);

	if (xp->irq < 0) {
		xp->pcs.poll = true;
	} else {
		/* The IRQ is enabled by default; turn it off */
		ret = mdiodev_write(xp->mdiodev, XILINX_PCS_ANICR, 0);
		if (ret) {
			dev_err(dev, "could not disable IRQ: %d\n", ret);
			return ret;
		}

		/* Some PCSs have a bad habit of re-enabling their IRQ!
		 * Request the IRQ in probe so we don't end up triggering the
		 * spurious IRQ logic.
		 */
		ret = devm_request_threaded_irq(dev, xp->irq, NULL, xilinx_pcs_an_irq,
						IRQF_SHARED | IRQF_ONESHOT,
						dev_name(dev), xp);
		if (ret) {
			dev_err(dev, "could not request IRQ: %d\n", ret);
			return ret;
		}
	}

	xp->pcs.ops = &xilinx_pcs_ops;
	ret = devm_pcs_register(dev, &xp->pcs);
	if (ret)
		return dev_err_probe(dev, ret, "could not register PCS\n");

	if (xp->irq < 0)
		dev_info(dev, "probed with irq=poll\n");
	else
		dev_info(dev, "probed with irq=%d\n", xp->irq);
	return 0;
}

static const struct of_device_id xilinx_pcs_of_match[] = {
	{ .compatible = "xlnx,pcs", },
	{},
};
MODULE_DEVICE_TABLE(of, xilinx_pcs_of_match);

static struct mdio_driver xilinx_pcs_driver = {
	.probe = xilinx_pcs_probe,
	.mdiodrv.driver = {
		.name = "xilinx-pcs",
		.of_match_table = of_match_ptr(xilinx_pcs_of_match),
	},
};

static int __init xilinx_pcs_init(void)
{
	phy_caps_linkmodes(LINK_CAPA_10HD | LINK_CAPA_100HD | LINK_CAPA_1000HD,
			   half_duplex);
	return mdio_driver_register(&xilinx_pcs_driver);
}
module_init(xilinx_pcs_init);

static void __exit xilinx_pcs_exit(void)
{
	mdio_driver_unregister(&xilinx_pcs_driver);
}
module_exit(xilinx_pcs_exit)

MODULE_ALIAS("platform:xilinx-pcs");
MODULE_DESCRIPTION("Xilinx PCS driver");
MODULE_LICENSE("GPL");
