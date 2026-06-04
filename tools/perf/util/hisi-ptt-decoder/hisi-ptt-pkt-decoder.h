/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HiSilicon PCIe Trace and Tuning (PTT) support
 * Copyright (c) 2022 HiSilicon Technologies Co., Ltd.
 */

#ifndef INCLUDE__HISI_PTT_PKT_DECODER_H__
#define INCLUDE__HISI_PTT_PKT_DECODER_H__

#include <stddef.h>
#include <stdint.h>

#define HISI_PTT_8DW_CHECK_MASK		GENMASK(31, 11)
#define HISI_PTT_IS_8DW_PKT		GENMASK(31, 11)
#define HISI_PTT_MAX_SPACE_LEN		10
#define HISI_PTT_FIELD_LENGTH		4

enum hisi_ptt_pkt_type {
	HISI_PTT_4DW_PKT,
	HISI_PTT_8DW_PKT,
	HISI_PTT_PKT_MAX
};

static int hisi_ptt_pkt_size[] = {
	[HISI_PTT_4DW_PKT]	= 16,
	[HISI_PTT_8DW_PKT]	= 32,
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
};

int hisi_ptt_pkt_desc(struct hisi_ptt_pkt_buf *pkt_buf);

#endif
