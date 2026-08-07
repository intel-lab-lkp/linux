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
#include <linux/net_tstamp.h>
#include <linux/notifier.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/dsa/xlnx_tsn.h>
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

/* Shared readl_poll_timeout() parameters (in microseconds) used at
 * two call sites: the port-state change-commit bit and the CAM
 * operation enable bit. Both self-clear when the operation completes.
 */
#define TSN_SW_POLL_DELAY_US		10
#define TSN_SW_POLL_TIMEOUT_US		5000

/* Stream-destination-lookup CAM: maps a (destination MAC, VLAN ID) key
 * to an egress port list, backing the bridge FDB. Access is indirect:
 * load the key, value, and port-list registers, write the opcode, then
 * set the Enable Operation bit in the control register. Poll CAM Init
 * Done in the status register to 1 before each operation to confirm the
 * block is ready. Enable Operation self-clears when the operation
 * finishes.
 */
#define TSN_CAM_CTRL_OFFSET		0x01000
#define TSN_CAM_STATUS_OFFSET		0x01004
#define TSN_CAM_KEY1_OFFSET		0x01008
#define TSN_CAM_KEY2_OFFSET		0x0100c
#define TSN_CAM_TV1_OFFSET		0x01010
#define TSN_CAM_TV2_OFFSET		0x01014
#define TSN_CAM_PORT_ACT_OFFSET		0x01018

/* Control register: set to start an operation, self-clears when done. */
#define TSN_CAM_OP_ENABLE		BIT(0)
/* Status register: reads 1 when the CAM is ready for the next operation. */
#define TSN_CAM_STATUS_READY		BIT(0)
#define TSN_CAM_OP_MASK			GENMASK(2, 1)
#define TSN_CAM_OP_READ_KEY		0x0
#define TSN_CAM_OP_ADD			0x1
#define TSN_CAM_OP_DELETE		0x2
#define TSN_CAM_OP_READ			0x3
#define TSN_CAM_FOUND			BIT(7)
#define TSN_CAM_VLAN			GENMASK(27, 16)
#define TSN_CAM_PORT_LIST		GENMASK(10, 8)
#define TSN_CAM_READ_KEY_ADDR		GENMASK(19, 8)
#define TSN_CAM_MAC2_READ_KEY_BASE	0x800
#define TSN_CAM_READ_KEY_COUNT		2048

/* HW reset-default native VID; DSA's "no VLAN" (vid 0) maps onto it. */
#define TSN_SW_DEFAULT_VID		1

/* Switch Control Register: switch-wide control fields. */
#define TSN_SW_CTRL_OFFSET		0x00004
/* Per-port native-VLAN untag enables: strip the tag on egress when the
 * frame VID equals the egress port's native VID. Only the wire ports are
 * untagged; the endpoint port is left tagged so the host keeps the VID for
 * software classification.
 */
#define TSN_SW_CTRL_MAC1_NATIVE_UNTAG_EN	BIT(18)
#define TSN_SW_CTRL_MAC2_NATIVE_UNTAG_EN	BIT(19)
/* Action for an ingress frame whose port is not in its VLAN's member
 * list: forward to the processor or discard. Pinned to discard. This only
 * matters once a VID's Port-List-Valid bit is set, which happens only
 * while VLAN filtering is on.
 */
#define TSN_SW_CTRL_MEMBER_VIOL_MASK		GENMASK(9, 8)
#define TSN_SW_CTRL_MEMBER_VIOL_DISCARD		0x1

/* Forwarding action for a tagged unicast or multicast frame that misses
 * both the CAM and the VLAN membership list: flood, to processor, to MAC,
 * or discard.
 */
#define TSN_SW_CTRL_UCAST_MISS_MASK		GENMASK(1, 0)
#define TSN_SW_CTRL_MCAST_MISS_MASK		GENMASK(3, 2)
#define TSN_SW_CTRL_UCAST_MISS_FLOOD		0x1
#define TSN_SW_CTRL_MCAST_MISS_FLOOD		0x1
#define TSN_SW_CTRL_MISS_DISCARD		0x3

/* Hardware Address Learning Control: switch-wide learning behaviour. */
#define TSN_SW_ADDR_LEARN_OFFSET		0x00048
/* Disable the global learning engine; set to 1 while every user port is
 * standalone, cleared to 0 when at least one port joins a bridge. The
 * sub-qualifier bits below are inert while this bit is set.
 */
#define TSN_SW_ADDR_LEARN_DISABLE		BIT(0)
/* Learn the source MAC of untagged and priority-tagged frames against the
 * ingress port's native VID; the reset default excludes them from learning.
 */
#define TSN_SW_ADDR_LEARN_UNTAGGED_EN		BIT(1)
/* Permit learning on a VID that has no member entry programmed in the VLAN
 * membership memory; the reset default only learns already-configured VIDs.
 */
#define TSN_SW_ADDR_LEARN_NO_VLAN_EN		BIT(3)

/* Map a DSA port index to its bit in a switch port list. */
#define TSN_PORT_BIT(port)		BIT(port)

