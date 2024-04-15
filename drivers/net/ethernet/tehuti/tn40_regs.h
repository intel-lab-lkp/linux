/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) Tehuti Networks Ltd. */

#ifndef _TN40_REGS_H_
#define _TN40_REGS_H_

/* Register region size */
#define BDX_REGS_SIZE 0x10000

/* Registers from 0x0000-0x00fc were remapped to 0x4000-0x40fc */
#define REG_TXD_CFG1_0 0x4000
#define REG_TXD_CFG1_1 0x4004
#define REG_TXD_CFG1_2 0x4008
#define REG_TXD_CFG1_3 0x400C

#define REG_RXF_CFG1_0 0x4010
#define REG_RXF_CFG1_1 0x4014
#define REG_RXF_CFG1_2 0x4018
#define REG_RXF_CFG1_3 0x401C

#define REG_RXD_CFG1_0 0x4020
#define REG_RXD_CFG1_1 0x4024
#define REG_RXD_CFG1_2 0x4028
#define REG_RXD_CFG1_3 0x402C

#define REG_TXF_CFG1_0 0x4030
#define REG_TXF_CFG1_1 0x4034
#define REG_TXF_CFG1_2 0x4038
#define REG_TXF_CFG1_3 0x403C

#define REG_TXD_CFG0_0 0x4040
#define REG_TXD_CFG0_1 0x4044
#define REG_TXD_CFG0_2 0x4048
#define REG_TXD_CFG0_3 0x404C

#define REG_RXF_CFG0_0 0x4050
#define REG_RXF_CFG0_1 0x4054
#define REG_RXF_CFG0_2 0x4058
#define REG_RXF_CFG0_3 0x405C

#define REG_RXD_CFG0_0 0x4060
#define REG_RXD_CFG0_1 0x4064
#define REG_RXD_CFG0_2 0x4068
#define REG_RXD_CFG0_3 0x406C

#define REG_TXF_CFG0_0 0x4070
#define REG_TXF_CFG0_1 0x4074
#define REG_TXF_CFG0_2 0x4078
#define REG_TXF_CFG0_3 0x407C

#define REG_TXD_WPTR_0 0x4080
#define REG_TXD_WPTR_1 0x4084
#define REG_TXD_WPTR_2 0x4088
#define REG_TXD_WPTR_3 0x408C

#define REG_RXF_WPTR_0 0x4090
#define REG_RXF_WPTR_1 0x4094
#define REG_RXF_WPTR_2 0x4098
#define REG_RXF_WPTR_3 0x409C

#define REG_RXD_WPTR_0 0x40A0
#define REG_RXD_WPTR_1 0x40A4
#define REG_RXD_WPTR_2 0x40A8
#define REG_RXD_WPTR_3 0x40AC

#define REG_TXF_WPTR_0 0x40B0
#define REG_TXF_WPTR_1 0x40B4
#define REG_TXF_WPTR_2 0x40B8
#define REG_TXF_WPTR_3 0x40BC

#define REG_TXD_RPTR_0 0x40C0
#define REG_TXD_RPTR_1 0x40C4
#define REG_TXD_RPTR_2 0x40C8
#define REG_TXD_RPTR_3 0x40CC

#define REG_RXF_RPTR_0 0x40D0
#define REG_RXF_RPTR_1 0x40D4
#define REG_RXF_RPTR_2 0x40D8
#define REG_RXF_RPTR_3 0x40DC

#define REG_RXD_RPTR_0 0x40E0
#define REG_RXD_RPTR_1 0x40E4
#define REG_RXD_RPTR_2 0x40E8
#define REG_RXD_RPTR_3 0x40EC

#define REG_TXF_RPTR_0 0x40F0
#define REG_TXF_RPTR_1 0x40F4
#define REG_TXF_RPTR_2 0x40F8
#define REG_TXF_RPTR_3 0x40FC

/* Hardware versioning */
#define FW_VER 0x5010
#define SROM_VER 0x5020
#define FPGA_VER 0x5030
#define FPGA_SEED 0x5040

/* Registers from 0x0100-0x0150 were remapped to 0x5100-0x5150 */
#define REG_ISR REG_ISR0
#define REG_ISR0 0x5100

#define REG_IMR REG_IMR0
#define REG_IMR0 0x5110

