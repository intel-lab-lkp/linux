// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <net/tcp.h>

#include "fbnic.h"
#include "fbnic_mac.h"
#include "fbnic_netdev.h"

static void fbnic_init_readrq(struct fbnic_dev *fbd, unsigned int offset,
			      unsigned int cls, unsigned int readrq)
{
	u32 val = rd32(offset);

	/* The TDF_CTL masks are a superset of the RNI_RBP ones. So we can
	 * use them when setting either the TDE_CTF or RNI_RBP registers.
	 */
	val &= FBNIC_QM_TNI_TDF_CTL_MAX_OT | FBNIC_QM_TNI_TDF_CTL_MAX_OB;

	val |= FIELD_PREP(FBNIC_QM_TNI_TDF_CTL_MRRS, readrq) |
	       FIELD_PREP(FBNIC_QM_TNI_TDF_CTL_CLS, cls);

	wr32(offset, val);
}

static void fbnic_init_mps(struct fbnic_dev *fbd, unsigned int offset,
			   unsigned int cls, unsigned int mps)
{
	u32 val = rd32(offset);

	/* Currently all MPS masks are identical so just use the first one */
	val &= ~(FBNIC_QM_TNI_TCM_CTL_MPS | FBNIC_QM_TNI_TCM_CTL_CLS);

	val |= FIELD_PREP(FBNIC_QM_TNI_TCM_CTL_MPS, mps) |
	       FIELD_PREP(FBNIC_QM_TNI_TCM_CTL_CLS, cls);

	wr32(offset, val);
}

static void fbnic_mac_init_axi(struct fbnic_dev *fbd)
{
	bool override_1k = false;
	int readrq, mps, cls;

	/* All of the values are based on being a power of 2 starting
	 * with 64 == 0. Therefore we can either divide by 64 in the
	 * case of constants, or just subtract 6 from the log2 of the value
	 * in order to get the value we will be programming into the
	 * registers.
	 */
	readrq = ilog2(fbd->readrq) - 6;
	if (readrq > 3)
		override_1k = true;
	readrq = clamp(readrq, 0, 3);

	mps = ilog2(fbd->mps) - 6;
	mps = clamp(mps, 0, 3);

	cls = ilog2(L1_CACHE_BYTES) - 6;
	cls = clamp(cls, 0, 3);

	/* Configure Tx/Rx AXI Paths w/ Read Request and Max Payload sizes */
	fbnic_init_readrq(fbd, FBNIC_QM_TNI_TDF_CTL, cls, readrq);
	fbnic_init_mps(fbd, FBNIC_QM_TNI_TCM_CTL, cls, mps);

	/* Configure QM TNI TDE:
	 * - Max outstanding AXI beats to 704(768 - 64) - guaranetees 8% of
	 *   buffer capacity to descriptors.
	 * - Max outstanding transactions to 128
	 */
	wr32(FBNIC_QM_TNI_TDE_CTL,
	     FIELD_PREP(FBNIC_QM_TNI_TDE_CTL_MRRS_1K, override_1k ? 1 : 0) |
	     FIELD_PREP(FBNIC_QM_TNI_TDE_CTL_MAX_OB, 704) |
	     FIELD_PREP(FBNIC_QM_TNI_TDE_CTL_MAX_OT, 128) |
	     FIELD_PREP(FBNIC_QM_TNI_TDE_CTL_MRRS, readrq) |
	     FIELD_PREP(FBNIC_QM_TNI_TDE_CTL_CLS, cls));

	fbnic_init_readrq(fbd, FBNIC_QM_RNI_RBP_CTL, cls, readrq);
	fbnic_init_mps(fbd, FBNIC_QM_RNI_RDE_CTL, cls, mps);
	fbnic_init_mps(fbd, FBNIC_QM_RNI_RCM_CTL, cls, mps);

	/* Enable XALI AR/AW outbound */
	wr32(FBNIC_PUL_OB_TLP_HDR_AW_CFG,
	     FBNIC_PUL_OB_TLP_HDR_AW_CFG_BME);
	wr32(FBNIC_PUL_OB_TLP_HDR_AR_CFG,
	     FBNIC_PUL_OB_TLP_HDR_AR_CFG_BME);
}

static void fbnic_mac_init_qm(struct fbnic_dev *fbd)
{
	u32 clock_freq;

	/* Configure TSO behavior */
	fbnic_wr32(fbd, FBNIC_QM_TQS_CTL0,
		   FIELD_PREP(FBNIC_QM_TQS_CTL0_LSO_TS_MASK,
			      FBNIC_QM_TQS_CTL0_LSO_TS_LAST) |
		   FIELD_PREP(FBNIC_QM_TQS_CTL0_PREFETCH_THRESH,
			      FBNIC_QM_TQS_CTL0_PREFETCH_THRESH_MIN));

	/* Limit EDT to INT_MAX as this is the limit of the EDT Qdisc */
	fbnic_wr32(fbd, FBNIC_QM_TQS_EDT_TS_RANGE, INT_MAX);

	/* Configure MTU
	 * Due to known HW issue we cannot set the MTU to within 16 octets
	 * of a 64 octet aligned boundary. So we will set the TQS_MTU(s) to
	 * MTU + 1.
	 */
	fbnic_wr32(fbd, FBNIC_QM_TQS_MTU_CTL0, FBNIC_MAX_JUMBO_FRAME_SIZE + 1);
	fbnic_wr32(fbd, FBNIC_QM_TQS_MTU_CTL1,
		   FIELD_PREP(FBNIC_QM_TQS_MTU_CTL1_BULK,
			      FBNIC_MAX_JUMBO_FRAME_SIZE + 1));

	clock_freq = FBNIC_CLOCK_FREQ;

	/* Be aggressive on the timings. We will have the interrupt
	 * threshold timer tick once every 1 usec and coalese writes for
	 * up to 80 usecs.
	 */
	fbnic_wr32(fbd, FBNIC_QM_TCQ_CTL0,
		   FIELD_PREP(FBNIC_QM_TCQ_CTL0_TICK_CYCLES,
			      clock_freq / 1000000) |
		   FIELD_PREP(FBNIC_QM_TCQ_CTL0_COAL_WAIT,
			      clock_freq / 12500));

	/* We will have the interrupt threshold timer tick once every
	 * 1 usec and coalese writes for up to 2 usecs.
	 */
	fbnic_wr32(fbd, FBNIC_QM_RCQ_CTL0,
		   FIELD_PREP(FBNIC_QM_RCQ_CTL0_TICK_CYCLES,
			      clock_freq / 1000000) |
		   FIELD_PREP(FBNIC_QM_RCQ_CTL0_COAL_WAIT,
			      clock_freq / 500000));

	/* Configure spacer control to 64 beats. */
	fbnic_wr32(fbd, FBNIC_FAB_AXI4_AR_SPACER_2_CFG,
		   FBNIC_FAB_AXI4_AR_SPACER_MASK |
		   FIELD_PREP(FBNIC_FAB_AXI4_AR_SPACER_THREADSHOLD, 2));
}

