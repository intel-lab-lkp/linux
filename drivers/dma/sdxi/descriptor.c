// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI descriptor encoding.
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <kunit/visibility.h>
#include <linux/bitfield.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#include "hw.h"
#include "descriptor.h"

int sdxi_encode_cxt_start(struct sdxi_desc *desc,
			  const struct sdxi_cxt_start *params)
{
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_FE, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_START_NM) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.cxt_start = (typeof(desc->cxt_start)) {
			.opcode = cpu_to_le32(opcode),
			.cxt_start = cpu_to_le16(params->range.cxt_start),
			.cxt_end = cpu_to_le16(params->range.cxt_end),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_start);

int sdxi_encode_cxt_stop(struct sdxi_desc *desc,
			  const struct sdxi_cxt_stop *params)
{
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_FE, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_STOP) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.cxt_stop = (typeof(desc->cxt_stop)) {
			.opcode = cpu_to_le32(opcode),
			.cxt_start = cpu_to_le16(params->range.cxt_start),
			.cxt_end = cpu_to_le16(params->range.cxt_end),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_stop);

int sdxi_encode_sync(struct sdxi_desc *desc, const struct sdxi_sync *params)
{
	u64 csb_ptr;
	u32 opcode;
	u8 cflags;

	opcode = (FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_SYNC) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	cflags = FIELD_PREP(SDXI_DSC_SYNC_FLT, params->filter);

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	*desc = (typeof(*desc)) {
		.sync = (typeof(desc->sync)) {
			.opcode = cpu_to_le32(opcode),
			.cflags = cflags,
			.cxt_start = cpu_to_le16(params->range.cxt_start),
			.cxt_end = cpu_to_le16(params->range.cxt_end),
			.csb_ptr = cpu_to_le64(csb_ptr),
		},
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_sync);
