/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _IGB_REGISTERS_H_
#define _IGB_REGISTERS_H_

#include <linux/bits.h>

/* Register Offsets (Intel 82576EB Datasheet) */
#define IGB_CTRL 0x00000 /* Device Control */
#define IGB_STATUS 0x00008 /* Device Status */
#define IGB_CTRL_EXT 0x00018 /* Extended Device Control */
#define IGB_MDIC 0x00020 /* MDI Control */
#define IGB_RCTL 0x00100 /* Receive Control */
#define IGB_TCTL 0x00400 /* Transmit Control */
#define IGB_SCTL 0x00420 /* SerDes Control */

/* Interrupt Registers */
#define IGB_IMC 0x0150C /* Interrupt Mask Clear */
#define IGB_IVAR0 0x01700 /* Interrupt Vector Allocation Register 0 */

/* Rx Ring 0 Registers */
#define IGB_RDBAL0 0x0C000 /* Rx Desc Base Address Low */
#define IGB_RDBAH0 0x0C004 /* Rx Desc Base Address High */
#define IGB_RDLEN0 0x0C008 /* Rx Desc Length */
#define IGB_SRRCTL0 0x0C00C /* Split and Replication Receive Control Q0 */
#define IGB_RDH0 0x0C010 /* Rx Desc Head */
#define IGB_RDT0 0x0C018 /* Rx Desc Tail */
#define IGB_RXDCTL0 0x0C028 /* Rx Desc Control */

/* SRRCTL fields per 82576 datasheet section 8.10.2 */
#define IGB_SRRCTL_DESCTYPE_ADV_ONEBUF (1u << 25) /* 001b: advanced one-buffer */

/* Tx Ring 0 Registers */
#define IGB_TDBAL0 0x0E000 /* Tx Desc Base Address Low */
#define IGB_TDBAH0 0x0E004 /* Tx Desc Base Address High */
#define IGB_TDLEN0 0x0E008 /* Tx Desc Length */
#define IGB_TDH0 0x0E010 /* Tx Desc Head */
#define IGB_TDT0 0x0E018 /* Tx Desc Tail */
#define IGB_TXDCTL0 0x0E028 /* Tx Desc Control */

/* Control Bit Definitions */
/* CTRL */
#define IGB_CTRL_FD		(1 << 0)  /* Full Duplex */
#define IGB_CTRL_SLU		(1 << 6)  /* Set Link Up */
#define IGB_CTRL_SPD_SEL	(3 << 8) /* Speed Select Mask */
#define IGB_CTRL_SPD_1000	(2 << 8) /* Force 1000 Mb/s */
#define IGB_CTRL_FRCSPD		(1 << 11) /* Force Speed */
#define IGB_CTRL_FRCDPX		(1 << 12) /* Force Duplex */
#define IGB_CTRL_RST		(1 << 26) /* Device Reset */
#define IGB_CTRL_EXT_LINK_MODE_MASK (3 << 22)

/* CTRL_EXT */
#define IGB_CTRL_EXT_DRV_LOAD (1 << 28) /* Driver Loaded */

/* RCTL */
#define IGB_RCTL_EN (1 << 1) /* Receiver Enable */
#define IGB_RCTL_UPE (1 << 3) /* Unicast Promiscuous Enabled */
#define IGB_RCTL_LPE (1 << 5) /* Long Packet Reception Enable */
#define IGB_RCTL_LBM_MAC (1 << 6) /* Loopback Mode - MAC (set as QEMU-only accommodation) */
#define IGB_RCTL_SECRC (1 << 26) /* Strip Ethernet CRC */

/* TCTL */
#define IGB_TCTL_EN (1 << 1) /* Transmit Enable */

/* SCTL */
#define IGB_SCTL_DISABLE_SERDES_LOOPBACK (1 << 6)

/* MDIC */
#define IGB_MDIC_OP_WRITE (1 << 26)
#define IGB_MDIC_OP_READ  (2 << 26)

#define IGB_EICR 0x01580 /* Extended Interrupt Cause Read */

