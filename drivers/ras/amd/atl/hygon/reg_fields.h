/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD Address Translation Library
 *
 * reg_fields.h : Register field definitions for Hygon
 *
 * Author: AichunShi <shiaichun@open-hieco.net>
 */

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