#define REG_RDINTCM0 0x5120
#define REG_RDINTCM2 0x5128

#define REG_TDINTCM0 0x5130

#define REG_ISR_MSK0 0x5140

#define REG_INIT_SEMAPHORE 0x5170
#define REG_INIT_STATUS 0x5180

#define REG_MAC_LNK_STAT 0x0200
#define MAC_LINK_STAT 0x0004 /* Link state */

#define REG_BLNK_LED 0x0210

#define REG_GMAC_RXF_A 0x1240

#define REG_UNC_MAC0_A 0x1250
#define REG_UNC_MAC1_A 0x1260
#define REG_UNC_MAC2_A 0x1270

#define REG_VLAN_0 0x1800

#define REG_MAX_FRAME_A 0x12C0

#define REG_RX_MAC_MCST0 0x1A80
#define REG_RX_MAC_MCST1 0x1A84
#define MAC_MCST_NUM 15
#define REG_RX_MCST_HASH0 0x1A00
#define MAC_MCST_HASH_NUM 8

#define REG_VPC 0x2300
#define REG_VIC 0x2320
#define REG_VGLB 0x2340

#define REG_CLKPLL 0x5000

/* MDIO interface */

#define REG_MDIO_CMD_STAT 0x6030
#define REG_MDIO_CMD 0x6034
#define REG_MDIO_DATA 0x6038
#define REG_MDIO_ADDR 0x603C
#define GET_MDIO_BUSY(x) GET_BITS_SHIFT(x, 1, 0)
#define GET_MDIO_RD_ERR(x) GET_BITS_SHIFT(x, 1, 1)

/* for 10G only*/
#define REG_RSS_CNG 0x000000b0

#define RSS_ENABLED 0x00000001
#define RSS_HFT_TOEPLITZ 0x00000002
#define RSS_HASH_IPV4 0x00000100
#define RSS_HASH_TCP_IPV4 0x00000200
#define RSS_HASH_IPV6 0x00000400
#define RSS_HASH_IPV6_EX 0x00000800
#define RSS_HASH_TCP_IPV6 0x00001000
#define RSS_HASH_TCP_IPV6_EX 0x00002000

#define REG_RSS_HASH_BASE 0x0400
#define RSS_HASH_LEN 40
#define REG_RSS_INDT_BASE 0x0600
#define RSS_INDT_LEN 256

#define REG_REVISION 0x6000
#define REG_SCRATCH 0x6004
#define REG_CTRLST 0x6008
#define REG_MAC_ADDR_0 0x600C
#define REG_MAC_ADDR_1 0x6010
#define REG_FRM_LENGTH 0x6014
#define REG_PAUSE_QUANT 0x6054
#define REG_RX_FIFO_SECTION 0x601C
#define REG_TX_FIFO_SECTION 0x6020
#define REG_RX_FULLNESS 0x6024
#define REG_TX_FULLNESS 0x6028
#define REG_HASHTABLE 0x602C

#define REG_RST_PORT 0x7000
#define REG_DIS_PORT 0x7010
#define REG_RST_QU 0x7020
#define REG_DIS_QU 0x7030

#define REG_CTRLST_TX_ENA 0x0001
#define REG_CTRLST_RX_ENA 0x0002
#define REG_CTRLST_PRM_ENA 0x0010
#define REG_CTRLST_PAD_ENA 0x0020

#define REG_CTRLST_BASE (REG_CTRLST_PAD_ENA | REG_CTRLST_PRM_ENA)

#define REG_RX_FLT 0x1400

/* TXD TXF RXF RXD  CONFIG 0x0000 --- 0x007c */
#define TX_RX_CFG1_BASE 0xffffffff /*0-31 */
#define TX_RX_CFG0_BASE 0xfffff000 /*31:12 */
#define TX_RX_CFG0_RSVD 0x00000ffc /*11:2 */
#define TX_RX_CFG0_SIZE 0x00000003 /*1:0 */

/* TXD TXF RXF RXD  WRITE 0x0080 --- 0x00BC */
#define TXF_WPTR_WR_PTR 0x00007ff8 /*14:3 */

/* TXD TXF RXF RXD  READ  0x00CO --- 0x00FC */
#define TXF_RPTR_RD_PTR 0x00007ff8 /*14:3 */