#define FBNIC_DROP_EN_MASK	0x7d
#define FBNIC_PAUSE_EN_MASK	0x14
#define FBNIC_ECN_EN_MASK	0x10

struct fbnic_fifo_config {
	unsigned int addr;
	unsigned int size;
};

/* Rx FIFO Configuration
 * The table consists of 8 entries, of which only 4 are currently used
 * The starting addr is in units of 64B and the size is in 2KB units
 * Below is the human readable version of the table defined below:
 * Function		Addr	Size
 * ----------------------------------
 * network to Host/BMC	384K	64K
 * Unused
 * Unused
 * network to BMC	448K	32K
 * network to Host	0	384K
 * Unused
 * BMC to Host		480K	32K
 * Unused
 */
static const struct fbnic_fifo_config fifo_config[] = {
	{ .addr = 0x1800, .size = 0x20 },	/* network to Host/BMC */
	{ },					/* not used */
	{ },					/* not used */
	{ .addr = 0x1c00, .size = 0x10 },	/* network to BMC */
	{ .addr = 0x0000, .size = 0xc0 },	/* network to Host */
	{ },					/* not used */
	{ .addr = 0x1e00, .size = 0x10 },	/* BMC to Host */
	{ }					/* not used */
};

static void fbnic_mac_init_rxb(struct fbnic_dev *fbd)
{
	bool rx_enable;
	int i;

	rx_enable = !!(fbnic_rd32(fbd, FBNIC_RPC_RMI_CONFIG) &
		       FBNIC_RPC_RMI_CONFIG_ENABLE);

	for (i = 0; i < 8; i++) {
		unsigned int size = fifo_config[i].size;

		/* If we are coming up on a system that already has the
		 * Rx data path enabled we don't need to reconfigure the
		 * FIFOs. Instead we can check to verify the values are
		 * large enough to meet our needs, and use the values to
		 * populate the flow control, ECN, and drop thresholds.
		 */
		if (rx_enable) {
			size = FIELD_GET(FBNIC_RXB_PBUF_SIZE,
					 fbnic_rd32(fbd,
						    FBNIC_RXB_PBUF_CFG(i)));
			if (size < fifo_config[i].size)
				dev_warn(fbd->dev,
					 "fifo%d size of %d smaller than expected value of %d\n",
					 i, size << 11,
					 fifo_config[i].size << 11);
		} else {
			/* Program RXB Cuthrough */
			fbnic_wr32(fbd, FBNIC_RXB_CT_SIZE(i),
				   FIELD_PREP(FBNIC_RXB_CT_SIZE_HEADER, 4) |
				   FIELD_PREP(FBNIC_RXB_CT_SIZE_PAYLOAD, 2));

			/* The granularity for the packet buffer size is 2KB
			 * granularity while the packet buffer base address is
			 * only 64B granularity
			 */
			fbnic_wr32(fbd, FBNIC_RXB_PBUF_CFG(i),
				   FIELD_PREP(FBNIC_RXB_PBUF_BASE_ADDR,
					      fifo_config[i].addr) |
				   FIELD_PREP(FBNIC_RXB_PBUF_SIZE, size));

			/* The granularity for the credits is 64B. This is
			 * based on RXB_PBUF_SIZE * 32 + 4.
			 */
			fbnic_wr32(fbd, FBNIC_RXB_PBUF_CREDIT(i),
				   FIELD_PREP(FBNIC_RXB_PBUF_CREDIT_MASK,
					      size ? size * 32 + 4 : 0));
		}

		if (!size)
			continue;

		/* Pause is size of FIFO with 56KB skid to start/stop */
		fbnic_wr32(fbd, FBNIC_RXB_PAUSE_THLD(i),
			   !(FBNIC_PAUSE_EN_MASK & (1u << i)) ? 0x1fff :
			   FIELD_PREP(FBNIC_RXB_PAUSE_THLD_ON,
				      size * 32 - 0x380) |
			   FIELD_PREP(FBNIC_RXB_PAUSE_THLD_OFF, 0x380));

		/* Enable Drop when only one packet is left in the FIFO */
		fbnic_wr32(fbd, FBNIC_RXB_DROP_THLD(i),
			   !(FBNIC_DROP_EN_MASK & (1u << i)) ? 0x1fff :
			   FIELD_PREP(FBNIC_RXB_DROP_THLD_ON,
				      size * 32 -
				      FBNIC_MAX_JUMBO_FRAME_SIZE / 64) |
			   FIELD_PREP(FBNIC_RXB_DROP_THLD_OFF,
				      size * 32 -
				      FBNIC_MAX_JUMBO_FRAME_SIZE / 64));

		/* Enable ECN bit when 1/4 of RXB is filled with at least
		 * 1 room for one full jumbo frame before setting ECN
		 */
		fbnic_wr32(fbd, FBNIC_RXB_ECN_THLD(i),
			   !(FBNIC_ECN_EN_MASK & (1u << i)) ? 0x1fff :
			   FIELD_PREP(FBNIC_RXB_ECN_THLD_ON,
				      max_t(unsigned int,
					    size * 32 / 4,
					    FBNIC_MAX_JUMBO_FRAME_SIZE / 64)) |
			   FIELD_PREP(FBNIC_RXB_ECN_THLD_OFF,
				      max_t(unsigned int,
					    size * 32 / 4,
					    FBNIC_MAX_JUMBO_FRAME_SIZE / 64)));
	}

	/* For now only enable drop and ECN. We need to add driver/kernel
	 * interfaces for configuring pause.
	 */
	fbnic_wr32(fbd, FBNIC_RXB_PAUSE_DROP_CTRL,
		   FIELD_PREP(FBNIC_RXB_PAUSE_DROP_CTRL_DROP_ENABLE,
			      FBNIC_DROP_EN_MASK) |
		   FIELD_PREP(FBNIC_RXB_PAUSE_DROP_CTRL_ECN_ENABLE,
			      FBNIC_ECN_EN_MASK));

	/* Program INTF credits */
	fbnic_wr32(fbd, FBNIC_RXB_INTF_CREDIT,
		   FBNIC_RXB_INTF_CREDIT_MASK0 |
		   FBNIC_RXB_INTF_CREDIT_MASK1 |
		   FBNIC_RXB_INTF_CREDIT_MASK2 |
		   FIELD_PREP(FBNIC_RXB_INTF_CREDIT_MASK3, 8));

	/* Configure calendar slots.
	 * Rx: 0 - 62	RDE 1st, BMC 2nd
	 *     63	BMC 1st, RDE 2nd
	 */
	for (i = 0; i < 16; i++) {
		u32 calendar_val = (i == 15) ? 0x1e1b1b1b : 0x1b1b1b1b;

		fbnic_wr32(fbd, FBNIC_RXB_CLDR_PRIO_CFG(i), calendar_val);
	}

	/* Split the credits for the DRR up as follows:
	 * Quantum0: 8000	Network to Host
	 * Quantum1: 0		Not used
	 * Quantum2: 80		BMC to Host
	 * Quantum3: 0		Not used
	 * Quantum4: 8000	Multicast to Host and BMC
	 */
	fbnic_wr32(fbd, FBNIC_RXB_DWRR_RDE_WEIGHT0,
		   FIELD_PREP(FBNIC_RXB_DWRR_RDE_WEIGHT0_QUANTUM0, 0x40) |
		   FIELD_PREP(FBNIC_RXB_DWRR_RDE_WEIGHT0_QUANTUM2, 0x50));
	fbnic_wr32(fbd, FBNIC_RXB_DWRR_RDE_WEIGHT0_EXT,
		   FIELD_PREP(FBNIC_RXB_DWRR_RDE_WEIGHT0_QUANTUM0, 0x1f));
	fbnic_wr32(fbd, FBNIC_RXB_DWRR_RDE_WEIGHT1,
		   FIELD_PREP(FBNIC_RXB_DWRR_RDE_WEIGHT1_QUANTUM4, 0x40));
	fbnic_wr32(fbd, FBNIC_RXB_DWRR_RDE_WEIGHT1_EXT,
		   FIELD_PREP(FBNIC_RXB_DWRR_RDE_WEIGHT1_QUANTUM4, 0x1f));

	/* Program RXB FCS Endian register */
	fbnic_wr32(fbd, FBNIC_RXB_ENDIAN_FCS, 0x0aaaaaa0);
}