#define IGB_RAH0 0x05404 /* Receive Address High 0 */
#define IGB_VMOLR0 0x05AD0 /* VM Offload Layout Register 0 */
#define IGB_GCR 0x05B00 /* PCIe Control */
#define IGB_GCR_CMPL_TMOUT_RESEND BIT(16) /* Re-send on completion timeout */

#define IGB_VMOLR_LPE 0x00010000 /* Long Packet Enable */
#define IGB_VMOLR_BAM 0x08000000 /* Broadcast Accept Mode */
#define IGB_RAH_POOL_1 0x00040000 /* Pool 1 assignment */

#define IGB_EICS 0x01520 /* Extended Interrupt Cause Set */
#define IGB_EIMS 0x01524 /* Extended Interrupt Mask Set */
#define IGB_EIMC 0x01528 /* Extended Interrupt Mask Clear */
#define IGB_EIAC 0x0152C /* Extended Interrupt Auto Clear */
#define IGB_EIAM 0x01530 /* Extended Interrupt Auto Mask Enable */
#define IGB_EICR_VEC0 BIT(0) /* MSI-X cause/vector 0 */
#define IGB_CTRL_GIO_MASTER_DISABLE (1 << 2) /* GIO Master Disable */
#define IGB_STATUS_GIO_MASTER_ENABLE (1 << 19) /* GIO Master Enable */
#define IGB_GPIE 0x01514 /* General Purpose Interrupt Enable */
/* GPIE fields per 82576 datasheet section 7.3.2.11, Table 7-47 */
#define IGB_GPIE_MULTIPLE_MSIX BIT(4)  /* Multi-vector MSI-X mode */
#define IGB_GPIE_EIAME         BIT(30) /* Apply EIAM on MSI-X assertion */
#define IGB_TXDCTL0_Q_EN (1 << 25) /* Transmit Queue Enable */
#define IGB_RXDCTL0_Q_EN (1 << 25) /* Receive Queue Enable */
#define IGB_MRQC 0x05818 /* Multiple Receive Queues Command */

#define IGB_MDIC_PHY_SHIFT 21 /* PHY Address Shift */
#define IGB_MDIC_REG_SHIFT 16 /* Register Address Shift */
#define IGB_MDIC_READY (1 << 28) /* MDI Data Ready */
#define IGB_MDIC_ERROR (1 << 29) /* MDI Error */

/* PHY register 0 (Control), per 82576 datasheet section 3.5.6.3.1 */
#define IGB_PHY_CTRL_REG_OFFSET		0
#define IGB_PHY_CTRL_AN_RESTART		0x0200 /* bit 9 */
#define IGB_PHY_CTRL_AN_ENABLE		0x1000 /* bit 12 */
#define IGB_PHY_CTRL_SPEED_1000		0x0040 /* bit 6 set, bit 13 clear */
#define IGB_PHY_CTRL_FULL_DUPLEX	0x0100 /* bit 8 */
#define IGB_PHY_CTRL_LOOPBACK		0x4000 /* bit 14 */

#define IGB_IVAR_VALID 0x80 /* Valid bit for IVAR register */

/*
 * Advanced TX Data Descriptor fields per 82576 datasheet section 7.2.2.3.
 * The cmd_type_len word holds: DTALEN[15:0], MAC[19:18], DTYP[23:20],
 * DCMD[31:24].  The olinfo_status word holds: STA[3:0], IDX[6:4],
 * POPTS[13:8], PAYLEN[31:14].
 */
#define IGB_ADVTXD_DTYP_DATA	(0x3u << 20) /* DTYP=0011b: advanced data */
#define IGB_ADVTXD_DCMD_EOP	(1u << 24)   /* DCMD bit 0: End of Packet */
#define IGB_ADVTXD_DCMD_IFCS	(1u << 25)   /* DCMD bit 1: Insert FCS */
#define IGB_ADVTXD_DCMD_RS	(1u << 27)   /* DCMD bit 3: Report Status */
#define IGB_ADVTXD_DCMD_DEXT	(1u << 29)   /* DCMD bit 5: 1b for advanced */
#define IGB_ADVTXD_PAYLEN_SHIFT	14           /* PAYLEN bit position */

#endif /* _IGB_REGISTERS_H_ */
