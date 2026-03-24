// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2026 Microchip Technology Inc.
 */
#include <net/addrconf.h>

#include "lan9645x_main.h"

static void disable_conduit_ipv6(struct lan9645x *lan9645x,
				 struct net_device *conduit)
{
	struct inet6_dev *dev_v6;

	if (!conduit)
		return;

	/* IPv6 on the conduit will generate frames bypassing our tag driver, so
	 * they lack an IFH. This will be garbage in garbage out and we want to
	 * avoid this.
	 */
	rtnl_lock();
	dev_v6 = __in6_dev_get(conduit);
	if (dev_v6) {
		WRITE_ONCE(dev_v6->cnf.disable_ipv6, 1);
		dev_warn(lan9645x->dev, "Disabled IPv6 on conduit device: %s\n",
			 netdev_name(conduit));
	}
	rtnl_unlock();
}

void lan9645x_npi_port_init(struct lan9645x *lan9645x,
			    struct dsa_port *cpu_port)
{
	int port = cpu_port->index;
	struct lan9645x_port *p;

	p = lan9645x_to_port(lan9645x, port);
	lan9645x->npi = port;

	dev_dbg(lan9645x->dev, "NPI port=%d\n", port);

	/* Enabling IPv6 on the conduit will send frames directly on the
	 * interface, without being intercepted by our tag driver. This causes a
	 * GIGO situation.
	 */
	disable_conduit_ipv6(lan9645x, cpu_port->conduit);

	/* Any CPU extraction queue frames, are sent to external CPU on given
	 * port. Never send injected frames back to cpu.
	 */
	lan_wr(QSYS_EXT_CPU_CFG_EXT_CPUQ_MSK |
	       QSYS_EXT_CPU_CFG_EXT_CPU_PORT_SET(p->chip_port) |
	       QSYS_EXT_CPU_CFG_EXT_CPU_KILL_ENA_SET(1) |
	       QSYS_EXT_CPU_CFG_INT_CPU_KILL_ENA_SET(1),
	       lan9645x, QSYS_EXT_CPU_CFG);

	/* Configure IFH prefix mode for NPI port. */
	lan_rmw(SYS_PORT_MODE_INCL_XTR_HDR_SET(LAN9645X_TAG_PREFIX_LONG) |
		SYS_PORT_MODE_INCL_INJ_HDR_SET(LAN9645X_TAG_PREFIX_NONE),
		SYS_PORT_MODE_INCL_XTR_HDR |
		SYS_PORT_MODE_INCL_INJ_HDR,
		lan9645x,
		SYS_PORT_MODE(p->chip_port));

	/* Rewriting and extraction with IFH does not play nice together. A VLAN
	 * tag pushed into the frame by REW will cause 4 bytes at the end of the
	 * extraction header to be overwritten with the top 4 bytes of the DMAC.
	 *
	 * We can not use REW_PORT_CFG_NO_REWRITE=1 as that disabled RTAGD
	 * setting in the IFH
	 */
	lan_rmw(REW_TAG_CFG_TAG_CFG_SET(LAN9645X_TAG_DISABLED),
		REW_TAG_CFG_TAG_CFG, lan9645x, REW_TAG_CFG(port));

	/* Make sure frames with src_port=CPU_PORT are not reflected back via
	 * the NPI port. This could happen if a frame is flooded for instance.
	 * The *_CPU_KILL_ENA flags above only have an effect when a frame is
	 * output due to a CPU forwarding decision such as trapping or cpu copy.
	 */
	lan_rmw(0, BIT(port), lan9645x, ANA_PGID(PGID_SRC + CPU_PORT));
}

void lan9645x_npi_port_deinit(struct lan9645x *lan9645x, int port)
{
	struct lan9645x_port *p = lan9645x_to_port(lan9645x, port);

	lan9645x->npi = -1;

	lan_wr(QSYS_EXT_CPU_CFG_EXT_CPU_PORT_SET(0x1f) |
	       QSYS_EXT_CPU_CFG_EXT_CPU_KILL_ENA_SET(1) |
	       QSYS_EXT_CPU_CFG_INT_CPU_KILL_ENA_SET(1),
	       lan9645x, QSYS_EXT_CPU_CFG);

	lan_rmw(SYS_PORT_MODE_INCL_XTR_HDR_SET(LAN9645X_TAG_PREFIX_DISABLED) |
		SYS_PORT_MODE_INCL_INJ_HDR_SET(LAN9645X_TAG_PREFIX_DISABLED),
		SYS_PORT_MODE_INCL_XTR_HDR |
		SYS_PORT_MODE_INCL_INJ_HDR,
		lan9645x,
		SYS_PORT_MODE(p->chip_port));
}