static void fbnic_mac_init_txb(struct fbnic_dev *fbd)
{
	int i;

	fbnic_wr32(fbd, FBNIC_TCE_TXB_CTRL, 0);

	/* Configure Tx QM Credits */
	fbnic_wr32(fbd, FBNIC_QM_TQS_CTL1,
		   FIELD_PREP(FBNIC_QM_TQS_CTL1_MC_MAX_CREDITS, 0x40) |
		   FIELD_PREP(FBNIC_QM_TQS_CTL1_BULK_MAX_CREDITS, 0x20));

	/* Initialize internal Tx queues */
	fbnic_wr32(fbd, FBNIC_TCE_TXB_TEI_Q0_CTRL, 0);
	fbnic_wr32(fbd, FBNIC_TCE_TXB_TEI_Q1_CTRL, 0);
	fbnic_wr32(fbd, FBNIC_TCE_TXB_MC_Q_CTRL,
		   FIELD_PREP(FBNIC_TCE_TXB_Q_CTRL_SIZE, 0x400) |
		   FIELD_PREP(FBNIC_TCE_TXB_Q_CTRL_START, 0x000));
	fbnic_wr32(fbd, FBNIC_TCE_TXB_RX_TEI_Q_CTRL, 0);
	fbnic_wr32(fbd, FBNIC_TCE_TXB_TX_BMC_Q_CTRL,
		   FIELD_PREP(FBNIC_TCE_TXB_Q_CTRL_SIZE, 0x200) |
		   FIELD_PREP(FBNIC_TCE_TXB_Q_CTRL_START, 0x400));
	fbnic_wr32(fbd, FBNIC_TCE_TXB_RX_BMC_Q_CTRL,
		   FIELD_PREP(FBNIC_TCE_TXB_Q_CTRL_SIZE, 0x200) |
		   FIELD_PREP(FBNIC_TCE_TXB_Q_CTRL_START, 0x600));

	fbnic_wr32(fbd, FBNIC_TCE_LSO_CTRL,
		   FBNIC_TCE_LSO_CTRL_IPID_MODE_INC |
		   FIELD_PREP(FBNIC_TCE_LSO_CTRL_TCPF_CLR_1ST, TCPHDR_PSH |
							       TCPHDR_FIN) |
		   FIELD_PREP(FBNIC_TCE_LSO_CTRL_TCPF_CLR_MID, TCPHDR_PSH |
							       TCPHDR_CWR |
							       TCPHDR_FIN) |
		   FIELD_PREP(FBNIC_TCE_LSO_CTRL_TCPF_CLR_END, TCPHDR_CWR));
	fbnic_wr32(fbd, FBNIC_TCE_CSO_CTRL, 0);

	fbnic_wr32(fbd, FBNIC_TCE_BMC_MAX_PKTSZ,
		   FIELD_PREP(FBNIC_TCE_BMC_MAX_PKTSZ_TX,
			      FBNIC_MAX_JUMBO_FRAME_SIZE) |
		   FIELD_PREP(FBNIC_TCE_BMC_MAX_PKTSZ_RX,
			      FBNIC_MAX_JUMBO_FRAME_SIZE));
	fbnic_wr32(fbd, FBNIC_TCE_MC_MAX_PKTSZ,
		   FIELD_PREP(FBNIC_TCE_MC_MAX_PKTSZ_TMI,
			      FBNIC_MAX_JUMBO_FRAME_SIZE));

	/* Enable Drops in Tx path, needed for FPGA only */
	fbnic_wr32(fbd, FBNIC_TCE_DROP_CTRL,
		   FBNIC_TCE_DROP_CTRL_TTI_CM_DROP_EN |
		   FBNIC_TCE_DROP_CTRL_TTI_FRM_DROP_EN |
		   FBNIC_TCE_DROP_CTRL_TTI_TBI_DROP_EN);

	/* Configure calendar slots.
	 * Tx: 0 - 62	TMI 1st, BMC 2nd
	 *     63	BMC 1st, TMI 2nd
	 */
	for (i = 0; i < 16; i++) {
		u32 calendar_val = (i == 15) ? 0x1e1b1b1b : 0x1b1b1b1b;

		fbnic_wr32(fbd, FBNIC_TCE_TXB_CLDR_SLOT_CFG(i), calendar_val);
	}

	/* Configure DWRR */
	fbnic_wr32(fbd, FBNIC_TCE_TXB_ENQ_WRR_CTRL,
		   FIELD_PREP(FBNIC_TCE_TXB_ENQ_WRR_CTRL_WEIGHT0, 0x64) |
		   FIELD_PREP(FBNIC_TCE_TXB_ENQ_WRR_CTRL_WEIGHT2, 0x04));
	fbnic_wr32(fbd, FBNIC_TCE_TXB_TEI_DWRR_CTRL, 0);
	fbnic_wr32(fbd, FBNIC_TCE_TXB_TEI_DWRR_CTRL_EXT, 0);
	fbnic_wr32(fbd, FBNIC_TCE_TXB_BMC_DWRR_CTRL,
		   FIELD_PREP(FBNIC_TCE_TXB_BMC_DWRR_CTRL_QUANTUM0, 0x50) |
		   FIELD_PREP(FBNIC_TCE_TXB_BMC_DWRR_CTRL_QUANTUM1, 0x82));
	fbnic_wr32(fbd, FBNIC_TCE_TXB_BMC_DWRR_CTRL_EXT, 0);
	fbnic_wr32(fbd, FBNIC_TCE_TXB_NTWRK_DWRR_CTRL,
		   FIELD_PREP(FBNIC_TCE_TXB_NTWRK_DWRR_CTRL_QUANTUM1, 0x50) |
		   FIELD_PREP(FBNIC_TCE_TXB_NTWRK_DWRR_CTRL_QUANTUM2, 0x20));
	fbnic_wr32(fbd, FBNIC_TCE_TXB_NTWRK_DWRR_CTRL_EXT,
		   FIELD_PREP(FBNIC_TCE_TXB_NTWRK_DWRR_CTRL_QUANTUM2, 0x03));

	/* Configure SOP protocol protection */
	fbnic_wr32(fbd, FBNIC_TCE_SOP_PROT_CTRL,
		   FIELD_PREP(FBNIC_TCE_SOP_PROT_CTRL_TBI, 0x78) |
		   FIELD_PREP(FBNIC_TCE_SOP_PROT_CTRL_TTI_FRM, 0x40) |
		   FIELD_PREP(FBNIC_TCE_SOP_PROT_CTRL_TTI_CM, 0x0c));

	/* Conservative configuration on MAC interface Start of Packet
	 * protection FIFO. This sets the minimum depth of the FIFO before
	 * we start sending packets to the MAC measured in 64B units and
	 * up to 160 entries deep.
	 *
	 * For the ASIC the clock is fast enough that we will likely fill
	 * the SOP FIFO before the MAC can drain it. So just use a minimum
	 * value of 8.
	 *
	 * For the FPGA we have a clock that is about 3/5 of the MAC clock.
	 * As such we will need to account for adding more runway before
	 * transmitting the frames.
	 * SOP = (9230 / 64) * 2/5 + 8
	 * SOP = 66
	 */
	fbnic_wr32(fbd, FBNIC_TMI_SOP_PROT_CTRL, 8);

	wrfl();
	fbnic_wr32(fbd, FBNIC_TCE_TXB_CTRL, FBNIC_TCE_TXB_CTRL_TCAM_ENABLE |
					    FBNIC_TCE_TXB_CTRL_LOAD);
}

