/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD Address Translation Library
 *
 * reg_fields.h : Register field definitions for Hygon
 *
 * Author: AichunShi <shiaichun@open-hieco.net>
 */

/*
 * Coherent Station Fabric ID
 *
 * Access type: Instance
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F0x50 [Fabric Block Instance Information 3]
 *	HYGON_DF1   BlockFabricId	[17:8]
 */
#define HYGON_DF1_COH_ST_FABRIC_ID	GENMASK(17, 8)

/*
 * Interleave Number of Sockets
 *
 * Access type: Instance
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F0x110 [DRAM Base Address]
 *	HYGON_DF1 IntLvNumSockets [3:2]
 */
#define HYGON_DF1_INTLV_NUM_SOCKETS	GENMASK(3, 2)

/*
 * Interleave Number of Channels
 *
 * Access type: Instance
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F0x110 [DRAM Base Address]
 *	HYGON_DF1	IntLvNumChan	[7:4]
 */
#define HYGON_DF1_INTLV_NUM_CHAN	GENMASK(7, 4)

/*
 * Interleave Address Select
 *
 * Access type: Instance
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F0x110 [DRAM Base Address]
 *	HYGON_DF1	IntLvAddrSel	[10:8]
 */
#define HYGON_DF1_INTLV_ADDR_SEL	GENMASK(10, 8)

/*
 * Interleave Number of Dies
 *
 * Access type: Instance
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F0x114 [DRAM Limit Address]
 *	HYGON_DF1	IntLvNumDies	[11:10]
 */
#define HYGON_DF1_INTLV_NUM_DIES	GENMASK(11, 10)

/*
 * High Address Offset
 *
 * Access type: Instance
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F0x214 [DRAM Offset]
 *	HYGON_DF1	HiAddrOffset	[31:19]
 */
#define HYGON_DF1_HI_ADDR_OFFSET	GENMASK(31, 19)

/*
 * Die ID Mask
 *
 * Access type: Broadcast
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F1x208 [System Fabric ID Mask]
 *	HYGON_DF1 DieIdMask [9:0]
 */
#define HYGON_DF1_DIE_ID_MASK	GENMASK(9, 0)

/*
 * Die ID Shift
 *
 * Access type: Broadcast
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F1x208 [System Fabric ID Mask]
 *	HYGON_DF1 DieIdShift [15:12]
 */
#define HYGON_DF1_DIE_ID_SHIFT	GENMASK(15, 12)

/*
 * Socket ID Mask
 *
 * Access type: Broadcast
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 * D18F1x208 [System Fabric ID Mask]
 *	HYGON_DF1 SocketIdMask	[25:16]
 */
#define HYGON_DF1_SOCKET_ID_MASK	GENMASK(25, 16)

/*
 * Socket ID Shift
 *
 * Access type: Broadcast
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 * D18F1x208 [System Fabric ID Mask]
 *	HYGON_DF1 SocketIdShift	[31:28]
 */
#define HYGON_DF1_SOCKET_ID_SHIFT	GENMASK(31, 28)
