/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 *  rtase is the Linux device driver released for Realtek Automotive Switch
 *  controllers with PCI-Express interface.
 *
 *  Copyright(c) 2023 Realtek Semiconductor Corp.
 */

#ifndef _RTASE_H_
#define _RTASE_H_

/* the low 32 bit address of receive buffer must be 8-byte alignment. */
#define RTK_RX_ALIGN 8

#define HW_VER_MASK 0x7C800000

#define RX_DMA_BURST_256       4
#define TX_DMA_BURST_UNLIMITED 7
#define RX_BUF_SIZE            (PAGE_SIZE - \
				SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))
#define MAX_JUMBO_SIZE         (RX_BUF_SIZE - VLAN_ETH_HLEN - ETH_FCS_LEN)

/* 3 means InterFrameGap = the shortest one */
#define INTERFRAMEGAP 0x03

#define RTASE_REGS_SIZE     256
#define RTASE_PCI_REGS_SIZE 0x100

#define MULTICAST_FILTER_MASK  GENMASK(30, 26)
#define MULTICAST_FILTER_LIMIT 32

#define RTASE_VLAN_FILTER_ENTRY_NUM 32
#define RTASE_NUM_TX_QUEUE 8
#define RTASE_NUM_RX_QUEUE 4

#define RTASE_TXQ_CTRL      1
#define RTASE_FUNC_TXQ_NUM  1
#define RTASE_FUNC_RXQ_NUM  1
#define RTASE_INTERRUPT_NUM 1

#define MITI_TIME_COUNT_MASK     GENMASK(3, 0)
#define MITI_TIME_UNIT_MASK      GENMASK(7, 4)
#define MITI_DEFAULT_TIME        128
#define MITI_MAX_TIME            491520
#define MITI_PKT_NUM_COUNT_MASK  GENMASK(11, 8)
#define MITI_PKT_NUM_UNIT_MASK   GENMASK(13, 12)
#define MITI_DEFAULT_PKT_NUM     64
#define MITI_MAX_PKT_NUM_IDX     3
#define MITI_MAX_PKT_NUM_UNIT    16
#define MITI_MAX_PKT_NUM         240
#define MITI_COUNT_BIT_NUM       4

#define RTASE_NUM_MSIX 4

#define RTASE_DWORD_MOD 16

/*****************************************************************************/
enum rtase_registers {
	RTASE_MAC0   = 0x0000,
	RTASE_MAC4   = 0x0004,
	RTASE_MAR0   = 0x0008,
	RTASE_MAR1   = 0x000C,
	RTASE_DTCCR0 = 0x0010,
	RTASE_DTCCR4 = 0x0014,
#define COUNTER_RESET BIT(0)
#define COUNTER_DUMP  BIT(3)

	RTASE_FCR    = 0x0018,
#define FCR_RXQ_MASK    GENMASK(5, 4)
#define FCR_VLAN_FTR_EN BIT(1)

	RTASE_LBK_CTRL = 0x001A,
#define LBK_ATLD BIT(1)
#define LBK_CLR  BIT(0)

	RTASE_TX_DESC_ADDR0   = 0x0020,
	RTASE_TX_DESC_ADDR4   = 0x0024,
	RTASE_TX_DESC_COMMAND = 0x0028,
#define TX_DESC_CMD_CS BIT(15)
#define TX_DESC_CMD_WE BIT(14)

	RTASE_BOOT_CTL  = 0x6004,
	RTASE_CLKSW_SET = 0x6018,

	RTASE_CHIP_CMD = 0x0037,
#define STOP_REQ      BIT(7)
#define STOP_REQ_DONE BIT(6)
#define RE            BIT(3)
#define TE            BIT(2)

	RTASE_IMR0 = 0x0038,
	RTASE_ISR0 = 0x003C,
#define TOK7 BIT(30)
#define TOK6 BIT(28)
#define TOK5 BIT(26)
#define TOK4 BIT(24)
#define FOVW BIT(6)
#define RDU  BIT(4)
#define TOK  BIT(2)
#define ROK  BIT(0)

	RTASE_IMR1 = 0x0800,
	RTASE_ISR1 = 0x0802,
#define Q_TOK BIT(4)
#define Q_RDU BIT(1)
#define Q_ROK BIT(0)

	RTASE_EPHY_ISR = 0x6014,
	RTASE_EPHY_IMR = 0x6016,

	RTASE_TX_CONFIG_0 = 0x0040,
#define TX_INTER_FRAME_GAP_MASK GENMASK(25, 24)
	/* DMA burst value (0-7) is shift this many bits */
#define TX_DMA_MASK             GENMASK(10, 8)

