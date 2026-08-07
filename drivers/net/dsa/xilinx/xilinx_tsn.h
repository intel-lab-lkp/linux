/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver.
 */
#ifndef _XILINX_TSN_H
#define _XILINX_TSN_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/types.h>
#include <net/dsa.h>

#define XLNX_TSN_NUM_PORTS	3
#define XLNX_TSN_CPU_PORT	0
#define XLNX_TSN_PORT_MAC1	1
#define XLNX_TSN_PORT_MAC2	2

#define TSN_SW_STATUS_OFFSET		0x00000
/* Poll this before changing port state. */
#define TSN_SW_STATUS_READY		BIT(0)

/* Switch Port State Control register: packs per-port STP state and
 * change-commit bits into one 32-bit word.
 */
#define TSN_PORT_STATE_CTRL_OFFSET	0x0004c

#define EP_PORT_STATUS_CHG_BIT		BIT(0)
#define EP_PORT_STATUS_MASK		GENMASK(3, 1)
#define MAC1_PORT_STATUS_CHG_BIT	BIT(8)
#define MAC1_PORT_STATUS_MASK		GENMASK(11, 9)
#define MAC2_PORT_STATUS_CHG_BIT	BIT(16)
#define MAC2_PORT_STATUS_MASK		GENMASK(19, 17)

/* readl_poll_timeout() parameters (in microseconds): poll until
 * the port-state change-commit bit self-clears.
 */
#define TSN_SW_POLL_DELAY_US		10
#define TSN_SW_POLL_TIMEOUT_US		5000

enum tsn_port_state {
	TSN_PORT_STATE_DISABLED = 0,
	TSN_PORT_STATE_BLOCKING,
	TSN_PORT_STATE_LISTENING,
	TSN_PORT_STATE_LEARNING,
	TSN_PORT_STATE_FORWARDING,
};

/* Per-MAC MDIO controller register window, sitting at +0x500 inside
 * each MAC's reg space owned via reg-names = "mac1", "mac2".
 */
#define TSN_MDIO_MC_OFFSET		0x00000500
#define TSN_MDIO_MCR_OFFSET		0x00000504
#define TSN_MDIO_MWD_OFFSET		0x00000508
#define TSN_MDIO_MRD_OFFSET		0x0000050c

#define TSN_MDIO_MC_MDIOEN		BIT(6)
#define TSN_MDIO_MC_CLOCK_DIVIDE_MAX	0x3f

#define TSN_MDIO_MCR_PHYAD_MASK		GENMASK(28, 24)
#define TSN_MDIO_MCR_REGAD_MASK		GENMASK(20, 16)
#define TSN_MDIO_MCR_OP_READ		BIT(15)
#define TSN_MDIO_MCR_OP_WRITE		BIT(14)
#define TSN_MDIO_MCR_INITIATE		BIT(11)
#define TSN_MDIO_MCR_READY		BIT(7)

#define TSN_MDIO_MRD_MASK		GENMASK(15, 0)

struct mii_bus;
struct xlnx_tsn;

/**
 * struct xlnx_tsn_mac - per-MAC switch-side state
 * @sw: back-pointer to the parent switch (for dev_* logging in
 *	bus callbacks)
 * @regs: per-MAC register window, from reg-name "macN"
 * @mii_bus: MDIO bus registered under the "mdio-macN" DT child,
 *	     or NULL if absent
 */
struct xlnx_tsn_mac {
	struct xlnx_tsn *sw;
	void __iomem *regs;
	struct mii_bus *mii_bus;
};

/**
 * struct xlnx_tsn - per-IP switch state
 * @ds: DSA switch
 * @dev: backing device
 * @sw_base: switch fabric register window
 * @mac: per-MAC state, indexed by user-port number (index 0 unused;
 *	 MAC1 at [1], MAC2 at [2])
 */
struct xlnx_tsn {
	struct dsa_switch ds;
	struct device *dev;
	void __iomem *sw_base;
	struct xlnx_tsn_mac mac[XLNX_TSN_NUM_PORTS];
};

static inline void mac_iow(struct xlnx_tsn_mac *m, u32 off, u32 val)
{
	iowrite32(val, m->regs + off);
}

static inline u32 mac_ior(struct xlnx_tsn_mac *m, u32 off)
{
	return ioread32(m->regs + off);
}

#endif /* _XILINX_TSN_H */
