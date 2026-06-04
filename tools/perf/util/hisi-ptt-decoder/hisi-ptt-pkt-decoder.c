// SPDX-License-Identifier: GPL-2.0
/*
 * HiSilicon PCIe Trace and Tuning (PTT) support
 * Copyright (c) 2022 HiSilicon Technologies Co., Ltd.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <endian.h>
#include <byteswap.h>
#include <linux/bitops.h>
#include <stdarg.h>
#include <linux/kernel.h>

#include "../color.h"
#include "hisi-ptt-pkt-decoder.h"

/*
 * For 8DW format, the bit[31:11] of DW0 is always 0x1fffff, which can be
 * used to distinguish the data format.
 * 8DW format is like:
 *   bits [                 31:11                 ][       10:0       ]
 *        |---------------------------------------|-------------------|
 *    DW0 [                0x1fffff               ][ Reserved (0x7ff) ]
 *    DW1 [                       Prefix                              ]
 *    DW2 [                     Header DW0                            ]
 *    DW3 [                     Header DW1                            ]
 *    DW4 [                     Header DW2                            ]
 *    DW5 [                     Header DW3                            ]
 *    DW6 [                   Reserved (0x0)                          ]
 *    DW7 [                        Time                               ]
 *
 * 4DW format is like:
 *   bits [31:30] [ 29:25 ][24][23][22][21][    20:11   ][    10:0    ]
 *        |-----|---------|---|---|---|---|-------------|-------------|
 *    DW0 [ Fmt ][  Type  ][T9][T8][TH][SO][   Length   ][    Time    ]
 *    DW1 [                     Header DW1                            ]
 *    DW2 [                     Header DW2                            ]
 *    DW3 [                     Header DW3                            ]
 *
 * Header DW2 for MWr/Msg/MsgD/FetchAdd/Swap/CAS/IORd/IOWr is like:
 *   bits   [   31   ][     30:23      ][22][21][20][  19:16  ][   15:0   ]
 *          |---------|----------------|----|---|--|-----------|----------|
 *   fields [Reserved][Request Segment][RSV][TV][T][Tag<13:10>][Header DW2]
 *
 * Header DW3 for CfgRd0/CfgWr0/CfgRd1/CfgWr1 is like:
 *   bits   [   31   ][       30:23        ][22][21][20][  19:16  ][   15:0   ]
 *          |---------|--------------------|----|---|--|-----------|----------|
 *   fields [Reserved][Destination Segment][DSV][TV][T][Tag<13:10>][Header DW3]
 *
 * Header DW3 for Cpl/CplD/CplLk/CplDlk is like:
 *   bits   [       31:24       ][       23:16      ][15][  14:6   ][5][4][   3:0    ]
 *          |--------------------|------------------|----|---------|--|---|----------|
 *   fields [Destination Segment][Completer Segment][DSV][Reserved][TV][T][Tag<13:10>]
 */

enum hisi_ptt_8dw_pkt_field_type {
	HISI_PTT_8DW_CHK_AND_RSV0,
	HISI_PTT_8DW_PREFIX,
	HISI_PTT_8DW_HEAD0,
	HISI_PTT_8DW_HEAD1,
	HISI_PTT_8DW_HEAD2,
	HISI_PTT_8DW_HEAD3,
	HISI_PTT_8DW_RSV1,
	HISI_PTT_8DW_TIME,
	HISI_PTT_8DW_TYPE_MAX
};

enum hisi_ptt_4dw_pkt_field_type {
	HISI_PTT_4DW_HEAD0,
	HISI_PTT_4DW_HEAD1,
	HISI_PTT_4DW_HEAD2,
	HISI_PTT_4DW_HEAD3,
	HISI_PTT_4DW_TYPE_MAX
};

static int hisi_ptt_pkt_size[] = {
	[HISI_PTT_4DW_PKT]	= 16,
	[HISI_PTT_8DW_PKT]	= 32,
};

static const char * const hisi_ptt_8dw_pkt_field_name[] = {
	[HISI_PTT_8DW_CHK_AND_RSV0]	= "CHK & RSV0",
	[HISI_PTT_8DW_PREFIX]		= "Prefix",
	[HISI_PTT_8DW_HEAD0]		= "Header DW0",
	[HISI_PTT_8DW_HEAD1]		= "Header DW1",
	[HISI_PTT_8DW_HEAD2]		= "Header DW2",
	[HISI_PTT_8DW_HEAD3]		= "Header DW3",
	[HISI_PTT_8DW_RSV1]		= "RSV1",
	[HISI_PTT_8DW_TIME]		= "Time"
};