	RTASE_RX_CONFIG_0 = 0x0044,
#define RX_SINGLE_FETCH  BIT(14)
#define RX_SINGLE_TAG    BIT(13)
#define RX_MX_DMA_MASK   GENMASK(10, 8)
#define ACPT_FLOW        BIT(7)
#define ACCEPT_ERR       BIT(5)
#define ACCEPT_RUNT      BIT(4)
#define ACCEPT_BROADCAST BIT(3)
#define ACCEPT_MULTICAST BIT(2)
#define ACCEPT_MYPHYS    BIT(1)
#define ACCEPT_ALLPHYS   BIT(0)
#define ACCEPT_MASK      (ACPT_FLOW | ACCEPT_ERR | ACCEPT_RUNT | \
			  ACCEPT_BROADCAST | ACCEPT_MULTICAST | \
			  ACCEPT_MYPHYS | ACCEPT_ALLPHYS)

	RTASE_RX_CONFIG_1 = 0x0046,
#define RX_MAX_FETCH_DESC_MASK  GENMASK(15, 11)
#define RX_NEW_DESC_FORMAT_EN   BIT(8)
#define OUTER_VLAN_DETAG_EN     BIT(7)
#define INNER_VLAN_DETAG_EN     BIT(6)
#define PCIE_NEW_FLOW           BIT(2)
#define PCIE_RELOAD_En          BIT(0)

	RTASE_EEM = 0x0050,
#define EEM_UNLOCK 0xC0

	RTASE_TDFNR  = 0x0057,
	RTASE_TPPOLL = 0x0090,
	RTASE_PDR    = 0x00B0,
	RTASE_FIFOR  = 0x00D3,
#define TX_FIFO_EMPTY BIT(5)
#define RX_FIFO_EMPTY BIT(4)

	RTASE_PCPR = 0x00D8,
#define PCPR_VLAN_FTR_EN BIT(6)

	RTASE_RMS       = 0x00DA,
	RTASE_CPLUS_CMD = 0x00E0,
#define FORCE_RXFLOW_EN BIT(11)
#define FORCE_TXFLOW_EN BIT(10)
#define RX_CHKSUM       BIT(5)

	RTASE_Q0_RX_DESC_ADDR0 = 0x00E4,
	RTASE_Q0_RX_DESC_ADDR4 = 0x00E8,
	RTASE_Q1_RX_DESC_ADDR0 = 0x4000,
	RTASE_Q1_RX_DESC_ADDR4 = 0x4004,
	RTASE_MTPS             = 0x00EC,
#define TAG_NUM_SEL_MASK  GENMASK(10, 8)

	RTASE_MISC = 0x00F2,
#define RX_DV_GATE_EN BIT(3)

	RTASE_TFUN_CTRL = 0x0400,
#define TX_NEW_DESC_FORMAT_EN BIT(0)

	RTASE_TX_CONFIG_1 = 0x203E,
#define TC_MODE_MASK  GENMASK(11, 10)

	RTASE_TOKSEL      = 0x2046,
	RTASE_RFIFONFULL  = 0x4406,
	RTASE_INT_MITI_TX = 0x0A00,
	RTASE_INT_MITI_RX = 0x0A80,

	RTASE_VLAN_ENTRY_MEM_0 = 0x7234,
	RTASE_VLAN_ENTRY_0     = 0xAC80,
};

enum desc_status_bit {
	DESC_OWN = BIT(31), /* Descriptor is owned by NIC */
	RING_END = BIT(30), /* End of descriptor ring */
};

enum sw_flag_content {
	SWF_MSI_ENABLED  = BIT(1),
	SWF_MSIX_ENABLED = BIT(2),
};

#define RSVD_MASK 0x3FFFC000

struct tx_desc {
	__le32 opts1;
	__le32 opts2;
	__le64 addr;
	__le32 opts3;
	__le32 reserved1;
	__le32 reserved2;
	__le32 reserved3;
} __packed;

/*------ offset 0 of tx descriptor ------*/
#define TX_FIRST_FRAG BIT(29) /* Tx First segment of a packet */
#define TX_LAST_FRAG  BIT(28) /* Tx Final segment of a packet */
#define GIANT_SEND_V4 BIT(26) /* TCP Giant Send Offload V4 (GSOv4) */
#define GIANT_SEND_V6 BIT(25) /* TCP Giant Send Offload V6 (GSOv6) */
#define TX_VLAN_TAG   BIT(17) /* Add VLAN tag */

