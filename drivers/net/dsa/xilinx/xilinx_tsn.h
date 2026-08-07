/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver.
 */
#ifndef _XILINX_TSN_H
#define _XILINX_TSN_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/if_ether.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <net/dsa.h>

#define XLNX_TSN_NUM_PORTS	3
#define XLNX_TSN_CPU_PORT	0
#define XLNX_TSN_PORT_MAC1	1
#define XLNX_TSN_PORT_MAC2	2

/* Unicast Frame Filter: the switch accepts an incoming frame when
 * its destination MAC matches this 48-bit address under the 16-bit
 * mask in the MSB register's upper half. A set mask bit acts as a
 * wildcard for the corresponding bit of bytes 4..5.
 */
#define TSN_SW_MAC_LSB_OFFSET		0x0000c
#define TSN_SW_MAC_MSB_OFFSET		0x00010
#define TSN_SW_MAC_MSB_ADDR_MASK	GENMASK(15, 0)
#define TSN_SW_MAC_MSB_MASK_MASK	GENMASK(31, 16)

/* Mask value that covers the low nibble of byte 5, leaving a
 * 44-bit prefix common to all switch-port MACs. Those 4 bits are
 * filled in by the per-port MAC-nibble fields in the Switch Port
 * State Control register below.
 */
#define TSN_SW_MAC_NIBBLE_WILDCARD	0x000f

#define TSN_SW_STATUS_OFFSET		0x00000
/* Poll this before changing port state. */
#define TSN_SW_STATUS_READY		BIT(0)

/* Switch Port State Control register: packs per-port STP state,
 * change-commit bits, and MAC-nibble fields into one 32-bit word.
 */
#define TSN_PORT_STATE_CTRL_OFFSET	0x0004c

#define EP_PORT_STATUS_CHG_BIT		BIT(0)
#define EP_PORT_STATUS_MASK		GENMASK(3, 1)
#define MAC1_PORT_STATUS_CHG_BIT	BIT(8)
#define MAC1_PORT_STATUS_MASK		GENMASK(11, 9)
#define MAC2_PORT_STATUS_CHG_BIT	BIT(16)
#define MAC2_PORT_STATUS_MASK		GENMASK(19, 17)
#define EP_PORT_MAC_NIBBLE_MASK		GENMASK(7, 4)
#define MAC1_PORT_MAC_NIBBLE_MASK	GENMASK(15, 12)
#define MAC2_PORT_MAC_NIBBLE_MASK	GENMASK(23, 20)

#define TSN_SW_MGMT_QUEUING_OFFSET		0x00054
#define TSN_SW_MGMT_QUEUING_EP_SA_EGRESS	BIT(4)

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

/* Per-MAC receive / transmit / speed configuration registers. */
#define TSN_RCW1_OFFSET			0x00000404
#define TSN_RCW1_RX_EN			BIT(28)

#define TSN_TC_OFFSET			0x00000408
#define TSN_TC_TX_EN			BIT(28)

#define TSN_SPEED_CFG_OFFSET		0x00000410
#define TSN_SPEED_CFG_MASK		GENMASK(31, 30)
#define TSN_SPEED_CFG_100		BIT(30)
#define TSN_SPEED_CFG_1000		BIT(31)

/* PTP RTC timer block: a single block per IP, physically housed
 * inside MAC1's per-MAC reg window. Owned by the switch driver
 * because the PHC it backs is IP-wide, not per-MAC.
 */
#define TSN_TIMER_RTC_OFFSET_NS		0x00012800
#define TSN_TIMER_RTC_OFFSET_SEC_L	0x00012808
#define TSN_TIMER_RTC_OFFSET_SEC_H	0x0001280c
#define TSN_TIMER_RTC_INCREMENT		0x00012810
#define TSN_TIMER_CURRENT_RTC_NS	0x00012814
#define TSN_TIMER_CURRENT_RTC_SEC_L	0x00012818
#define TSN_TIMER_CURRENT_RTC_SEC_H	0x0001281c
#define TSN_TIMER_INTERRUPT		0x00012820

#define TSN_TIMER_MAX_NSEC_SIZE		30
#define TSN_TIMER_MAX_NSEC_MASK		GENMASK_ULL(TSN_TIMER_MAX_NSEC_SIZE - 1, 0)
#define TSN_TIMER_MAX_SEC_SIZE		48
#define TSN_TIMER_MAX_SEC_MASK		GENMASK_ULL(TSN_TIMER_MAX_SEC_SIZE - 1, 0)
#define TSN_TIMER_INT_CLEAR		BIT(0)
#define TSN_TIMER_RTC_NS_SHIFT		20
#define TSN_TIMER_PULSES_PER_PPS	128
#define TSN_TIMER_GTX_CLK_FREQ		125000000U

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
 * @conduit: DSA conduit netdev (EP MAC), used as the source of the
 *	     shared 44-bit frame-filter prefix
 * @mac_prefix: conduit MAC with byte 5's low nibble cleared to zero,
 *		forming the 44-bit prefix common to all switch-port MACs
 * @nb: netdev notifier that handles NETDEV_REGISTER on each swpN
 *	to set its final MAC, and NETDEV_CHANGEADDR on the conduit
 *	to refresh the shared prefix
 * @mac: per-MAC state, indexed by user-port number (index 0 unused;
 *	 MAC1 at [1], MAC2 at [2])
 * @ptp_timer_irq: 1 PPS / RTC-overflow interrupt
 * @ptp_clock: registered PHC; NULL until setup() succeeds
 * @ptp_clock_info: PHC capability + ops descriptor
 * @reg_lock: serialises RTC offset / increment register accesses
 *	      from process context and the PHC ops
 * @rtc_value: base RTC increment word for the GTX clock frequency,
 *	       used as the starting point for adjust_by_scaled_ppm()
 * @pps_enable: user requested PPS event delivery
 * @countpulse: timer-tick counter, reset to zero every TSN_TIMER_PULSES_PER_PPS ticks
 */
struct xlnx_tsn {
	struct dsa_switch ds;
	struct device *dev;
	void __iomem *sw_base;
	struct net_device *conduit;
	u8 mac_prefix[ETH_ALEN];
	struct notifier_block nb;
	struct xlnx_tsn_mac mac[XLNX_TSN_NUM_PORTS];
	int ptp_timer_irq;
	struct ptp_clock *ptp_clock;
	struct ptp_clock_info ptp_clock_info;
	spinlock_t reg_lock; /* serialises RTC offset/increment registers */
	u64 rtc_value;
	int pps_enable;
	int countpulse;
};

static inline void mac_iow(struct xlnx_tsn_mac *m, u32 off, u32 val)
{
	iowrite32(val, m->regs + off);
}

static inline u32 mac_ior(struct xlnx_tsn_mac *m, u32 off)
{
	return ioread32(m->regs + off);
}

int xlnx_tsn_ptp_init(struct xlnx_tsn *sw);
void xlnx_tsn_ptp_exit(struct xlnx_tsn *sw);

#endif /* _XILINX_TSN_H */