static const char * const hisi_ptt_4dw_pkt_field_name[] = {
	[HISI_PTT_4DW_HEAD0]	= "Header DW0",
	[HISI_PTT_4DW_HEAD1]	= "Header DW1",
	[HISI_PTT_4DW_HEAD2]	= "Header DW2",
	[HISI_PTT_4DW_HEAD3]	= "Header DW3",
};

static bool hisi_ptt_is_mwr_tlp(uint32_t format, uint32_t type)
{
	return (format == 0x2 || format == 0x3) && (type == 0);
}

static bool hisi_ptt_is_msg_tlp(uint32_t format, uint32_t type)
{
	return (format == 0x1 || format == 0x3) && ((type & 0x10) != 0);
}

static bool hisi_ptt_is_io_tlp(uint32_t format, uint32_t type)
{
	return (format == 0 || format == 0x2) && (type == 0x2);
}

static bool hisi_ptt_is_atomic_tlp(uint32_t format, uint32_t type)
{
	return (format == 0x2 || format == 0x3) &&
	       (type == 0xc || type == 0xd || type == 0xe);
}

static bool hisi_ptt_is_cfg_tlp(uint32_t format, uint32_t type)
{
	return (format == 0 || format == 0x2) && (type == 0x4 || type == 0x5);
}

static bool hisi_ptt_is_cpl_tlp(uint32_t format, uint32_t type)
{
	return (format == 0  || format == 0x2) && (type == 0xa || type == 0xb);
}

union hisi_ptt_field_data {
	/* Header DW0 for 4DW format */
	struct {
		uint32_t time : 11;
		uint32_t len : 10;
		uint32_t so : 1;
		uint32_t th : 1;
		uint32_t t8 : 1;
		uint32_t t9 : 1;
		uint32_t type : 5;
		uint32_t format : 2;
	} dw0_4dw;
	/* Header DW0 for 8DW format */
	struct {
		uint32_t others : 24;
		uint32_t type : 5;
		uint32_t format : 3;
	} dw0_8dw;
	/*
	 * Header DW2 for MWr/Msg/MsgD/FetchAdd/Swap/CAS/IORd/IOWr TLPs.
	 * Affects both 4DW and 8DW format.
	 */
	struct {
		uint32_t header_dw2 : 16;
		uint32_t tag : 4;
		uint32_t t : 1;
		uint32_t tv : 1;
		uint32_t rsv : 1;
		uint32_t request_segment : 8;
		uint32_t reserved : 1;
	} dw2_mixed;
	/*
	 * Header DW3 for CfgRd0/CfgWr0/CfgRd1/CfgWr1 TLPs.
	 * Affects both 4DW and 8DW format.
	 */
	struct {
		uint32_t header_dw3 : 16;
		uint32_t tag : 4;
		uint32_t t : 1;
		uint32_t tv : 1;
		uint32_t dsv : 1;
		uint32_t destination_segment : 8;
		uint32_t reserved : 1;
	} dw3_cfg;
	/*
	 * Header DW3 for Cpl/CplD/CplLk/CplDlk TLPs.
	 * Affects both 4DW and 8DW format.
	 */
	struct {
		uint32_t tag : 4;
		uint32_t t : 1;
		uint32_t tv : 1;
		uint32_t reserved : 9;
		uint32_t dsv : 1;
		uint32_t completer_segment : 8;
		uint32_t destination_segment : 8;
	} dw3_cpl;
	uint32_t value;
};

static int hisi_ptt_parse_pkt_msg_type(union hisi_ptt_field_data dw,
				       enum hisi_ptt_pkt_type pkt_type)
{
	uint32_t format, type;

	format = (pkt_type == HISI_PTT_4DW_PKT) ? dw.dw0_4dw.format :
						  dw.dw0_8dw.format;
	type = (pkt_type == HISI_PTT_4DW_PKT) ? dw.dw0_4dw.type :
						dw.dw0_8dw.type;

	if (hisi_ptt_is_mwr_tlp(format, type))
		return HISI_PTT_PKT_TYPE_MWR;
	else if (hisi_ptt_is_msg_tlp(format, type))
		return HISI_PTT_PKT_TYPE_MSG;
	else if (hisi_ptt_is_atomic_tlp(format, type))
		return HISI_PTT_PKT_TYPE_ATOM;
	else if (hisi_ptt_is_io_tlp(format, type))
		return HISI_PTT_PKT_TYPE_IO;
	else if (hisi_ptt_is_cfg_tlp(format, type))
		return HISI_PTT_PKT_TYPE_CFG;
	else if (hisi_ptt_is_cpl_tlp(format, type))
		return HISI_PTT_PKT_TYPE_CPL;

	return HISI_PTT_PKT_TYPE_UNKNOWN;
}

