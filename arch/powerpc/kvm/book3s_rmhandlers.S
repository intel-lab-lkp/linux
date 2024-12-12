/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *
 * Copyright SUSE Linux Products GmbH 2009
 *
 * Authors: Alexander Graf <agraf@suse.de>
 */

#include <asm/ppc_asm.h>
#include <asm/kvm_asm.h>
#include <asm/reg.h>
#include <asm/mmu.h>
#include <asm/page.h>
#include <asm/asm-offsets.h>
#include <asm/asm-compat.h>

#include <asm/exception-64s.h>

/*****************************************************************************
 *                                                                           *
 *        Real Mode handlers that need to be in low physical memory          *
 *                                                                           *
 ****************************************************************************/

#ifdef CONFIG_PPC64_ELF_ABI_V2
#define FUNC(name) 		name
#else
#define FUNC(name) 		GLUE(.,name)
#endif

/*
 * Call kvmppc_handler_trampoline_enter in real mode
 *
 * On entry, r4 contains the guest shadow MSR
 * MSR.EE has to be 0 when calling this function
 */
_GLOBAL_TOC(kvmppc_entry_trampoline)
	mfmsr	r5
	LOAD_REG_ADDR(r7, kvmppc_handler_trampoline_enter)
	toreal(r7)

	li	r6, MSR_IR | MSR_DR
	andc	r6, r5, r6	/* Clear DR and IR in MSR value */
	/*
	 * Set EE in HOST_MSR so that it's enabled when we get into our
	 * C exit handler function.
	 */
	ori	r5, r5, MSR_EE
	mtsrr0	r7
	mtsrr1	r6
	RFI_TO_KERNEL

#include "book3s_segment.S"
