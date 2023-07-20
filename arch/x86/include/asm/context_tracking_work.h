/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CONTEXT_TRACKING_WORK_H
#define _ASM_X86_CONTEXT_TRACKING_WORK_H

#include <asm/sync_core.h>
#include <asm/tlbflush.h>

static __always_inline void arch_context_tracking_work(int work)
{
	switch (work) {
	case CONTEXT_WORK_SYNC:
		sync_core();
		break;
	case CONTEXT_WORK_TLBI:
		__flush_tlb_all_noinstr();
		break;
	}
}

#endif