static void hisi_ptt_print_raw_record(size_t offset, uint32_t value)
{
	const char *color = PERF_COLOR_BLUE;
	uint8_t byte;
	int i;

	printf(".");
	color_fprintf(stdout, color, "  %08zx: ", offset);
	for (i = 0; i < HISI_PTT_FIELD_LENGTH; i++) {
		byte = (value >> (24 - i * 8)) & 0xFF;
		color_fprintf(stdout, color, "%02x ", byte);
	}
	for (i = 0; i < HISI_PTT_MAX_SPACE_LEN; i++)
		color_fprintf(stdout, color, "   ");
}

static void hisi_ptt_print_pkt(struct hisi_ptt_pkt_buf *pkt_buf,
			       const char *desc)
{
	const char *color = PERF_COLOR_BLUE;
	uint32_t value;

	value = le32_to_cpu(*(__le32 *)(pkt_buf->buf + pkt_buf->pos));
	hisi_ptt_print_raw_record(pkt_buf->pos, value);

	color_fprintf(stdout, color, "  %s\n", desc);
	pkt_buf->pos += HISI_PTT_FIELD_LENGTH;
}

static void hisi_ptt_print_head0(struct hisi_ptt_pkt_buf *pkt_buf)
{
	const char *color = PERF_COLOR_BLUE;
	union hisi_ptt_field_data dw;

	dw.value = le32_to_cpu(*(__le32 *)(pkt_buf->buf + pkt_buf->pos));
	pkt_buf->pkt_msg_type = hisi_ptt_parse_pkt_msg_type(dw,
							    pkt_buf->pkt_type);
	hisi_ptt_print_raw_record(pkt_buf->pos, dw.value);

	if (pkt_buf->pkt_type == HISI_PTT_4DW_PKT)
		color_fprintf(stdout, color,
			      "  %s %x %s %x %s %x %s %x %s %x %s %x %s %x %s %x\n",
			      "Format", dw.dw0_4dw.format,
			      "Type", dw.dw0_4dw.type,
			      "T9", dw.dw0_4dw.t9, "T8", dw.dw0_4dw.t8,
			      "TH", dw.dw0_4dw.th, "SO", dw.dw0_4dw.so,
			      "Length", dw.dw0_4dw.len,
			      "Time", dw.dw0_4dw.time);
	else
		color_fprintf(stdout, color, "  %s\n",
			      hisi_ptt_8dw_pkt_field_name[HISI_PTT_8DW_HEAD0]);

	pkt_buf->pos += HISI_PTT_FIELD_LENGTH;
}

static void hisi_ptt_print_head1(struct hisi_ptt_pkt_buf *pkt_buf)
{
	const char *color = PERF_COLOR_BLUE;
	union hisi_ptt_field_data dw;

	dw.value = le32_to_cpu(*(__le32 *)(pkt_buf->buf + pkt_buf->pos));
	hisi_ptt_print_raw_record(pkt_buf->pos, dw.value);
	color_fprintf(stdout, color, "  %s\n",
		      pkt_buf->pkt_type == HISI_PTT_4DW_PKT ?
		      hisi_ptt_4dw_pkt_field_name[HISI_PTT_4DW_HEAD1] :
		      hisi_ptt_8dw_pkt_field_name[HISI_PTT_8DW_HEAD1]);

	pkt_buf->pos += HISI_PTT_FIELD_LENGTH;
}

static void hisi_ptt_print_head2(struct hisi_ptt_pkt_buf *pkt_buf)
{
	const char *color = PERF_COLOR_BLUE;
	union hisi_ptt_field_data dw;
	const char *desc = pkt_buf->pkt_type == HISI_PTT_4DW_PKT ?
			   hisi_ptt_4dw_pkt_field_name[HISI_PTT_4DW_HEAD2] :
			   hisi_ptt_8dw_pkt_field_name[HISI_PTT_8DW_HEAD2];

	dw.value = le32_to_cpu(*(__le32 *)(pkt_buf->buf + pkt_buf->pos));
	hisi_ptt_print_raw_record(pkt_buf->pos, dw.value);

	if (pkt_buf->version < HISI_PTT_DECODER_V2)
		color_fprintf(stdout, color, "  %s\n", desc);
	else if (pkt_buf->pkt_msg_type == HISI_PTT_PKT_TYPE_MWR ||
		 pkt_buf->pkt_msg_type == HISI_PTT_PKT_TYPE_MSG ||
		 pkt_buf->pkt_msg_type == HISI_PTT_PKT_TYPE_ATOM ||
		 pkt_buf->pkt_msg_type == HISI_PTT_PKT_TYPE_IO)
		color_fprintf(stdout, color,
			      "  %s %x %s %x %s %x %s %x %s %x %s %x %s %x\n",
			      "Reserved", dw.dw2_mixed.reserved,
			      "Request Segment", dw.dw2_mixed.request_segment,
			      "RSV", dw.dw2_mixed.rsv, "TV", dw.dw2_mixed.tv,
			      "T", dw.dw2_mixed.t, "Tag", dw.dw2_mixed.tag,
			      "Header DW2", dw.dw2_mixed.header_dw2);
	else
		color_fprintf(stdout, color, "  %s\n", desc);

	pkt_buf->pos += HISI_PTT_FIELD_LENGTH;
}

