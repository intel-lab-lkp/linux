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
/* Reset Control and Status Register */
#define RESET			0x0003
#define SWRESET			BIT(0)	/* Software Reset */

/* Status Register #0 */
#define STATUS0			0x0008
#define RESETC			BIT(6)	/* Reset Complete */

struct oa_tc6 *oa_tc6_init(struct spi_device *spi, bool prote);
int oa_tc6_write_register(struct oa_tc6 *tc6, u32 addr, u32 val);
int oa_tc6_read_register(struct oa_tc6 *tc6, u32 addr, u32 *val);
int oa_tc6_write_registers(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len);
int oa_tc6_read_registers(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len);