static void fbnic_mac_init_regs(struct fbnic_dev *fbd)
{
	fbnic_mac_init_axi(fbd);
	fbnic_mac_init_qm(fbd);
	fbnic_mac_init_rxb(fbd);
	fbnic_mac_init_txb(fbd);
}

static int fbnic_mac_get_link_event_asic(struct fbnic_dev *fbd)
{
	u32 pcs_intr_mask = rd32(FBNIC_MAC_PCS_INTR_STS);

	if (pcs_intr_mask & FBNIC_MAC_PCS_INTR_LINK_DOWN)
		return -1;

	return (pcs_intr_mask & FBNIC_MAC_PCS_INTR_LINK_UP) ? 1 : 0;
}

static u32 __fbnic_mac_config_asic(struct fbnic_dev *fbd)
{
	/* Enable MAC Promiscuous mode and Tx padding */
	u32 command_config = FBNIC_MAC_COMMAND_CONFIG_TX_PAD_EN |
			     FBNIC_MAC_COMMAND_CONFIG_PROMISC_EN;
	struct fbnic_net *fbn = netdev_priv(fbd->netdev);
	u32 rxb_pause_ctrl;

	/* Set class 0 Quanta and refresh */
	wr32(FBNIC_MAC_CL01_PAUSE_QUANTA, 0xffff);
	wr32(FBNIC_MAC_CL01_QUANTA_THRESH, 0x7fff);

	/* Enable generation of pause frames if enabled */
	rxb_pause_ctrl = rd32(FBNIC_RXB_PAUSE_DROP_CTRL);
	rxb_pause_ctrl &= ~FBNIC_RXB_PAUSE_DROP_CTRL_PAUSE_ENABLE;
	if (!fbn->tx_pause)
		command_config |= FBNIC_MAC_COMMAND_CONFIG_TX_PAUSE_DIS;
	else
		rxb_pause_ctrl |=
			FIELD_PREP(FBNIC_RXB_PAUSE_DROP_CTRL_PAUSE_ENABLE,
				   FBNIC_PAUSE_EN_MASK);
	wr32(FBNIC_RXB_PAUSE_DROP_CTRL, rxb_pause_ctrl);

	if (!fbn->rx_pause)
		command_config |= FBNIC_MAC_COMMAND_CONFIG_RX_PAUSE_DIS;

	/* Disable fault handling if no FEC is requested */
	if ((fbn->fec & FBNIC_FEC_MODE_MASK) == FBNIC_FEC_OFF)
		command_config |= FBNIC_MAC_COMMAND_CONFIG_FLT_HDL_DIS;

	return command_config;
}

static bool fbnic_mac_get_pcs_link_status(struct fbnic_dev *fbd)
{
	struct fbnic_net *fbn = netdev_priv(fbd->netdev);
	u32 pcs_status, lane_mask = ~0;

	pcs_status = rd32(FBNIC_MAC_PCS_STS0);
	if (!(pcs_status & FBNIC_MAC_PCS_STS0_LINK))
		return false;

	/* Define the expected lane mask for the status bits we need to check */
	switch (fbn->link_mode & FBNIC_LINK_MODE_MASK) {
	case FBNIC_LINK_100R2:
		lane_mask = 0xf;
		break;
	case FBNIC_LINK_50R1:
		lane_mask = 3;
		break;
	case FBNIC_LINK_50R2:
		switch (fbn->fec & FBNIC_FEC_MODE_MASK) {
		case FBNIC_FEC_OFF:
			lane_mask = 0x63;
			break;
		case FBNIC_FEC_RS:
			lane_mask = 5;
			break;
		case FBNIC_FEC_BASER:
			lane_mask = 0xf;
			break;
		}
		break;
	case FBNIC_LINK_25R1:
		lane_mask = 1;
		break;
	}

	/* Use an XOR to remove the bits we expect to see set */
	switch (fbn->fec & FBNIC_FEC_MODE_MASK) {
	case FBNIC_FEC_OFF:
		lane_mask ^= FIELD_GET(FBNIC_MAC_PCS_STS0_BLOCK_LOCK,
				       pcs_status);
		break;
	case FBNIC_FEC_RS:
		lane_mask ^= FIELD_GET(FBNIC_MAC_PCS_STS0_AMPS_LOCK,
				       pcs_status);
		break;
	case FBNIC_FEC_BASER:
		lane_mask ^= FIELD_GET(FBNIC_MAC_PCS_STS1_FCFEC_LOCK,
				       rd32(FBNIC_MAC_PCS_STS1));
		break;
	}

	/* If all lanes cancelled then we have a lock on all lanes */
	return !lane_mask;
}

