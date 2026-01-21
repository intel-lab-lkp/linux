// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Microsoft Corporation.
 *
 * Authors: Microsoft Linux virtualization team
 */

/*
	root	l1vh	vtl
vmbus

guest
vmbus, nothing else

vtl
mshv_vtl uses intercept SINT, VTL2_VMBUS_SINT_INDEX (7, not in hvgdk_mini lol)
vmbus

bm root
mshv_root, no vmbus

nested root
mshv_root uses L1
vmbus uses L0 (NESTED regs)

l1vh
mshv_root and vmbus use same regs

*/

struct hv_synic_page {
	u64 msr;
	void *ptr;
	struct kref refcount;
};

void *hv_get_synic_page(u32 msr) {
	struct hv_synic_page *page_obj;
	page_obj = kmalloc
}


#define HV_SYNIC_PAGE_STRUCT(type, name) \
struct 

/* UGH */
struct hv_percpu_synic_cxt {
	struct {
		struct hv_message_page *ptr;
		refcount_t pt_ref_count;
	} hv_simp;
	struct hv_message_page *hv_simp;
	struct hv_synic_event_flags_page *hv_siefp;
	struct hv_synic_event_ring_page *hv_sierp;
};

int hv_setup_sint(u32 sint_msr)
{
	union hv_synic_sint sint;

	// TODO validate sint_msr

	sint.as_uint64 = hv_get_msr(sint_msr);
	sint.vector = vmbus_interrupt;
	sint.masked = false;
	sint.auto_eoi = hv_recommend_using_aeoi();

	hv_set_msr(sint_msr, sint.as_uint64);

	return 0;
}

void *hv_setup_synic_page(u32 msr)
{
	void *addr;
	struct hv_synic_page synic_page;

	// TODO validate msr

	synic_page.as_uint64 = hv_get_msr(msr);
	synic_page.enabled = 1;

	if (ms_hyperv.paravisor_present || hv_root_partition()) {
		/* Mask out vTOM bit. ioremap_cache() maps decrypted */
		u64 base = (synic_page.gpa << HV_HYP_PAGE_SHIFT) &
			    ~ms_hyperv.shared_gpa_boundary;
		addr = (void *)ioremap_cache(base, HV_HYP_PAGE_SIZE);
		if (!addr) {
			pr_err("%s: Fail to map synic page from %#x.\n",
			       __func__, msr);
			return NULL;
		}
	} else {
		addr = (void *)__get_free_page(GFP_KERNEL);
		if (!page)
			return NULL;

		memset(page, 0, PAGE_SIZE);
		synic_page.gpa = virt_to_phys(addr) >> HV_HYP_PAGE_SHIFT;
	}
	hv_set_msr(msr, synic_page.as_uint64);

	return addr;
}

/*
 * hv_hyp_synic_enable_regs - Initialize the Synthetic Interrupt Controller
 * with the hypervisor.
 */
void hv_hyp_synic_enable_regs(unsigned int cpu)
{
	struct hv_per_cpu_context *hv_cpu =
		per_cpu_ptr(hv_context.cpu_context, cpu);
	union hv_synic_simp simp;
	union hv_synic_siefp siefp;
	union hv_synic_sint shared_sint;

	/* Setup the Synic's message page with the hypervisor. */
	simp.as_uint64 = hv_get_msr(HV_MSR_SIMP);
	simp.simp_enabled = 1;

	if (ms_hyperv.paravisor_present || hv_root_partition()) {
		/* Mask out vTOM bit. ioremap_cache() maps decrypted */
		u64 base = (simp.base_simp_gpa << HV_HYP_PAGE_SHIFT) &
				~ms_hyperv.shared_gpa_boundary;
		hv_cpu->hyp_synic_message_page =
			(void *)ioremap_cache(base, HV_HYP_PAGE_SIZE);
		if (!hv_cpu->hyp_synic_message_page)
			pr_err("Fail to map synic message page.\n");
	} else {
		simp.base_simp_gpa = virt_to_phys(hv_cpu->hyp_synic_message_page)
			>> HV_HYP_PAGE_SHIFT;
	}

	hv_set_msr(HV_MSR_SIMP, simp.as_uint64);

	/* Setup the Synic's event page with the hypervisor. */
	siefp.as_uint64 = hv_get_msr(HV_MSR_SIEFP);
	siefp.siefp_enabled = 1;

	if (ms_hyperv.paravisor_present || hv_root_partition()) {
		/* Mask out vTOM bit. ioremap_cache() maps decrypted */
		u64 base = (siefp.base_siefp_gpa << HV_HYP_PAGE_SHIFT) &
				~ms_hyperv.shared_gpa_boundary;
		hv_cpu->hyp_synic_event_page =
			(void *)ioremap_cache(base, HV_HYP_PAGE_SIZE);
		if (!hv_cpu->hyp_synic_event_page)
			pr_err("Fail to map synic event page.\n");
	} else {
		siefp.base_siefp_gpa = virt_to_phys(hv_cpu->hyp_synic_event_page)
			>> HV_HYP_PAGE_SHIFT;
	}

	hv_set_msr(HV_MSR_SIEFP, siefp.as_uint64);
	hv_enable_coco_interrupt(cpu, vmbus_interrupt, true);

	/* Setup the shared SINT. */
	if (vmbus_irq != -1)
		enable_percpu_irq(vmbus_irq, 0);
	shared_sint.as_uint64 = hv_get_msr(HV_MSR_SINT0 + VMBUS_MESSAGE_SINT);

	shared_sint.vector = vmbus_interrupt;
	shared_sint.masked = false;
	shared_sint.auto_eoi = hv_recommend_using_aeoi();
	hv_set_msr(HV_MSR_SINT0 + VMBUS_MESSAGE_SINT, shared_sint.as_uint64);
}

static void hv_hyp_synic_enable_interrupts(void)
{
	union hv_synic_scontrol sctrl;

	/* Enable the global synic bit */
	sctrl.as_uint64 = hv_get_msr(HV_MSR_SCONTROL);
	sctrl.enable = 1;

	hv_set_msr(HV_MSR_SCONTROL, sctrl.as_uint64);
}
