/* SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef _INTEL_DSB_H
#define _INTEL_DSB_H

#include <linux/types.h>

#include "i915_reg_defs.h"

struct intel_crtc;
struct i915_vma;

enum dsb_id {
	INVALID_DSB = -1,
	DSB1,
	DSB2,
	DSB3,
	MAX_DSB_PER_PIPE
};

struct intel_dsb {
	enum dsb_id id;

	u32 *cmd_buf;
	struct i915_vma *vma;
	struct intel_crtc *crtc;

	/*
	 * maximum number of dwords the buffer will hold.
	 */
	unsigned int size;

	/*
	 * free_pos will point the first free dword and
	 * help in calculating tail of command buffer.
	 */
	unsigned int free_pos;

	/*
	 * ins_start_offset will help to store start dword of the dsb
	 * instuction and help in identifying the batch of auto-increment
	 * register.
	 */
	unsigned int ins_start_offset;
};

struct intel_dsb *intel_dsb_prepare(struct intel_crtc *crtc,
				    unsigned int max_cmds);
void intel_dsb_finish(struct intel_dsb *dsb);
void intel_dsb_cleanup(struct intel_dsb *dsb);
void intel_dsb_reg_write(struct intel_dsb *dsb,
			 i915_reg_t reg, u32 val);
void intel_dsb_commit(struct intel_dsb *dsb,
		      bool wait_for_vblank);
void intel_dsb_wait(struct intel_dsb *dsb);
u32 intel_dsb_ggtt_offset(struct intel_dsb *dsb);
void intel_dsb_write(struct intel_dsb *dsb, u32 idx, u32 val);
u32 intel_dsb_read(struct intel_dsb *dsb, u32 idx);
void intel_dsb_memset(struct intel_dsb *dsb, u32 idx, u32 val, u32 sz);
bool intel_dsb_buffer_create(struct intel_crtc *crtc, struct intel_dsb *dsb, u32 size);

#endif