#define FBNIC_MAC_ENET_LED_DEFAULT				\
	(FIELD_PREP(FBNIC_MAC_ENET_LED_AMBER_MASK,		\
		    FBNIC_MAC_ENET_LED_AMBER_50G |		\
		    FBNIC_MAC_ENET_LED_AMBER_25G) |		\
	 FIELD_PREP(FBNIC_MAC_ENET_LED_BLUE_MASK,		\
		    FBNIC_MAC_ENET_LED_BLUE_100G |		\
		    FBNIC_MAC_ENET_LED_BLUE_50G))
#define FBNIC_MAC_ENET_LED_ACTIVITY_DEFAULT			\
	FIELD_PREP(FBNIC_MAC_ENET_LED_BLINK_RATE_MASK,		\
		   FBNIC_MAC_ENET_LED_BLINK_RATE_5HZ)
#define FBNIC_MAC_ENET_LED_ACTIVITY_ON				\
	FIELD_PREP(FBNIC_MAC_ENET_LED_OVERRIDE_EN,		\
		   FBNIC_MAC_ENET_LED_OVERRIDE_ACTIVITY)
#define FBNIC_MAC_ENET_LED_AMBER				\
	(FIELD_PREP(FBNIC_MAC_ENET_LED_OVERRIDE_EN,		\
		    FBNIC_MAC_ENET_LED_OVERRIDE_BLUE |		\
		    FBNIC_MAC_ENET_LED_OVERRIDE_AMBER) |	\
	 FIELD_PREP(FBNIC_MAC_ENET_LED_OVERRIDE_VAL,		\
		    FBNIC_MAC_ENET_LED_OVERRIDE_AMBER))
#define FBNIC_MAC_ENET_LED_BLUE					\
	(FIELD_PREP(FBNIC_MAC_ENET_LED_OVERRIDE_EN,		\
		    FBNIC_MAC_ENET_LED_OVERRIDE_BLUE |		\
		    FBNIC_MAC_ENET_LED_OVERRIDE_AMBER) |	\
	 FIELD_PREP(FBNIC_MAC_ENET_LED_OVERRIDE_VAL,		\
		    FBNIC_MAC_ENET_LED_OVERRIDE_BLUE))

static void fbnic_set_led_state_asic(struct fbnic_dev *fbd, int state)
{
	struct fbnic_net *fbn = netdev_priv(fbd->netdev);
	u32 led_csr = FBNIC_MAC_ENET_LED_DEFAULT;

	switch (state) {
	case FBNIC_LED_OFF:
		led_csr |= FBNIC_MAC_ENET_LED_AMBER |
			   FBNIC_MAC_ENET_LED_ACTIVITY_ON;
		break;
	case FBNIC_LED_ON:
		led_csr |= FBNIC_MAC_ENET_LED_BLUE |
			   FBNIC_MAC_ENET_LED_ACTIVITY_ON;
		break;
	case FBNIC_LED_RESTORE:
		led_csr |= FBNIC_MAC_ENET_LED_ACTIVITY_DEFAULT;

		/* Don't set LEDs on if link isn't up */
		if (fbd->link_state != FBNIC_LINK_UP)
			break;
		/* Don't set LEDs for supported autoneg modes */
		if ((fbn->link_mode & FBNIC_LINK_AUTO) &&
		    (fbn->link_mode & FBNIC_LINK_MODE_MASK) != FBNIC_LINK_50R2)
			break;

		/* Set LEDs based on link speed
		 * 100G	Blue,
		 * 50G	Blue & Amber
		 * 25G	Amber
		 */
		switch (fbn->link_mode & FBNIC_LINK_MODE_MASK) {
		case FBNIC_LINK_100R2:
			led_csr |= FBNIC_MAC_ENET_LED_BLUE;
			break;
		case FBNIC_LINK_50R1:
		case FBNIC_LINK_50R2:
			led_csr |= FBNIC_MAC_ENET_LED_BLUE;
			fallthrough;
		case FBNIC_LINK_25R1:
			led_csr |= FBNIC_MAC_ENET_LED_AMBER;
			break;
		}
		break;
	default:
		return;
	}

	wr32(FBNIC_MAC_ENET_LED, led_csr);
}

static bool fbnic_mac_get_link_asic(struct fbnic_dev *fbd)
{
	u32 cmd_cfg, mac_ctrl;
	int link_direction;
	bool link;

	/* If disabled do not update link_state nor change settings */
	if (fbd->link_state == FBNIC_LINK_DISABLED)
		return false;

	link_direction = fbnic_mac_get_link_event_asic(fbd);

	/* Clear interrupt state due to recent changes. */
	wr32(FBNIC_MAC_PCS_INTR_STS,
	     FBNIC_MAC_PCS_INTR_LINK_DOWN | FBNIC_MAC_PCS_INTR_LINK_UP);

	/* If link bounced down clear the PCS_STS bit related to link */
	if (link_direction < 0) {
		wr32(FBNIC_MAC_PCS_STS0, FBNIC_MAC_PCS_STS0_LINK |
					 FBNIC_MAC_PCS_STS0_BLOCK_LOCK |
					 FBNIC_MAC_PCS_STS0_AMPS_LOCK);
		wr32(FBNIC_MAC_PCS_STS1, FBNIC_MAC_PCS_STS1_FCFEC_LOCK);
	}

	link = fbnic_mac_get_pcs_link_status(fbd);
	cmd_cfg = __fbnic_mac_config_asic(fbd);
	mac_ctrl = rd32(FBNIC_MAC_CTRL);

	/* Depending on the event we will unmask the cause that will force a
	 * transition, and update the Tx to reflect our status to the remote
	 * link partner.
	 */
	if (link) {
		mac_ctrl &= ~(FBNIC_MAC_CTRL_RESET_FF_TX_CLK |
			      FBNIC_MAC_CTRL_RESET_TX_CLK |
			      FBNIC_MAC_CTRL_RESET_FF_RX_CLK |
			      FBNIC_MAC_CTRL_RESET_RX_CLK);
		cmd_cfg |= FBNIC_MAC_COMMAND_CONFIG_RX_ENA |
			   FBNIC_MAC_COMMAND_CONFIG_TX_ENA;
		fbd->link_state = FBNIC_LINK_UP;
	} else {
		mac_ctrl |= FBNIC_MAC_CTRL_RESET_FF_TX_CLK |
			    FBNIC_MAC_CTRL_RESET_TX_CLK |
			    FBNIC_MAC_CTRL_RESET_FF_RX_CLK |
			    FBNIC_MAC_CTRL_RESET_RX_CLK;
		fbd->link_state = FBNIC_LINK_DOWN;
	}

	wr32(FBNIC_MAC_CTRL, mac_ctrl);
	wr32(FBNIC_MAC_COMMAND_CONFIG, cmd_cfg);

	/* Toggle LED settings to enable LEDs manually if necessary */
	fbnic_set_led_state_asic(fbd, FBNIC_LED_RESTORE);

	if (link_direction)
		wr32(FBNIC_MAC_PCS_INTR_MASK,
		     link ?  ~FBNIC_MAC_PCS_INTR_LINK_DOWN :
			     ~FBNIC_MAC_PCS_INTR_LINK_UP);

	return link;
}

