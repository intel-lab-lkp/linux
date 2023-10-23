/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface framework
 *
 * Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#include <linux/etherdevice.h>
#include <linux/spi/spi.h>

/* Control header */
#define CTRL_HDR_DNC	BIT(31)		/* Data-Not-Control */
#define CTRL_HDR_HDRB	BIT(30)		/* Received Header Bad */
#define CTRL_HDR_WNR	BIT(29)		/* Write-Not-Read */
#define CTRL_HDR_AID	BIT(28)		/* Address Increment Disable */
#define CTRL_HDR_MMS	GENMASK(27, 24)	/* Memory Map Selector */
#define CTRL_HDR_ADDR	GENMASK(23, 8)	/* Address */
#define CTRL_HDR_LEN	GENMASK(7, 1)	/* Length */
#define CTRL_HDR_P	BIT(0)		/* Parity Bit */

/* Data header */
#define DATA_HDR_DNC	BIT(31)		/* Data-Not-Control */
#define DATA_HDR_SEQ	BIT(30)		/* Data Chunk Sequence */
#define DATA_HDR_NORX	BIT(29)		/* No Receive */
#define DATA_HDR_DV	BIT(21)		/* Data Valid */
#define DATA_HDR_SV	BIT(20)		/* Start Valid */
#define DATA_HDR_SWO	GENMASK(19, 16)	/* Start Word Offset */
#define DATA_HDR_EV	BIT(14)		/* End Valid */
#define DATA_HDR_EBO	GENMASK(13, 8)	/* End Byte Offset */
#define DATA_HDR_P	BIT(0)		/* Header Parity Bit */

/* Data footer */
#define DATA_FTR_EXST	BIT(31)		/* Extended Status */
#define DATA_FTR_HDRB	BIT(30)		/* Received Header Bad */
#define DATA_FTR_SYNC	BIT(29)		/* Configuration Synchronized */
#define DATA_FTR_RCA	GENMASK(28, 24)	/* Receive Chunks Available */
#define DATA_FTR_DV	BIT(21)		/* Data Valid */
#define DATA_FTR_SV	BIT(20)		/* Start Valid */
#define DATA_FTR_SWO	GENMASK(19, 16)	/* Start Word Offset */
#define DATA_FTR_FD	BIT(15)		/* Frame Drop */
#define DATA_FTR_EV	BIT(14)		/* End Valid */
#define DATA_FTR_EBO	GENMASK(13, 8)	/* End Byte Offset */
#define DATA_FTR_TXC	GENMASK(5, 1)	/* Transmit Credits */
#define DATA_FTR_P	BIT(0)		/* Footer Parity Bit */

#define TC6_HDR_SIZE		4	/* Ctrl command header size as per OA */
#define TC6_FTR_SIZE		4	/* Ctrl command footer size ss per OA */
#define TC6_CTRL_BUF_SIZE	1032	/* Max ctrl buffer size for 128 regs */

#define FTR_OK		0
#define FTR_ERR		1

#define ETH_LEN		(ETH_DATA_LEN + ETH_HLEN + ETH_FCS_LEN)
#define OA_TC6_MAX_CPS	64

/* Open Alliance TC6 Standard Control and Status Registers */
/* Standard Capabilities Register */
#define STDCAP		0x0002
#define IPRAC		BIT(9)	/* Indirect PHY Reg access Capability */
#define DPRAC		BIT(8)	/* Direct PHY Reg Access Capability */
#define CTC		BIT(7)	/* Cut-Through Capability */
#define MINCPS		GENMASK(2, 0)	/* Minimum supported cps */

/* Reset Control and Status Register */
#define RESET		0x0003
#define SWRESET		BIT(0)	/* Software Reset */

/* Configuration Register #0 */
#define CONFIG0		0x0004
#define SYNC		BIT(15)	/* Configuration Synchronization */
#define TXCTE		BIT(9)	/* Tx cut-through enable */
#define RXCTE		BIT(8)	/* Rx cut-through enable */
#define PROTE		BIT(5)	/* Ctrl read/write Protection Enable */
#define CPS		GENMASK(2, 0)	/* Chunk Payload Size */

/* Status Register #0 */
#define STATUS0		0x0008
#define CDPE		BIT(12)	/* Control Data Protection Error */
#define TXFCSE		BIT(11)	/* Transmit Frame Check Sequence Error */
#define RESETC		BIT(6)	/* Reset Complete */
#define HDRE		BIT(5)	/* Header Error */
#define LOFE		BIT(4)	/* Loss of Framing Error */
#define RXBOE		BIT(3)	/* Receive Buffer Overflow Error */
#define TXBUE		BIT(2)	/* Transmit Buffer Underflow Error */
#define TXBOE		BIT(1)	/* Transmit Buffer Overflow Error */
#define TXPE		BIT(0)	/* Transmit Protocol Error */

/* Buffer Status Register */
#define OA_TC6_BUFSTS	0x000B
#define TXC		GENMASK(15, 8)	/* Transmit Credits Available */
#define RCA		GENMASK(7, 0)	/* Receive Chunks Available */

/* Interrupt Mask Register #0 */
#define IMASK0		0x000C
#define HDREM		BIT(5)	/* Header Error Mask */
#define LOFEM		BIT(4)	/* Loss of Framing Error Mask */
#define RXBOEM		BIT(3)	/* Rx Buffer Overflow Error Mask */
#define TXBUEM		BIT(2)	/* Tx Buffer Underflow Error Mask */
#define TXBOEM		BIT(1)	/* Tx Buffer Overflow Error Mask */
#define TXPEM		BIT(0)	/* Tx Protocol Error Mask */

struct oa_tc6 *oa_tc6_init(struct spi_device *spi, struct net_device *netdev,
			   int (*config_cps_buf)(void *tc6, u32 cps));
void oa_tc6_exit(struct oa_tc6 *tc6);
int oa_tc6_write_register(struct oa_tc6 *tc6, u32 addr, u32 val);
int oa_tc6_read_register(struct oa_tc6 *tc6, u32 addr, u32 *val);
int oa_tc6_write_registers(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len);
int oa_tc6_read_registers(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len);
netdev_tx_t oa_tc6_send_eth_pkt(struct oa_tc6 *tc6, struct sk_buff *skb);
