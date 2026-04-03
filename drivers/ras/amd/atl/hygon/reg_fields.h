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
 *	HYGON_DF2   BlockFabricId	[18:8]
 *	HYGON_DF3   BlockFabricId	[15:8]
 */
#define HYGON_DF1_COH_ST_FABRIC_ID	GENMASK(17, 8)
#define HYGON_DF2_COH_ST_FABRIC_ID	GENMASK(18, 8)
#define HYGON_DF3_COH_ST_FABRIC_ID	GENMASK(15, 8)

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
 *	HYGON_DF2 IntLvNumSockets [3:2]
 *
 *	D18F0x114 [DRAM Limit Address]
 *	HYGON_DF3 IntLvNumSockets [8]
 */
#define HYGON_DF1_INTLV_NUM_SOCKETS	GENMASK(3, 2)
#define HYGON_DF3_INTLV_NUM_SOCKETS	BIT(8)

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
 *	HYGON_DF2	IntLvNumChan	[7:4]
 *	HYGON_DF3	IntLvNumChan	[7:4]
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
 *	HYGON_DF2	IntLvAddrSel	[10:8]
 *	HYGON_DF3	IntLvAddrSel	[10:8]
 */
#define HYGON_DF1_INTLV_ADDR_SEL	GENMASK(10, 8)

/*
 * Destination Fabric ID
 *
 * Access type: Instance
 *
 * Register
 *	Rev	Fieldname	Bits
 *
 *	D18F0x114 [DRAM Limit Address]
 *	HYGON_DF1	DstFabricID	[9:0]
 *	HYGON_DF2	DstFabricID	[10:0]
 *	HYGON_DF3	DstFabricID	[7:0]
 */
#define HYGON_DF1_DST_FABRIC_ID	GENMASK(9, 0)
#define HYGON_DF2_DST_FABRIC_ID	GENMASK(10, 0)
#define HYGON_DF3_DST_FABRIC_ID	GENMASK(7, 0)

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
 *	HYGON_DF3	IntLvNumDies	[11:10]
 *
 *	D18F0x60
 *	HYGON_DF2	IntLvNumDies	[1:0]
 */
#define HYGON_DF1_INTLV_NUM_DIES	GENMASK(11, 10)
#define HYGON_DF2_INTLV_NUM_DIES	GENMASK(1, 0)

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
 *	HYGON_DF2	HiAddrOffset	[31:19]
 *
 *  D18F0x1B4 [DRAM Offset]
 *	HYGON_DF3	HiAddrOffset	[31:20]
 */
#define HYGON_DF1_HI_ADDR_OFFSET	GENMASK(31, 19)
#define HYGON_DF3_HI_ADDR_OFFSET	GENMASK(31, 20)

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
 *	HYGON_DF2 DieIdMask [10:0]
 *	HYGON_DF3 DieIdMask [15:8]
 */
#define HYGON_DF1_DIE_ID_MASK	GENMASK(9, 0)
#define HYGON_DF2_DIE_ID_MASK	GENMASK(10, 0)
#define HYGON_DF3_DIE_ID_MASK	GENMASK(15, 8)

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
 *	HYGON_DF2 DieIdShift [15:12]
 *	HYGON_DF3 DieIdShift [27:24]
 */
#define HYGON_DF1_DIE_ID_SHIFT	GENMASK(15, 12)
#define HYGON_DF3_DIE_ID_SHIFT	GENMASK(27, 24)

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
 *	HYGON_DF2 SocketIdMask	[26:16]
 *	HYGON_DF3 SocketIdMask	[23:16]
 */
#define HYGON_DF1_SOCKET_ID_MASK	GENMASK(25, 16)
#define HYGON_DF2_SOCKET_ID_MASK	GENMASK(26, 16)
#define HYGON_DF3_SOCKET_ID_MASK	GENMASK(23, 16)

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
 *	HYGON_DF2 SocketIdShift	[31:28]
 *	HYGON_DF3 SocketIdShift	[31:28]
 */
#define HYGON_DF1_SOCKET_ID_SHIFT	GENMASK(31, 28)