static void fbnic_mac_pre_config(struct fbnic_dev *fbd)
{
	u32 serdes_ctrl, mac_ctrl, xif_mode, enet_fec_ctrl = 0;
	struct fbnic_net *fbn = netdev_priv(fbd->netdev);

	/* set reset bits and enable appending of Tx CRC */
	mac_ctrl = FBNIC_MAC_CTRL_RESET_FF_TX_CLK |
		   FBNIC_MAC_CTRL_RESET_FF_RX_CLK |
		   FBNIC_MAC_CTRL_RESET_TX_CLK |
		   FBNIC_MAC_CTRL_RESET_RX_CLK |
		   FBNIC_MAC_CTRL_TX_CRC;
	serdes_ctrl = FBNIC_MAC_SERDES_CTRL_RESET_PCS_REF_CLK |
		      FBNIC_MAC_SERDES_CTRL_RESET_F91_REF_CLK |
		      FBNIC_MAC_SERDES_CTRL_RESET_SD_TX_CLK |
		      FBNIC_MAC_SERDES_CTRL_RESET_SD_RX_CLK;
	xif_mode = FBNIC_MAC_XIF_MODE_TX_MAC_RS_ERR;

	switch (fbn->link_mode & FBNIC_LINK_MODE_MASK) {
	case FBNIC_LINK_25R1:
		/* Enable XGMII to run w/ 10G pacer */
		xif_mode |= FBNIC_MAC_XIF_MODE_XGMII;
		serdes_ctrl |= FBNIC_MAC_SERDES_CTRL_PACER_10G_MASK;
		if (fbn->fec & FBNIC_FEC_RS)
			serdes_ctrl |= FBNIC_MAC_SERDES_CTRL_F91_1LANE_IN0;
		break;
	case FBNIC_LINK_50R2:
		if (!(fbn->fec & FBNIC_FEC_RS))
			serdes_ctrl |= FBNIC_MAC_SERDES_CTRL_RXLAUI_ENA_IN0;
		break;
	case FBNIC_LINK_100R2:
		mac_ctrl |= FBNIC_MAC_CTRL_CFG_MODE128;
		serdes_ctrl |= FBNIC_MAC_SERDES_CTRL_PCS100_ENA_IN0;
		enet_fec_ctrl |= FBNIC_MAC_ENET_FEC_CTRL_KP_MODE_ENA;
		fallthrough;
	case FBNIC_LINK_50R1:
		serdes_ctrl |= FBNIC_MAC_SERDES_CTRL_SD_8X;
		if (fbn->fec & FBNIC_FEC_AUTO)
			fbn->fec = FBNIC_FEC_AUTO | FBNIC_FEC_RS;
		break;
	}

	switch (fbn->fec & FBNIC_FEC_MODE_MASK) {
	case FBNIC_FEC_RS:
		enet_fec_ctrl |= FBNIC_MAC_ENET_FEC_CTRL_F91_ENA;
		break;
	case FBNIC_FEC_BASER:
		enet_fec_ctrl |= FBNIC_MAC_ENET_FEC_CTRL_FEC_ENA;
		break;
	case FBNIC_FEC_OFF:
		break;
	default:
		dev_err(fbd->dev, "Unsupported FEC mode detected");
	}

	/* Store updated config to MAC */
	wr32(FBNIC_MAC_CTRL, mac_ctrl);
	wr32(FBNIC_MAC_SERDES_CTRL, serdes_ctrl);
	wr32(FBNIC_MAC_XIF_MODE, xif_mode);
	wr32(FBNIC_MAC_ENET_FEC_CTRL, enet_fec_ctrl);

	/* flush writes to allow time for MAC to go into resets */
	wrfl();

	/* Set signal detect for all lanes */
	wr32(FBNIC_MAC_ENET_SIG_DETECT, FBNIC_MAC_ENET_SIG_DETECT_PCS_MASK);
}

