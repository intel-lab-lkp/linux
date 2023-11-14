// SPDX-License-Identifier: GPL-2.0
/*
 * Calibration procedure for the IPQ4019 PSGMII link
 *
 * Copyright (C) 2009 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2011-2012, 2020-2021 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (c) 2015, 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2016 John Crispin <john@phrozen.org>
 * Copyright (c) 2022 Robert Marko <robert.marko@sartura.hr>
 * Copyright (c) 2023 Romain Gantois <romain.gantois@bootlin.com>
 */

#include <linux/dsa/qca8k.h>
#include <linux/phylink.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/regmap.h>

#include "ipqess_port.h"
#include "ipqess_switch.h"

/* Nonstandard MII registers for the psgmii
 * device on the IPQ4019 MDIO bus.
 */

#define PSGMII_RSTCTRL      0x0     /* Reset control register */
#define PSGMII_RSTCTRL_RST  BIT(6)
#define PSGMII_RSTCTRL_RX20 BIT(2)  /* Fix/release RX 20 bit */

#define PSGMII_CDRCTRL         0x1a /* Clock and data recovery control register */
#define PSGMII_CDRCTRL_RELEASE BIT(12)

/* Retry policy */

#define PSGMII_CALIB_RETRIES        50
#define PSGMII_CALIB_RETRIES_BURST  5
#define PSGMII_CALIB_RETRY_DELAY    100

/* PSGMII PHY specific definitions */
#define PSGMII_VCO_CALIB_INTERVAL   1000000
#define PSGMII_VCO_CALIB_TIMEOUT    10000

static void ipqess_port_unprep_test(struct ipqess_port *port)
{
	struct qca8k_priv *priv = port->sw->priv;
	/* disable loopback on switch port */
	qca8k_clear_bits(priv, QCA8K_PORT_LOOKUP_CTRL(port->index),
			 QCA8K_PORT_LOOKUP_LOOPBACK_EN);

	qca8k_write(priv, QCA8K_REG_PORT_STATUS(port->index), 0);
	qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port->index),
		  QCA8K_PORT_LOOKUP_STATE_DISABLED,
		  QCA8K_PORT_LOOKUP_STATE_DISABLED);
}

static void ipqess_port_prep_test(struct ipqess_port *port)
{
	struct qca8k_priv *priv = port->sw->priv;

	qca8k_write(priv, QCA8K_REG_PORT_STATUS(port->index),
		    QCA8K_PORT_STATUS_SPEED_1000
		    | QCA8K_PORT_STATUS_TXMAC
		    | QCA8K_PORT_STATUS_RXMAC
		    | QCA8K_PORT_STATUS_DUPLEX);

	qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port->index),
		  QCA8K_PORT_LOOKUP_STATE_FORWARD,
		  QCA8K_PORT_LOOKUP_STATE_FORWARD);

	/* put switch port in loopback */
	qca8k_set_bits(priv, QCA8K_PORT_LOOKUP_CTRL(port->index),
		       QCA8K_PORT_LOOKUP_LOOPBACK_EN);
}

static int psgmii_vco_calibrate(struct ipqess_port *port)
{
	struct ipqess_switch *sw = port->sw;
	struct qca8k_priv *priv = sw->priv;
	struct ipqess_port *other_port;
	int val, ret, i;

	ret = phy_start_calibration(port->netdev->phydev);
	if (ret) {
		dev_err(priv->dev,
			"PHY VCO calibration PLL not ready\n");
		return ret;
	}

	/* Start PSGMIIPHY VCO PLL calibration */
	ret = regmap_set_bits(priv->psgmii,
			      PSGMIIPHY_VCO_CALIBRATION_CONTROL_REGISTER_1,
			      PSGMIIPHY_REG_PLL_VCO_CALIB_RESTART);

	/* Poll for PSGMIIPHY PLL calibration finish - Dakota(IPQ40xx) */
	ret = regmap_read_poll_timeout(priv->psgmii,
				       PSGMIIPHY_VCO_CALIBRATION_CONTROL_REGISTER_2,
				       val,
				       val & PSGMIIPHY_REG_PLL_VCO_CALIB_READY,
				       PSGMII_VCO_CALIB_INTERVAL,
				       PSGMII_VCO_CALIB_TIMEOUT);
	if (ret) {
		dev_err(priv->dev,
			"IPQ PSGMIIPHY VCO calibration PLL not ready\n");
		return ret;
	}

	/* Prepare all switch ports, in case we're dealing with a multiport PHY */
	for (i = 0; i < IPQESS_SWITCH_MAX_PORTS; i++) {
		other_port = sw->port_list[i];
		if (!other_port)
			continue;
		ipqess_port_prep_test(other_port);
	}

	ret = phy_stop_calibration(port->netdev->phydev);

	for (i = 0; i < IPQESS_SWITCH_MAX_PORTS; i++) {
		other_port = sw->port_list[i];
		if (!other_port)
			continue;
		ipqess_port_unprep_test(other_port);
	}

	qca8k_fdb_flush(priv);

	return ret;
}

int psgmii_calibrate_and_test(struct ipqess_port *port)
{
	int ret, attempt;

	for (attempt = 0; attempt <= PSGMII_CALIB_RETRIES; attempt++) {
		ret = psgmii_vco_calibrate(port);
		if (!ret) {
			netdev_dbg(port->netdev,
				   "PSGMII link stabilized after %d attempts\n",
				   attempt + 1);
			return 0;
		}

		/* On tested hardware, the link often stabilizes in 4 or 5 retries.
		 * If it still isn't stable, we wait a bit, then try another set
		 * of calibration attempts.
		 */
		netdev_dbg(port->netdev, "PSGMII link is unstable! Retrying... %d/QCA8K_PSGMII_CALIB_RETRIES\n",
			   attempt + 1);
		if (attempt % PSGMII_CALIB_RETRIES_BURST == 0)
			schedule_timeout_interruptible(msecs_to_jiffies(PSGMII_CALIB_RETRY_DELAY));
		else
			schedule();
	}

	netdev_err(port->netdev, "PSGMII work is unstable! Repeated recalibration attempts did not help!\n");
	return -EFAULT;
}
