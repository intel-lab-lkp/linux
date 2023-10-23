/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface framework
 *
 * Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#include <linux/spi/spi.h>

/* Control header */
#define CTRL_HDR_DNC		BIT(31)		/* Data-Not-Control */
#define CTRL_HDR_HDRB		BIT(30)		/* Received Header Bad */
#define CTRL_HDR_WNR		BIT(29)		/* Write-Not-Read */
#define CTRL_HDR_AID		BIT(28)		/* Address Increment Disable */
#define CTRL_HDR_MMS		GENMASK(27, 24)	/* Memory Map Selector */
#define CTRL_HDR_ADDR		GENMASK(23, 8)	/* Address */
#define CTRL_HDR_LEN		GENMASK(7, 1)	/* Length */
#define CTRL_HDR_P		BIT(0)		/* Parity Bit */

#define TC6_HDR_SIZE		4	/* Ctrl command header size as per OA */
#define TC6_FTR_SIZE		4	/* Ctrl command footer size ss per OA */
#define TC6_CTRL_BUF_SIZE	1032	/* Max ctrl buffer size for 128 regs */

/* Open Alliance TC6 Standard Control and Status Registers */
/* Standard Capabilities Register */
#define STDCAP			0x0002
#define CTC			BIT(7)	/* Cut-Through Capability */
#define MINCPS			GENMASK(2, 0)	/* Minimum supported cps */

/* Reset Control and Status Register */
#define RESET			0x0003
#define SWRESET			BIT(0)	/* Software Reset */

/* Configuration Register #0 */
#define CONFIG0			0x0004
#define SYNC			BIT(15)	/* Configuration Synchronization */
#define TXCTE			BIT(9)	/* Tx cut-through enable */
#define RXCTE			BIT(8)	/* Rx cut-through enable */
#define PROTE			BIT(5)	/* Ctrl read/write Protection Enable */
#define CPS			GENMASK(2, 0)	/* Chunk Payload Size */

/* Status Register #0 */
#define STATUS0			0x0008
#define RESETC			BIT(6)	/* Reset Complete */

/* Interrupt Mask Register #0 */
#define IMASK0			0x000C
#define HDREM			BIT(5)	/* Header Error Mask */
#define LOFEM			BIT(4)	/* Loss of Framing Error Mask */
#define RXBOEM			BIT(3)	/* Rx Buffer Overflow Error Mask */
#define TXBUEM			BIT(2)	/* Tx Buffer Underflow Error Mask */
#define TXBOEM			BIT(1)	/* Tx Buffer Overflow Error Mask */
#define TXPEM			BIT(0)	/* Tx Protocol Error Mask */

struct oa_tc6 *oa_tc6_init(struct spi_device *spi);
int oa_tc6_write_register(struct oa_tc6 *tc6, u32 addr, u32 val);
int oa_tc6_read_register(struct oa_tc6 *tc6, u32 addr, u32 *val);
int oa_tc6_write_registers(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len);
int oa_tc6_read_registers(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len);
