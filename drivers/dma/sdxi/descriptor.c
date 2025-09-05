// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI descriptor encoding.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <kunit/test.h>
#include <kunit/test-bug.h>
#include <kunit/visibility.h>
#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/log2.h>
#include <linux/packing.h>
#include <linux/string.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#include "hw.h"
#include "descriptor.h"

enum {
	SDXI_PACKING_QUIRKS = QUIRK_LITTLE_ENDIAN | QUIRK_LSW32_IS_FIRST,
};

#define sdxi_desc_field(_high, _low, _member) \
	PACKED_FIELD(_high, _low, struct sdxi_desc_unpacked, _member)
#define sdxi_desc_flag(_bit, _member) \
	sdxi_desc_field(_bit, _bit, _member)

static const struct packed_field_u16 common_descriptor_fields[] = {
	sdxi_desc_flag(0, vl),
	sdxi_desc_flag(1, se),
	sdxi_desc_flag(2, fe),
	sdxi_desc_flag(3, ch),
	sdxi_desc_flag(4, csr),
	sdxi_desc_flag(5, rb),
	sdxi_desc_field(15, 8, subtype),
	sdxi_desc_field(26, 16, type),
	sdxi_desc_flag(448, np),
	sdxi_desc_field(511, 453, csb_ptr),
};

void sdxi_desc_unpack(struct sdxi_desc_unpacked *to,
		      const struct sdxi_desc *from)
{
	*to = (struct sdxi_desc_unpacked){};
	unpack_fields(from, sizeof(*from), to, common_descriptor_fields,
		      SDXI_PACKING_QUIRKS);
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_desc_unpack);

static void desc_clear(struct sdxi_desc *desc)
{
	memset(desc, 0, sizeof(*desc));
}

static __must_check int sdxi_encode_size32(u64 size, __le32 *dest)
{
	/*
	 * sizes are encoded as value - 1:
	 * value    encoding
	 *     1           0
	 *     2           1
	 *   ...
	 *    4G  0xffffffff
	 */
	if (WARN_ON_ONCE(size > SZ_4G) ||
	    WARN_ON_ONCE(size == 0))
		return -EINVAL;
	size = clamp_val(size, 1, SZ_4G);
	*dest = cpu_to_le32((u32)(size - 1));
	return 0;
}

int sdxi_encode_copy(struct sdxi_desc *desc, const struct sdxi_copy *params)
{
	u64 csb_ptr;
	u32 opcode;
	__le32 size;
	int err;

	err = sdxi_encode_size32(params->len, &size);
	if (err)
		return err;
	/*
	 * TODO: reject overlapping src and dst. Quoting "Memory
	 * Consistency Model": "Software shall not ... overlap the
	 * source buffer, destination buffer, Atomic Return Data, or
	 * completion status block."
	 */

	opcode = (FIELD_PREP(SDXI_DSC_VL, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_COPY) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_DMAB));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	desc_clear(desc);
	desc->copy = (struct sdxi_dsc_dmab_copy) {
		.opcode = cpu_to_le32(opcode),
		.size = size,
		.akey0 = cpu_to_le16(params->src_akey),
		.akey1 = cpu_to_le16(params->dst_akey),
		.addr0 = cpu_to_le64(params->src),
		.addr1 = cpu_to_le64(params->dst),
		.csb_ptr = cpu_to_le64(csb_ptr),
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_copy);

int sdxi_encode_intr(struct sdxi_desc *desc,
		     const struct sdxi_intr *params)
{
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_VL, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_INTR) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_INTR));

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	desc_clear(desc);
	desc->intr = (struct sdxi_dsc_intr) {
		.opcode = cpu_to_le32(opcode),
		.akey = cpu_to_le16(params->akey),
		.csb_ptr = cpu_to_le64(csb_ptr),
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_intr);

int sdxi_encode_cxt_start(struct sdxi_desc *desc,
			  const struct sdxi_cxt_start *params)
{
	u16 cxt_start;
	u16 cxt_end;
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_VL, 1) |
		  FIELD_PREP(SDXI_DSC_FE, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_START_NM) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	cxt_start = params->range.cxt_start;
	cxt_end = params->range.cxt_end;

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	desc_clear(desc);
	desc->cxt_start = (struct sdxi_dsc_cxt_start) {
		.opcode = cpu_to_le32(opcode),
		.cxt_start = cpu_to_le16(cxt_start),
		.cxt_end = cpu_to_le16(cxt_end),
		.csb_ptr = cpu_to_le64(csb_ptr),
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_start);

int sdxi_encode_cxt_stop(struct sdxi_desc *desc,
			  const struct sdxi_cxt_stop *params)
{
	u16 cxt_start;
	u16 cxt_end;
	u64 csb_ptr;
	u32 opcode;

	opcode = (FIELD_PREP(SDXI_DSC_VL, 1) |
		  FIELD_PREP(SDXI_DSC_FE, 1) |
		  FIELD_PREP(SDXI_DSC_SUBTYPE, SDXI_DSC_OP_SUBTYPE_CXT_STOP) |
		  FIELD_PREP(SDXI_DSC_TYPE, SDXI_DSC_OP_TYPE_ADMIN));

	cxt_start = params->range.cxt_start;
	cxt_end = params->range.cxt_end;

	csb_ptr = FIELD_PREP(SDXI_DSC_NP, 1);

	desc_clear(desc);
	desc->cxt_stop = (struct sdxi_dsc_cxt_stop) {
		.opcode = cpu_to_le32(opcode),
		.cxt_start = cpu_to_le16(cxt_start),
		.cxt_end = cpu_to_le16(cxt_end),
		.csb_ptr = cpu_to_le64(csb_ptr),
	};

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(sdxi_encode_cxt_stop);
