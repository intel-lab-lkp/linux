// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Memory Encryption Support
 *
 * Copyright (C) 2019 SUSE
 *
 * Author: Joerg Roedel <jroedel@suse.de>
 */

#define pr_fmt(fmt)	"SEV: " fmt

#include <linux/percpu-defs.h>
#include <linux/cc_platform.h>
#include <linux/printk.h>
#include <linux/mm_types.h>
#include <linux/set_memory.h>
#include <linux/memblock.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/cpumask.h>
#include <linux/efi.h>
#include <linux/io.h>
#include <linux/psp-sev.h>
#include <uapi/linux/sev-guest.h>

#include <asm/init.h>
#include <asm/cpu_entry_area.h>
#include <asm/stacktrace.h>
#include <asm/sev.h>
#include <asm/sev-internal.h>
#include <asm/insn-eval.h>
#include <asm/fpu/xcr.h>
#include <asm/processor.h>
#include <asm/realmode.h>
#include <asm/setup.h>
#include <asm/traps.h>
#include <asm/svm.h>
#include <asm/smp.h>
#include <asm/cpu.h>
#include <asm/apic.h>
#include <asm/cpuid.h>
#include <asm/cmdline.h>

/* Include code shared with pre-decompression boot stage */
#include "sev-shared.c"

void __init
early_set_pages_state(unsigned long vaddr, unsigned long paddr,
		      unsigned long npages, enum psc_op op)
{
	unsigned long paddr_end;
	u64 val;

	vaddr = vaddr & PAGE_MASK;

	paddr = paddr & PAGE_MASK;
	paddr_end = paddr + (npages << PAGE_SHIFT);

	while (paddr < paddr_end) {
		/* Page validation must be rescinded before changing to shared */
		if (op == SNP_PAGE_STATE_SHARED)
			pvalidate_4k_page(vaddr, paddr, false);

		/*
		 * Use the MSR protocol because this function can be called before
		 * the GHCB is established.
		 */
		sev_es_wr_ghcb_msr(GHCB_MSR_PSC_REQ_GFN(paddr >> PAGE_SHIFT, op));
		VMGEXIT();

		val = sev_es_rd_ghcb_msr();

		if (GHCB_RESP_CODE(val) != GHCB_MSR_PSC_RESP)
			goto e_term;

		if (GHCB_MSR_PSC_RESP_VAL(val))
			goto e_term;

		/* Page validation must be performed after changing to private */
		if (op == SNP_PAGE_STATE_PRIVATE)
			pvalidate_4k_page(vaddr, paddr, true);

		vaddr += PAGE_SIZE;
		paddr += PAGE_SIZE;
	}

	return;

e_term:
	sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_PSC);
}

void __init early_snp_set_memory_private(unsigned long vaddr, unsigned long paddr,
					 unsigned long npages)
{
	/*
	 * This can be invoked in early boot while running identity mapped, so
	 * use an open coded check for SNP instead of using cc_platform_has().
	 * This eliminates worries about jump tables or checking boot_cpu_data
	 * in the cc_platform_has() function.
	 */
	if (!(sev_status & MSR_AMD64_SEV_SNP_ENABLED))
		return;

	 /*
	  * Ask the hypervisor to mark the memory pages as private in the RMP
	  * table.
	  */
	early_set_pages_state(vaddr, paddr, npages, SNP_PAGE_STATE_PRIVATE);
}

void __init early_snp_set_memory_shared(unsigned long vaddr, unsigned long paddr,
					unsigned long npages)
{
	/*
	 * This can be invoked in early boot while running identity mapped, so
	 * use an open coded check for SNP instead of using cc_platform_has().
	 * This eliminates worries about jump tables or checking boot_cpu_data
	 * in the cc_platform_has() function.
	 */
	if (!(sev_status & MSR_AMD64_SEV_SNP_ENABLED))
		return;

	 /* Ask hypervisor to mark the memory pages shared in the RMP table. */
	early_set_pages_state(vaddr, paddr, npages, SNP_PAGE_STATE_SHARED);
}

/*
 * Initial set up of SNP relies on information provided by the
 * Confidential Computing blob, which can be passed to the kernel
 * in the following ways, depending on how it is booted:
 *
 * - when booted via the boot/decompress kernel:
 *   - via boot_params
 *
 * - when booted directly by firmware/bootloader (e.g. CONFIG_PVH):
 *   - via a setup_data entry, as defined by the Linux Boot Protocol
 *
 * Scan for the blob in that order.
 */
static struct cc_blob_sev_info *__init find_cc_blob(struct boot_params *bp)
{
	struct cc_blob_sev_info *cc_info;

	/* Boot kernel would have passed the CC blob via boot_params. */
	if (bp->cc_blob_address) {
		cc_info = (struct cc_blob_sev_info *)(unsigned long)bp->cc_blob_address;
		goto found_cc_info;
	}

	/*
	 * If kernel was booted directly, without the use of the
	 * boot/decompression kernel, the CC blob may have been passed via
	 * setup_data instead.
	 */
	cc_info = find_cc_blob_setup_data(bp);
	if (!cc_info)
		return NULL;

found_cc_info:
	if (cc_info->magic != CC_BLOB_SEV_HDR_MAGIC)
		snp_abort();

	return cc_info;
}

static void __init svsm_setup(struct cc_blob_sev_info *cc_info)
{
	struct snp_secrets_page *secrets = (void *)cc_info->secrets_phys;
	struct svsm_call call = {};
	int ret;
	u64 pa;

	/*
	 * Record the SVSM Calling Area address (CAA) if the guest is not
	 * running at VMPL0. The CA will be used to communicate with the
	 * SVSM to perform the SVSM services.
	 */
	if (!svsm_setup_ca(cc_info))
		return;

	/*
	 * It is very early in the boot and the kernel is running identity
	 * mapped but without having adjusted the pagetables to where the
	 * kernel was loaded (physbase), so the get the CA address using
	 * RIP-relative addressing.
	 */
	pa = (u64)rip_rel_ptr(&boot_svsm_ca_page);

	/*
	 * Switch over to the boot SVSM CA while the current CA is still
	 * addressable. There is no GHCB at this point so use the MSR protocol.
	 *
	 * SVSM_CORE_REMAP_CA call:
	 *   RAX = 0 (Protocol=0, CallID=0)
	 *   RCX = New CA GPA
	 */
	call.caa = (struct svsm_ca *)secrets->svsm_caa;
	call.rax = SVSM_CORE_CALL(SVSM_CORE_REMAP_CA);
	call.rcx = pa;
	ret = svsm_perform_msr_protocol(&call);
	if (ret)
		sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_SVSM_CA_REMAP_FAIL);

	boot_svsm_caa = (struct svsm_ca *)pa;
	boot_svsm_caa_pa = pa;
}

bool __init snp_init(struct boot_params *bp)
{
	struct cc_blob_sev_info *cc_info;

	if (!bp)
		return false;

	cc_info = find_cc_blob(bp);
	if (!cc_info)
		return false;

	if (cc_info->secrets_phys && cc_info->secrets_len == PAGE_SIZE)
		sev_secrets_pa = cc_info->secrets_phys;
	else
		return false;

	setup_cpuid_table(cc_info);

	svsm_setup(cc_info);

	/*
	 * The CC blob will be used later to access the secrets page. Cache
	 * it here like the boot kernel does.
	 */
	bp->cc_blob_address = (u32)(unsigned long)cc_info;

	return true;
}

void __init __noreturn snp_abort(void)
{
	sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SNP_UNSUPPORTED);
}