static void fbnic_mac_pcs_config(struct fbnic_dev *fbd)
{
	u32 pcs_mode = 0, rsfec_ctrl = 0, vl_intvl = 0;
	struct fbnic_net *fbn = netdev_priv(fbd->netdev);
	int i;

	/* Set link mode specific lane and FEC values */
	switch (fbn->link_mode & FBNIC_LINK_MODE_MASK) {
	case FBNIC_LINK_25R1:
		if (fbn->fec & FBNIC_FEC_RS)
			vl_intvl = 20479;
		else
			pcs_mode |= FBNIC_PCS_MODE_DISABLE_MLD;
		pcs_mode |= FBNIC_PCS_MODE_HI_BER25 |
			    FBNIC_PCS_MODE_ENA_CLAUSE49;
		break;
	case FBNIC_LINK_50R1:
		rsfec_ctrl |= FBNIC_RSFEC_CONTROL_KP_ENABLE;
		fallthrough;
	case FBNIC_LINK_50R2:
		rsfec_ctrl |= FBNIC_RSFEC_CONTROL_TC_PAD_ALTER;
		vl_intvl = 20479;
		break;
	case FBNIC_LINK_100R2:
		rsfec_ctrl |= FBNIC_RSFEC_CONTROL_AM16_COPY_DIS |
			      FBNIC_RSFEC_CONTROL_KP_ENABLE;
		pcs_mode |= FBNIC_PCS_MODE_DISABLE_MLD;
		vl_intvl = 16383;
		break;
	}

	for (i = 0; i < 4; i++)
		wr32(FBNIC_RSFEC_CONTROL(i), rsfec_ctrl);

	wr32(FBNIC_PCS_MODE_VL_CHAN_0, pcs_mode);
	wr32(FBNIC_PCS_MODE_VL_CHAN_1, pcs_mode);

	wr32(FBNIC_PCS_VENDOR_VL_INTVL_0, vl_intvl);
	wr32(FBNIC_PCS_VENDOR_VL_INTVL_1, vl_intvl);

	/* Update IPG to account for vl_intvl */
	wr32(FBNIC_MAC_TX_IPG_LENGTH,
	     FIELD_PREP(FBNIC_MAC_TX_IPG_LENGTH_COMP, vl_intvl) | 0xc);

	/* Program lane markers indicating which lanes are in use
	 * and what speeds we are transmitting at.
	 */
	switch (fbn->link_mode & FBNIC_LINK_MODE_MASK) {
	case FBNIC_LINK_100R2:
		wr32(FBNIC_PCS_VL0_0_CHAN_0, 0x68c1);
		wr32(FBNIC_PCS_VL0_1_CHAN_0, 0x21);
		wr32(FBNIC_PCS_VL1_0_CHAN_0, 0x719d);
		wr32(FBNIC_PCS_VL1_1_CHAN_0, 0x8e);
		wr32(FBNIC_PCS_VL2_0_CHAN_0, 0x4b59);
		wr32(FBNIC_PCS_VL2_1_CHAN_0, 0xe8);
		wr32(FBNIC_PCS_VL3_0_CHAN_0, 0x954d);
		wr32(FBNIC_PCS_VL3_1_CHAN_0, 0x7b);
		wr32(FBNIC_PCS_VL0_0_CHAN_1, 0x68c1);
		wr32(FBNIC_PCS_VL0_1_CHAN_1, 0x21);
		wr32(FBNIC_PCS_VL1_0_CHAN_1, 0x719d);
		wr32(FBNIC_PCS_VL1_1_CHAN_1, 0x8e);
		wr32(FBNIC_PCS_VL2_0_CHAN_1, 0x4b59);
		wr32(FBNIC_PCS_VL2_1_CHAN_1, 0xe8);
		wr32(FBNIC_PCS_VL3_0_CHAN_1, 0x954d);
		wr32(FBNIC_PCS_VL3_1_CHAN_1, 0x7b);
		break;
	case FBNIC_LINK_50R2:
		wr32(FBNIC_PCS_VL0_0_CHAN_1, 0x7690);
		wr32(FBNIC_PCS_VL0_1_CHAN_1, 0x47);
		wr32(FBNIC_PCS_VL1_0_CHAN_1, 0xc4f0);
		wr32(FBNIC_PCS_VL1_1_CHAN_1, 0xe6);
		wr32(FBNIC_PCS_VL2_0_CHAN_1, 0x65c5);
		wr32(FBNIC_PCS_VL2_1_CHAN_1, 0x9b);
		wr32(FBNIC_PCS_VL3_0_CHAN_1, 0x79a2);
		wr32(FBNIC_PCS_VL3_1_CHAN_1, 0x3d);
		fallthrough;
	case FBNIC_LINK_50R1:
		wr32(FBNIC_PCS_VL0_0_CHAN_0, 0x7690);
		wr32(FBNIC_PCS_VL0_1_CHAN_0, 0x47);
		wr32(FBNIC_PCS_VL1_0_CHAN_0, 0xc4f0);
		wr32(FBNIC_PCS_VL1_1_CHAN_0, 0xe6);
		wr32(FBNIC_PCS_VL2_0_CHAN_0, 0x65c5);
		wr32(FBNIC_PCS_VL2_1_CHAN_0, 0x9b);
		wr32(FBNIC_PCS_VL3_0_CHAN_0, 0x79a2);
		wr32(FBNIC_PCS_VL3_1_CHAN_0, 0x3d);
		break;
	case FBNIC_LINK_25R1:
		wr32(FBNIC_PCS_VL0_0_CHAN_0, 0x68c1);
		wr32(FBNIC_PCS_VL0_1_CHAN_0, 0x21);
		wr32(FBNIC_PCS_VL1_0_CHAN_0, 0xc4f0);
		wr32(FBNIC_PCS_VL1_1_CHAN_0, 0xe6);
		wr32(FBNIC_PCS_VL2_0_CHAN_0, 0x65c5);
		wr32(FBNIC_PCS_VL2_1_CHAN_0, 0x9b);
		wr32(FBNIC_PCS_VL3_0_CHAN_0, 0x79a2);
		wr32(FBNIC_PCS_VL3_1_CHAN_0, 0x3d);
		break;
	}
}

static bool fbnic_mac_pcs_reset_complete(struct fbnic_dev *fbd)
{
	return !(rd32(FBNIC_PCS_CONTROL1_0) & FBNIC_PCS_CONTROL1_RESET) &&
	       !(rd32(FBNIC_PCS_CONTROL1_1) & FBNIC_PCS_CONTROL1_RESET);
}

static int fbnic_mac_post_config(struct fbnic_dev *fbd)
{
	struct fbnic_net *fbn = netdev_priv(fbd->netdev);
	u32 serdes_ctrl, reset_complete, lane_mask;
	int err;

	/* Clear resets for XPCS and F91 reference clocks */
	serdes_ctrl = rd32(FBNIC_MAC_SERDES_CTRL);
	serdes_ctrl &= ~FBNIC_MAC_SERDES_CTRL_RESET_PCS_REF_CLK;
	if (fbn->fec & FBNIC_FEC_RS)
		serdes_ctrl &= ~FBNIC_MAC_SERDES_CTRL_RESET_F91_REF_CLK;
	wr32(FBNIC_MAC_SERDES_CTRL, serdes_ctrl);

	/* Reset PCS and flush reset value */
	wr32(FBNIC_PCS_CONTROL1_0,
	     FBNIC_PCS_CONTROL1_RESET |
	     FBNIC_PCS_CONTROL1_SPEED_SELECT_ALWAYS |
	     FBNIC_PCS_CONTROL1_SPEED_ALWAYS);
	wr32(FBNIC_PCS_CONTROL1_1,
	     FBNIC_PCS_CONTROL1_RESET |
	     FBNIC_PCS_CONTROL1_SPEED_SELECT_ALWAYS |
	     FBNIC_PCS_CONTROL1_SPEED_ALWAYS);

	/* poll for completion of reset */
	err = readx_poll_timeout(fbnic_mac_pcs_reset_complete, fbd,
				 reset_complete, reset_complete,
				 1000, 150000);
	if (err)
		return err;

	/* Flush any stale link status info */
	wr32(FBNIC_MAC_PCS_STS0, FBNIC_MAC_PCS_STS0_LINK |
				 FBNIC_MAC_PCS_STS0_BLOCK_LOCK |
				 FBNIC_MAC_PCS_STS0_AMPS_LOCK);

	/* Report starting state as "Link Event" to force detection of link */
	fbd->link_state = FBNIC_LINK_EVENT;

	/* Force link down to allow for link detection */
	netif_carrier_off(fbn->netdev);

	/* create simple bitmask for 2 or 1 lane setups */
	lane_mask = (fbn->link_mode & FBNIC_LINK_MODE_R2) ? 3 : 1;

	/* release the brakes and allow Tx/Rx to come out of reset */
	serdes_ctrl &=
	     ~(FIELD_PREP(FBNIC_MAC_SERDES_CTRL_RESET_SD_TX_CLK, lane_mask) |
	       FIELD_PREP(FBNIC_MAC_SERDES_CTRL_RESET_SD_RX_CLK, lane_mask));
	wr32(FBNIC_MAC_SERDES_CTRL, serdes_ctrl);

	fbn->link_mode &= ~FBNIC_LINK_AUTO;

	/* Ask firmware to configure the PHY for the correct encoding mode */
	return fbnic_fw_xmit_comphy_set_msg(fbd,
					    fbn->link_mode &
					    FBNIC_LINK_MODE_MASK);
}

