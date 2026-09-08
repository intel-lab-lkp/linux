/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_STACKDEPOT_H
#define __ASM_STACKDEPOT_H

#include <linux/types.h>
#include <asm/sections.h>

/*
 * Modules are allocated inside a 2 GB relocation window containing the
 * kernel image. Store a signed 32-bit offset from _text so compression is
 * independent of 4 GB high-bit boundaries crossed by that window.
 */
static inline unsigned long arch_stack_depot_frame_from_payload(u32 payload)
{
	long offset;

	offset = (s32)payload;
	if (offset < 0)
		return (unsigned long)_text - (unsigned long)(-offset);
	return (unsigned long)_text + (unsigned long)offset;
}

static inline bool
arch_stack_depot_frame_try_compress(unsigned long frame, u32 *payload)
{
	u32 candidate;

	candidate = (u32)(frame - (unsigned long)_text);
	if (arch_stack_depot_frame_from_payload(candidate) != frame)
		return false;

	*payload = candidate;
	return true;
}

static inline void
arch_stack_depot_frame_decompress(u32 payload, unsigned long *frame)
{
	*frame = arch_stack_depot_frame_from_payload(payload);
}

#endif /* __ASM_STACKDEPOT_H */
