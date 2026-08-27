/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HiSilicon PCIe Trace and Tuning (PTT) support
 * Copyright (c) 2022 HiSilicon Technologies Co., Ltd.
 */

#ifndef INCLUDE__HISI_PTT_PKT_DECODER_H__
#define INCLUDE__HISI_PTT_PKT_DECODER_H__

#include <stddef.h>
#include <stdint.h>
#include <linux/bits.h>
#include <linux/bitfield.h>

#define HISI_PTT_8DW_CHECK_MASK		GENMASK(31, 11)
#define HISI_PTT_IS_8DW_PKT		GENMASK(31, 11)
#define HISI_PTT_MAX_SPACE_LEN		10
#define HISI_PTT_FIELD_LENGTH		4
#define HISI_PTT_PATTERN_LEGACY		0
#define HISI_PTT_PATTERN_V1		1

/* Header DW0 fields for 4DW format */
#define HISI_PTT_HEAD0_4DW_TIME		GENMASK_U32(10, 0)
#define HISI_PTT_HEAD0_4DW_LEN		GENMASK_U32(20, 11)
#define HISI_PTT_HEAD0_4DW_SO		BIT_U32(21)
#define HISI_PTT_HEAD0_4DW_TH		BIT_U32(22)
#define HISI_PTT_HEAD0_4DW_T8		BIT_U32(23)
#define HISI_PTT_HEAD0_4DW_T9		BIT_U32(24)
#define HISI_PTT_HEAD0_4DW_TYPE		GENMASK_U32(29, 25)
#define HISI_PTT_HEAD0_4DW_FORMAT	GENMASK_U32(31, 30)

/* Header DW0 fields for 8DW format */
#define HISI_PTT_HEAD0_8DW_TYPE		GENMASK_U32(28, 24)
#define HISI_PTT_HEAD0_8DW_FORMAT	GENMASK_U32(31, 29)

/* Header DW2 fields for MWr/Msg/MsgD/FetchAdd/Swap/CAS/IORd/IOWr TLPs
 *   bits   [   31   ][     30:23      ][22][21][20][  19:16  ][   15:0   ]
 *          |---------|----------------|----|---|--|-----------|----------|
 *   fields [Reserved][Request Segment][RSV][TV][T][Tag<13:10>][Header DW2]
 */
#define HISI_PTT_HEAD2_HEADER_DW2	GENMASK_U32(15, 0)
#define HISI_PTT_HEAD2_TAG		GENMASK_U32(19, 16)
#define HISI_PTT_HEAD2_T		BIT_U32(20)
#define HISI_PTT_HEAD2_TV		BIT_U32(21)
#define HISI_PTT_HEAD2_RSV		BIT_U32(22)
#define HISI_PTT_HEAD2_REQ_SEG		GENMASK_U32(30, 23)
#define HISI_PTT_HEAD2_RESERVED		BIT_U32(31)

/* Header DW3 fields for CfgRd0/CfgWr0/CfgRd1/CfgWr1 TLPs
 *   bits   [   31   ][       30:23        ][22][21][20][  19:16  ][   15:0   ]
 *          |---------|--------------------|----|---|--|-----------|----------|
 *   fields [Reserved][Destination Segment][DSV][TV][T][Tag<13:10>][Header DW3]
 */
#define HISI_PTT_HEAD3_CFG_HEADER_DW3	GENMASK_U32(15, 0)
#define HISI_PTT_HEAD3_CFG_TAG		GENMASK_U32(19, 16)
#define HISI_PTT_HEAD3_CFG_T		BIT_U32(20)
#define HISI_PTT_HEAD3_CFG_TV		BIT_U32(21)
#define HISI_PTT_HEAD3_CFG_DSV		BIT_U32(22)
#define HISI_PTT_HEAD3_CFG_DST_SEG	GENMASK_U32(30, 23)
#define HISI_PTT_HEAD3_CFG_RESERVED	BIT_U32(31)

/* Header DW3 fields for Cpl/CplD/CplLk/CplDlk TLPs
 *   bits   [       31:24       ][       23:16      ][15][  14:6   ][5][4][   3:0    ]
 *          |--------------------|------------------|----|---------|--|---|----------|
 *   fields [Destination Segment][Completer Segment][DSV][Reserved][TV][T][Tag<13:10>]
 */
#define HISI_PTT_HEAD3_CPL_TAG		GENMASK_U32(3, 0)
#define HISI_PTT_HEAD3_CPL_T		BIT_U32(4)
#define HISI_PTT_HEAD3_CPL_TV		BIT_U32(5)
#define HISI_PTT_HEAD3_CPL_RESERVED	GENMASK_U32(14, 6)
#define HISI_PTT_HEAD3_CPL_DSV		BIT_U32(15)
#define HISI_PTT_HEAD3_CPL_CPL_SEG	GENMASK_U32(23, 16)
#define HISI_PTT_HEAD3_CPL_DST_SEG	GENMASK_U32(31, 24)

enum hisi_ptt_pkt_type {
	HISI_PTT_4DW_PKT,
	HISI_PTT_8DW_PKT,
	HISI_PTT_PKT_MAX
};

enum hisi_ptt_pkt_msg_type {
	HISI_PTT_PKT_TYPE_UNKNOWN,       /* Types do not support analysis */
	HISI_PTT_PKT_TYPE_MWR,           /* P-(MemWr) */
	HISI_PTT_PKT_TYPE_MSG,           /* P-(Message) */
	HISI_PTT_PKT_TYPE_ATOM,          /* NP-(Atomic) */
	HISI_PTT_PKT_TYPE_IO,            /* NP-(IO) */
	HISI_PTT_PKT_TYPE_CFG,           /* NP-(CFG) */
	HISI_PTT_PKT_TYPE_CPL,           /* CPL-(CPL) */
	HISI_PTT_PKT_TYPE_MAX
};

struct hisi_ptt_pkt_buf {
	const unsigned char *buf;
	size_t pos;
	size_t len;
	enum hisi_ptt_pkt_type pkt_type;
	enum hisi_ptt_pkt_msg_type pkt_msg_type;
	size_t pattern;
};

int hisi_ptt_pkt_desc(struct hisi_ptt_pkt_buf *pkt_buf);

#endif