static void fbnic_mac_get_fw_settings(struct fbnic_dev *fbd)
{
	struct fbnic_net *fbn = netdev_priv(fbd->netdev);
	u8 fec = fbn->fec;
	u8 link_mode;

	/* Update FEC first to reflect FW current mode */
	if (fbn->fec & FBNIC_FEC_AUTO) {
		switch (fbd->fw_cap.link_fec) {
		case FBNIC_FW_LINK_FEC_NONE:
			fec = FBNIC_FEC_OFF;
			break;
		case FBNIC_FW_LINK_FEC_RS:
			fec = FBNIC_FEC_RS;
			break;
		case FBNIC_FW_LINK_FEC_BASER:
			fec = FBNIC_FEC_BASER;
			break;
		default:
			return;
		}
	}

	/* Do nothing if AUTO mode is not engaged */
	if (fbn->link_mode & FBNIC_LINK_AUTO) {
		switch (fbd->fw_cap.link_speed) {
		case FBNIC_FW_LINK_SPEED_25R1:
			link_mode = FBNIC_LINK_25R1;
			break;
		case FBNIC_FW_LINK_SPEED_50R2:
			link_mode = FBNIC_LINK_50R2;
			break;
		case FBNIC_FW_LINK_SPEED_50R1:
			link_mode = FBNIC_LINK_50R1;
			fec = FBNIC_FEC_RS;
			break;
		case FBNIC_FW_LINK_SPEED_100R2:
			link_mode = FBNIC_LINK_100R2;
			fec = FBNIC_FEC_RS;
			break;
		default:
			return;
		}

		fbn->link_mode = link_mode;
		fbn->fec = fec;
	}
}

static int fbnic_mac_enable_asic(struct fbnic_dev *fbd)
{
	/* Mask and clear the PCS interrupt, will be enabled by link handler */
	wr32(FBNIC_MAC_PCS_INTR_MASK, ~0);
	wr32(FBNIC_MAC_PCS_INTR_STS, ~0);

	/* Pull in settings from FW */
	fbnic_mac_get_fw_settings(fbd);

	/* Configure MAC registers */
	fbnic_mac_pre_config(fbd);

	/* Configure PCS block */
	fbnic_mac_pcs_config(fbd);

	/* Configure flow control and error correction */
	wr32(FBNIC_MAC_COMMAND_CONFIG, __fbnic_mac_config_asic(fbd));

	/* Configure maximum frame size */
	wr32(FBNIC_MAC_FRM_LENGTH, FBNIC_MAX_JUMBO_FRAME_SIZE);

	/* Configure LED defaults */
	fbnic_set_led_state_asic(fbd, FBNIC_LED_RESTORE);

	return fbnic_mac_post_config(fbd);
}

static void fbnic_mac_disable_asic(struct fbnic_dev *fbd)
{
	u32 mask = FBNIC_MAC_COMMAND_CONFIG_LOOPBACK_EN;
	u32 cmd_cfg = rd32(FBNIC_MAC_COMMAND_CONFIG);
	u32 mac_ctrl = rd32(FBNIC_MAC_CTRL);

	/* Clear link state to disable any further transitions */
	fbd->link_state = FBNIC_LINK_DISABLED;

	/* Clear Tx and Rx enable bits to disable MAC, ignore other values */
	if (!fbnic_bmc_present(fbd)) {
		mask |= FBNIC_MAC_COMMAND_CONFIG_RX_ENA |
			FBNIC_MAC_COMMAND_CONFIG_TX_ENA;
		mac_ctrl |= FBNIC_MAC_CTRL_RESET_FF_TX_CLK |
			    FBNIC_MAC_CTRL_RESET_TX_CLK |
			    FBNIC_MAC_CTRL_RESET_FF_RX_CLK |
			    FBNIC_MAC_CTRL_RESET_RX_CLK;

		/* Restore LED defaults */
		fbnic_set_led_state_asic(fbd, FBNIC_LED_RESTORE);
	}

	/* Check mask for enabled bits, if any set clear and write back */
	if (mask & cmd_cfg) {
		wr32(FBNIC_MAC_COMMAND_CONFIG, cmd_cfg & ~mask);
		wr32(FBNIC_MAC_CTRL, mac_ctrl);
	}

	/* Disable loopback, and flush write */
	wr32(FBNIC_PCS_CONTROL1_0,
	     FBNIC_PCS_CONTROL1_RESET |
	     FBNIC_PCS_CONTROL1_SPEED_SELECT_ALWAYS |
	     FBNIC_PCS_CONTROL1_SPEED_ALWAYS);
	wr32(FBNIC_PCS_CONTROL1_1,
	     FBNIC_PCS_CONTROL1_RESET |
	     FBNIC_PCS_CONTROL1_SPEED_SELECT_ALWAYS |
	     FBNIC_PCS_CONTROL1_SPEED_ALWAYS);
}

static const struct fbnic_mac fbnic_mac_asic = {
	.enable = fbnic_mac_enable_asic,
	.disable = fbnic_mac_disable_asic,
	.init_regs = fbnic_mac_init_regs,
	.get_link = fbnic_mac_get_link_asic,
	.get_link_event = fbnic_mac_get_link_event_asic,
};

/**
 * fbnic_mac_init - Assign a MAC type and initialize the fbnic device
 * @fbd: Device pointer to device to initialize
 *
 * Returns 0 on success, negative on failure
 *
 * Initialize the MAC function pointers and initializes the MAC of
 * the device.
 **/
int fbnic_mac_init(struct fbnic_dev *fbd)
{
	fbd->mac = &fbnic_mac_asic;

	fbd->mac->init_regs(fbd);

	return 0;
}