static void hisi_ptt_print_head3(struct hisi_ptt_pkt_buf *pkt_buf)
{
	const char *color = PERF_COLOR_BLUE;
	union hisi_ptt_field_data dw;
	const char *desc = pkt_buf->pkt_type == HISI_PTT_4DW_PKT ?
			   hisi_ptt_4dw_pkt_field_name[HISI_PTT_4DW_HEAD3] :
			   hisi_ptt_8dw_pkt_field_name[HISI_PTT_8DW_HEAD3];

	dw.value = le32_to_cpu(*(__le32 *)(pkt_buf->buf + pkt_buf->pos));
	hisi_ptt_print_raw_record(pkt_buf->pos, dw.value);

	if (pkt_buf->version < HISI_PTT_DECODER_V2)
		color_fprintf(stdout, color, "  %s\n", desc);
	else if (pkt_buf->pkt_msg_type == HISI_PTT_PKT_TYPE_CPL)
		color_fprintf(stdout, color,
			      "  %s %x %s %x %s %x %s %x %s %x %s %x %s %x\n",
			      "Destination Segment",
			      dw.dw3_cpl.destination_segment,
			      "Completer Segment", dw.dw3_cpl.completer_segment,
			      "DSV", dw.dw3_cpl.dsv,
			      "Reserved", dw.dw3_cpl.reserved,
			      "TV", dw.dw3_cpl.tv, "T", dw.dw3_cpl.t,
			      "Tag", dw.dw3_cpl.tag);
	else if (pkt_buf->pkt_msg_type == HISI_PTT_PKT_TYPE_CFG)
		color_fprintf(stdout, color,
			      "  %s %x %s %x %s %x %s %x %s %x %s %x %s %x\n",
			      "Reserved", dw.dw3_cfg.reserved,
			      "Destination Segment",
			      dw.dw3_cfg.destination_segment,
			      "DSV", dw.dw3_cfg.dsv, "TV", dw.dw3_cfg.tv,
			      "T", dw.dw3_cfg.t, "Tag", dw.dw3_cfg.tag,
			      "Header DW3", dw.dw3_cfg.header_dw3);
	else
		color_fprintf(stdout, color, "  %s\n", desc);

	pkt_buf->pos += HISI_PTT_FIELD_LENGTH;
}

static int hisi_ptt_8dw_pkt_desc(struct hisi_ptt_pkt_buf *pkt_buf)
{
	int i;

	for (i = HISI_PTT_8DW_CHK_AND_RSV0; i < HISI_PTT_8DW_TYPE_MAX; i++) {
		/* Do not show 8DW check field and reserved fields */
		if (i == HISI_PTT_8DW_CHK_AND_RSV0 || i == HISI_PTT_8DW_RSV1) {
			pkt_buf->pos += HISI_PTT_FIELD_LENGTH;
			continue;
		}

		switch (i) {
		case HISI_PTT_8DW_HEAD0:
			hisi_ptt_print_head0(pkt_buf);
			break;
		case HISI_PTT_8DW_HEAD1:
			hisi_ptt_print_head1(pkt_buf);
			break;
		case HISI_PTT_8DW_HEAD2:
			hisi_ptt_print_head2(pkt_buf);
			break;
		case HISI_PTT_8DW_HEAD3:
			hisi_ptt_print_head3(pkt_buf);
			break;
		default:
			hisi_ptt_print_pkt(pkt_buf,
					   hisi_ptt_8dw_pkt_field_name[i]);
			break;
		}
	}

	return hisi_ptt_pkt_size[HISI_PTT_8DW_PKT];
}

static int hisi_ptt_4dw_pkt_desc(struct hisi_ptt_pkt_buf *pkt_buf)
{
	hisi_ptt_print_head0(pkt_buf);
	hisi_ptt_print_head1(pkt_buf);
	hisi_ptt_print_head2(pkt_buf);
	hisi_ptt_print_head3(pkt_buf);

	return hisi_ptt_pkt_size[HISI_PTT_4DW_PKT];
}

int hisi_ptt_pkt_desc(struct hisi_ptt_pkt_buf *pkt_buf)
{
	if (pkt_buf->pkt_type == HISI_PTT_8DW_PKT)
		return hisi_ptt_8dw_pkt_desc(pkt_buf);

	return hisi_ptt_4dw_pkt_desc(pkt_buf);
}
