/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DMA_SDXI_DESCRIPTOR_H
#define DMA_SDXI_DESCRIPTOR_H

/*
 * Facilities for encoding SDXI descriptors.
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/ratelimit.h>
#include <linux/types.h>
#include <asm/byteorder.h>

#include "hw.h"

static inline void sdxi_desc_vl_expect(const struct sdxi_desc *desc, bool expected)
{
	u8 vl = FIELD_GET(SDXI_DSC_VL, le32_to_cpu(desc->opcode));

	WARN_RATELIMIT(vl != expected, "expected vl=%u but got %u\n", expected, vl);
}

static inline void sdxi_desc_set_csb(struct sdxi_desc *desc, dma_addr_t addr)
{
	sdxi_desc_vl_expect(desc, 0);
	desc->csb_ptr = cpu_to_le64(FIELD_PREP(SDXI_DSC_CSB_PTR, addr >> 5));
}

static inline void sdxi_desc_make_valid(struct sdxi_desc *desc)
{
	u32 opcode = le32_to_cpu(desc->opcode);

	sdxi_desc_vl_expect(desc, 0);
	FIELD_MODIFY(SDXI_DSC_VL, &opcode, 1);
	/*
	 * Once vl is set, no more modifications to the descriptor
	 * payload are allowed. Ensure the vl update is ordered after
	 * all other initialization of the descriptor.
	 */
	dma_wmb();
	WRITE_ONCE(desc->opcode, cpu_to_le32(opcode));
}

static inline void sdxi_desc_set_fence(struct sdxi_desc *desc)
{
	u32 opcode = le32_to_cpu(desc->opcode);

	sdxi_desc_vl_expect(desc, 0);
	FIELD_MODIFY(SDXI_DSC_FE, &opcode, 1);
	desc->opcode = cpu_to_le32(opcode);
}

static inline void sdxi_desc_set_sequential(struct sdxi_desc *desc)
{
	u32 opcode = le32_to_cpu(desc->opcode);

	sdxi_desc_vl_expect(desc, 0);
	FIELD_MODIFY(SDXI_DSC_SE, &opcode, 1);
	desc->opcode = cpu_to_le32(opcode);
}

#endif /* DMA_SDXI_DESCRIPTOR_H */