/*------ offset 4 of tx descriptor ------*/
#define TX_UDPCS_C BIT(31) /* Calculate UDP/IP checksum */
#define TX_TCPCS_C BIT(30) /* Calculate TCP/IP checksum */
#define TX_IPCS_C  BIT(29) /* Calculate IP checksum */
#define TX_IPV6F_C BIT(28) /* Indicate it is an IPv6 packet */

union rx_desc {
	struct {
		__le64 header_buf_addr;
		__le32 reserved1;
		__le32 opts_header_len;
		__le64 addr;
		__le32 reserved2;
		__le32 opts1;
	} __packed desc_cmd;

	struct {
		__le32 reserved1;
		__le32 reserved2;
		__le32 rss;
		__le32 opts4;
		__le32 reserved3;
		__le32 opts3;
		__le32 opts2;
		__le32 opts1;
	} __packed desc_status;
} __packed;

/*------ offset 28 of rx descriptor ------*/
#define RX_FIRST_FRAG    BIT(25) /* Rx First segment of a packet */
#define RX_LAST_FRAG     BIT(24) /* Rx Final segment of a packet */
#define RX_RES           BIT(20)
#define RX_RUNT          BIT(19)
#define RX_RWT           BIT(18)
#define RX_CRC           BIT(16)
#define RX_V6F           BIT(31)
#define RX_V4F           BIT(30)
#define RX_UDPT          BIT(29)
#define RX_TCPT          BIT(28)
#define RX_IPF           BIT(26) /* IP checksum failed */
#define RX_UDPF          BIT(25) /* UDP/IP checksum failed */
#define RX_TCPF          BIT(24) /* TCP/IP checksum failed */
#define RX_LBK_FIFO_FULL BIT(17) /* Loopback FIFO Full */
#define RX_VLAN_TAG      BIT(16) /* VLAN tag available */

#define NUM_DESC                1024
#define RTASE_TX_RING_DESC_SIZE (NUM_DESC * sizeof(struct tx_desc))
#define RTASE_RX_RING_DESC_SIZE (NUM_DESC * sizeof(union rx_desc))
#define VLAN_ENTRY_CAREBIT      0xF0000000
#define VLAN_TAG_MASK           GENMASK(15, 0)
#define RX_PKT_SIZE_MASK        GENMASK(13, 0)

#define IVEC_NAME_SIZE (IFNAMSIZ + 10)

struct rtase_int_vector {
	struct rtase_private *tp;
	unsigned int irq;
	u8 status;
	char name[IVEC_NAME_SIZE];
	u16 index;
	u16 imr_addr;
	u16 isr_addr;
	u32 imr;
	struct list_head ring_list;
	struct napi_struct napi;
	int (*poll)(struct napi_struct *napi, int budget);
};

struct rtase_ring {
	struct rtase_int_vector *ivec;
	void *desc;
	dma_addr_t phy_addr;
	u32 cur_idx;
	u32 dirty_idx;
	u16 index;

	struct sk_buff *skbuff[NUM_DESC];
	union {
		u32 len[NUM_DESC];
		dma_addr_t data_phy_addr[NUM_DESC];
	} mis;

	struct list_head ring_entry;
	int (*ring_handler)(struct rtase_ring *ring, int budget);
};

struct rtase_private {
	void __iomem *mmio_addr;
	u32 sw_flag;

	struct pci_dev *pdev;
	struct net_device *dev;
	u32 rx_buf_sz;

	struct page_pool *page_pool;
	struct rtase_ring tx_ring[RTASE_NUM_TX_QUEUE];
	struct rtase_ring rx_ring[RTASE_NUM_RX_QUEUE];
	struct rtase_counters *tally_vaddr;
	dma_addr_t tally_paddr;

	u32 vlan_filter_ctrl;
	u16 vlan_filter_vid[RTASE_VLAN_FILTER_ENTRY_NUM];

	struct msix_entry msix_entry[RTASE_NUM_MSIX];
	struct rtase_int_vector int_vector[RTASE_NUM_MSIX];

	u16 tx_queue_ctrl;
	u16 func_tx_queue_num;
	u16 func_rx_queue_num;
	u16 int_nums;
	u16 tx_int_mit;
	u16 rx_int_mit;
};

#define LSO_64K 64000

#define NIC_MAX_PHYS_BUF_COUNT_LSO2 (16 * 4)

#define TCPHO_MASK GENMASK(24, 18)

#define MSS_MAX  0x07FF /* MSS value */
#define MSS_MASK GENMASK(28, 18)

#endif /* _RTASE_H_ */