/* The last 4 bits are dropped size is rounded to 16 */
#define TXF_WPTR_MASK 0x7ff0

/* regISR 0x0100 */
/* regIMR 0x0110 */
#define IMR_INPROG 0x80000000 /*31 */
#define IR_LNKCHG1 0x10000000 /*28 */
#define IR_LNKCHG0 0x08000000 /*27 */
#define IR_GPIO 0x04000000 /*26 */
#define IR_RFRSH 0x02000000 /*25 */
#define IR_RSVD 0x01000000 /*24 */
#define IR_SWI 0x00800000 /*23 */
#define IR_RX_FREE_3 0x00400000 /*22 */
#define IR_RX_FREE_2 0x00200000 /*21 */
#define IR_RX_FREE_1 0x00100000 /*20 */
#define IR_RX_FREE_0 0x00080000 /*19 */
#define IR_TX_FREE_3 0x00040000 /*18 */
#define IR_TX_FREE_2 0x00020000 /*17 */
#define IR_TX_FREE_1 0x00010000 /*16 */
#define IR_TX_FREE_0 0x00008000 /*15 */
#define IR_RX_DESC_3 0x00004000 /*14 */
#define IR_RX_DESC_2 0x00002000 /*13 */
#define IR_RX_DESC_1 0x00001000 /*12 */
#define IR_RX_DESC_0 0x00000800 /*11 */
#define IR_PSE 0x00000400 /*10 */
#define IR_TMR3 0x00000200 /* 9 */
#define IR_TMR2 0x00000100 /* 8 */
#define IR_TMR1 0x00000080 /* 7 */
#define IR_TMR0 0x00000040 /* 6 */
#define IR_VNT 0x00000020 /* 5 */
#define IR_RxFL 0x00000010 /* 4 */
#define IR_SDPERR 0x00000008 /* 3 */
#define IR_TR 0x00000004 /* 2 */
#define IR_PCIE_LINK 0x00000002 /* 1 */
#define IR_PCIE_TOUT 0x00000001 /* 0 */

#define IR_EXTRA                                                     \
	(IR_RX_FREE_0 | IR_LNKCHG0 | IR_LNKCHG1 | IR_PSE | IR_TMR0 | \
	 IR_PCIE_LINK | IR_PCIE_TOUT)

#define GMAC_RX_FILTER_OSEN 0x1000 /* shared OS enable */
#define GMAC_RX_FILTER_TXFC 0x0400 /* Tx flow control */
#define GMAC_RX_FILTER_RSV0 0x0200 /* reserved */
#define GMAC_RX_FILTER_FDA 0x0100 /* filter out direct address */
#define GMAC_RX_FILTER_AOF 0x0080 /* accept over run */
#define GMAC_RX_FILTER_ACF 0x0040 /* accept control frames */
#define GMAC_RX_FILTER_ARUNT 0x0020 /* accept under run */
#define GMAC_RX_FILTER_ACRC 0x0010 /* accept crc error */
#define GMAC_RX_FILTER_AM 0x0008 /* accept multicast */
#define GMAC_RX_FILTER_AB 0x0004 /* accept broadcast */
#define GMAC_RX_FILTER_PRM 0x0001 /* [0:1] promiscuous mode */

#define MAX_FRAME_AB_VAL 0x3fff /* 13:0 */

#define CLKPLL_PLLLKD 0x0200 /* 9 */
#define CLKPLL_RSTEND 0x0100 /* 8 */
#define CLKPLL_SFTRST 0x0001 /* 0 */

#define CLKPLL_LKD (CLKPLL_PLLLKD | CLKPLL_RSTEND)

/* PCI-E Device Control Register (Offset 0x88) Source: Luxor Data
 * Sheet, 7.1.3.3.3
 */
#define PCI_DEV_CTRL_REG 0x88
#define GET_DEV_CTRL_MAXPL(x) GET_BITS_SHIFT(x, 3, 5)
#define GET_DEV_CTRL_MRRS(x) GET_BITS_SHIFT(x, 3, 12)

/* PCI-E Link Status Register (Offset 0x92) Source: Luxor Data
 * Sheet, 7.1.3.3.7
 */
#define PCI_LINK_STATUS_REG 0x92
#define GET_LINK_STATUS_LANES(x) GET_BITS_SHIFT(x, 6, 4)

#endif