enum tsn_port_state {
	TSN_PORT_STATE_DISABLED = 0,
	TSN_PORT_STATE_BLOCKING,
	TSN_PORT_STATE_LISTENING,
	TSN_PORT_STATE_LEARNING,
	TSN_PORT_STATE_FORWARDING,
	/* Not an STP state. Writing it flushes the port's dynamic learnt
	 * entries and leaves static FDB entries in place.
	 */
	TSN_PORT_STATE_FLUSH,
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

/* Per-MAC PTP TX / RX register windows, sitting inside each per-MAC
 * reg space. Each PTP TX slot is 256 B wide; the first 8 B hold the
 * cmd1/cmd2 header, leaving 248 B for frame data. HW provides 8 slots.
 * The PTP RX buffer mirrors the layout with a 252 B usable area and
 * an 8 B HW timestamp footer.
 */
#define TSN_PTP_TX_CONTROL_OFFSET	0x00012000
#define TSN_PTP_RX_CONTROL_OFFSET	0x00012004

#define TSN_PTP_RX_BASE_OFFSET		0x00010000
#define TSN_PTP_RX_PACKET_FIELD_MASK	GENMASK(11, 8)
#define TSN_PTP_RX_PACKET_CLEAR		BIT(0)

#define TSN_PTP_TX_BASE_OFFSET		0x00011000
#define TSN_PTP_TX_HWBUF_SIZE		0x100
#define TSN_PTP_TX_BUFFERS		8
#define TSN_PTP_TX_BUFFER_OFFSET(i)	(TSN_PTP_TX_BASE_OFFSET + \
					 (i) * TSN_PTP_TX_HWBUF_SIZE)
#define TSN_PTP_TX_CMD_FIELD_LEN	8
#define TSN_PTP_TX_MAX_FRAME_SIZE	(TSN_PTP_TX_HWBUF_SIZE - \
					 TSN_PTP_TX_CMD_FIELD_LEN)
#define TSN_PTP_TX_BUFFER_CMD2_FIELD	0x4

#define TSN_PTP_TX_FRAME_WAITING_MASK	GENMASK(15, 8)
#define TSN_PTP_TX_BUFFERS_FULL_MASK	BIT(TSN_PTP_TX_BUFFERS - 1)
#define TSN_PTP_TX_PACKET_FIELD_MASK	GENMASK(18, 16)

#define TSN_PTP_HW_TSTAMP_SIZE		8
#define TSN_PTP_RX_HWBUF_SIZE		256
#define TSN_PTP_RX_FRAME_SIZE		252
#define TSN_PTP_HW_TSTAMP_OFFSET	(TSN_PTP_RX_HWBUF_SIZE - \
					 TSN_PTP_HW_TSTAMP_SIZE)

#define TSN_PTP_MSG_TYPE_MASK		BIT(3)

struct kernel_ethtool_ts_info;
struct mii_bus;
struct netlink_ext_ack;
struct xlnx_tsn;

/**
 * struct xlnx_tsn_mac - per-MAC switch-side state
 * @sw: back-pointer to the parent switch (for dev_* logging in
 *	bus callbacks)
 * @regs: per-MAC register window, from reg-name "macN"
 * @mii_bus: MDIO bus registered under the "mdio-macN" DT child,
 *	     or NULL if absent
 * @ptp_tx_irq: per-MAC PTP TX-completion interrupt
 * @ptp_rx_irq: per-MAC PTP RX interrupt
 * @ptp_tx_lock: serialises the PTP TX slot allocator and the TX
 *		 completion path
 * @ptp_txq: in-flight PTP TX frames awaiting timestamp completion.
 *	     Each frame's slot index is kept in skb->cb[0].
 * @tx_tstamp_work: work item queued by the PTP TX IRQ to drain
 *		    ptp_txq and deliver timestamps through skb_tstamp_tx()
 * @ptp_rx_hw_pointer: HW write pointer snapshot read in the RX ISR
 * @ptp_rx_sw_pointer: SW read pointer; drained until it catches up
 * @hwtstamp_tx_type: current SO_TIMESTAMPING TX type for this port
 * @hwtstamp_rx_filter: current SO_TIMESTAMPING RX filter for this port
 */
struct xlnx_tsn_mac {
	struct xlnx_tsn *sw;
	void __iomem *regs;
	struct mii_bus *mii_bus;
	int ptp_tx_irq;
	int ptp_rx_irq;
	spinlock_t ptp_tx_lock; /* serialises PTP TX slot alloc + completion */
	struct sk_buff_head ptp_txq;
	struct work_struct tx_tstamp_work;
	u32 ptp_rx_hw_pointer;
	u32 ptp_rx_sw_pointer;
	int hwtstamp_tx_type;
	int hwtstamp_rx_filter;
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
 * @indirect_lock: serialises the CAM and VLAN-membership indirect
 *		   register sequences
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
 * @tagger_data: PTP TX callback descriptor handed to the tag protocol
 *		 via @dsa_switch.tagger_data
 */
struct xlnx_tsn {
	struct dsa_switch ds;
	struct device *dev;
	void __iomem *sw_base;
	struct net_device *conduit;
	u8 mac_prefix[ETH_ALEN];
	struct notifier_block nb;
	struct mutex indirect_lock; /* serialises CAM indirect access */
	struct xlnx_tsn_mac mac[XLNX_TSN_NUM_PORTS];
	int ptp_timer_irq;
	struct ptp_clock *ptp_clock;
	struct ptp_clock_info ptp_clock_info;
	spinlock_t reg_lock; /* serialises RTC offset/increment registers */
	u64 rtc_value;
	int pps_enable;
	int countpulse;
	struct xlnx_tsn_tagger_data tagger_data;
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
int xlnx_tsn_port_ptp_init(struct xlnx_tsn *sw, int port,
			   const char *rx_name, const char *tx_name);
void xlnx_tsn_port_ptp_exit(struct xlnx_tsn *sw, int port);
void xlnx_tsn_ptp_tx(struct dsa_port *dp, struct sk_buff *skb);
int xlnx_tsn_port_hwtstamp_get(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config);
int xlnx_tsn_port_hwtstamp_set(struct dsa_switch *ds, int port,
			       struct kernel_hwtstamp_config *config,
			       struct netlink_ext_ack *extack);
int xlnx_tsn_get_ts_info(struct dsa_switch *ds, int port,
			 struct kernel_ethtool_ts_info *info);

#endif /* _XILINX_TSN_H */
