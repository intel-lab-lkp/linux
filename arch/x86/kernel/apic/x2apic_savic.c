// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Secure AVIC Support (SEV-SNP Guests)
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 * Author: Neeraj Upadhyay <Neeraj.Upadhyay@amd.com>
 */

#include <linux/cpumask.h>
#include <linux/cc_platform.h>
#include <linux/percpu-defs.h>
#include <linux/align.h>
#include <linux/sizes.h>
#include <linux/llist.h>

#include <asm/apic.h>
#include <asm/sev.h>

#include "local.h"

#define VEC_POS(v)	((v) & (32 - 1))
#define REG_POS(v)	(((v) >> 5) << 4)

static DEFINE_PER_CPU(void *, apic_backing_page);

struct apic_id_node {
	 struct llist_node node;
	 u32 apic_id;
	 int cpu;
};

static DEFINE_PER_CPU(struct apic_id_node, apic_id_node);

static struct llist_head *apic_id_map;

static int x2apic_savic_acpi_madt_oem_check(char *oem_id, char *oem_table_id)
{
	return x2apic_enabled() && cc_platform_has(CC_ATTR_SNP_SECURE_AVIC);
}

static inline u32 get_reg(char *page, int reg)
{
	return READ_ONCE(*((u32 *)(page + reg)));
}

static inline void set_reg(char *page, int reg, u32 val)
{
	WRITE_ONCE(*((u32 *)(page + reg)), val);
}

#define SAVIC_ALLOWED_IRR_OFFSET	0x204

static u32 x2apic_savic_read(u32 reg)
{
	void *backing_page = this_cpu_read(apic_backing_page);

	/*
	 * When Secure AVIC is enabled, rdmsr/wrmsr of APIC registers result in
	 * #VC exception (for non-accelerated register accesses). The #VC
	 * exception handler can read/write the x2APIC register in the guest
	 * APIC backing page. Since doing this would increase the latency of
	 * accessing x2APIC registers, instead of doing rdmsr/wrmsr based
	 * accesses and handling apic register reads/writes in
	 * #VC VMEXIT_AVIC_NOACCEL error condition, the read() and write()
	 * callbacks of Secure AVIC driver directly read/write APIC register
	 * from/to the guest APIC backing page.
	 */
	switch (reg) {
	case APIC_LVTT:
	case APIC_TMICT:
	case APIC_TMCCT:
	case APIC_TDCR:
		return savic_ghcb_msr_read(reg);
	case APIC_ID:
	case APIC_LVR:
	case APIC_TASKPRI:
	case APIC_ARBPRI:
	case APIC_PROCPRI:
	case APIC_LDR:
	case APIC_SPIV:
	case APIC_ESR:
	case APIC_ICR:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVT0:
	case APIC_LVT1:
	case APIC_LVTERR:
	case APIC_EFEAT:
	case APIC_ECTRL:
	case APIC_SEOI:
	case APIC_IER:
	case APIC_EILVTn(0) ... APIC_EILVTn(3):
		return get_reg(backing_page, reg);
	case APIC_ISR ... APIC_ISR + 0x70:
	case APIC_TMR ... APIC_TMR + 0x70:
		if (WARN_ONCE(!IS_ALIGNED(reg, 16),
			      "Reg offset 0x%x not aligned at 16 bytes", reg))
			return 0;
		return get_reg(backing_page, reg);
	/* IRR and ALLOWED_IRR offset range */
	case APIC_IRR ... APIC_IRR + 0x74:
		/*
		 * Either aligned at 16 bytes for valid IRR reg offset or a
		 * valid Secure AVIC ALLOWED_IRR offset.
		 */
		if (WARN_ONCE(!(IS_ALIGNED(reg, 16) ||
				IS_ALIGNED(reg - SAVIC_ALLOWED_IRR_OFFSET, 16)),
			      "Misaligned IRR/ALLOWED_IRR reg offset 0x%x", reg))
			return 0;
		return get_reg(backing_page, reg);
	default:
		pr_err("Permission denied: read of Secure AVIC reg offset 0x%x\n", reg);
		return 0;
	}
}

#define SAVIC_NMI_REQ_OFFSET		0x278

static void x2apic_savic_write(u32 reg, u32 data)
{
	void *backing_page = this_cpu_read(apic_backing_page);
	unsigned int cfg;

	switch (reg) {
	case APIC_LVTT:
	case APIC_TMICT:
	case APIC_TDCR:
		savic_ghcb_msr_write(reg, data);
		break;
	case APIC_LVT0:
	case APIC_LVT1:
	case APIC_TASKPRI:
	case APIC_EOI:
	case APIC_SPIV:
	case SAVIC_NMI_REQ_OFFSET:
	case APIC_ESR:
	case APIC_ICR:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVTERR:
	case APIC_ECTRL:
	case APIC_SEOI:
	case APIC_IER:
	case APIC_EILVTn(0) ... APIC_EILVTn(3):
		set_reg(backing_page, reg, data);
		break;
	/* Self IPIs are accelerated by hardware, use wrmsr */
	case APIC_SELF_IPI:
		cfg = __prepare_ICR(APIC_DEST_SELF, data, 0);
		native_x2apic_icr_write(cfg, 0);
		break;
	/* ALLOWED_IRR offsets are writable */
	case SAVIC_ALLOWED_IRR_OFFSET ... SAVIC_ALLOWED_IRR_OFFSET + 0x70:
		if (IS_ALIGNED(reg - SAVIC_ALLOWED_IRR_OFFSET, 16)) {
			set_reg(backing_page, reg, data);
			break;
		}
		fallthrough;
	default:
		pr_err("Permission denied: write to Secure AVIC reg offset 0x%x\n", reg);
	}
}

static void send_ipi(int cpu, int vector)
{
	void *backing_page;
	int reg_off;

	backing_page = per_cpu(apic_backing_page, cpu);
	reg_off = APIC_IRR + REG_POS(vector);
	/*
	 * Use test_and_set_bit() to ensure that IRR updates are atomic w.r.t. other
	 * IRR updates such as during VMRUN and during CPU interrupt handling flow.
	 */
	test_and_set_bit(VEC_POS(vector), (unsigned long *)((char *)backing_page + reg_off));
}

static void send_ipi_dest(u64 icr_data)
{
	int vector, cpu;

	vector = icr_data & APIC_VECTOR_MASK;
	cpu = icr_data >> 32;

	send_ipi(cpu, vector);
}

static void send_ipi_target(u64 icr_data)
{
	if (icr_data & APIC_DEST_LOGICAL) {
		pr_err("IPI target should be of PHYSICAL type\n");
		return;
	}

	send_ipi_dest(icr_data);
}

static void send_ipi_allbut(u64 icr_data)
{
	const struct cpumask *self_cpu_mask = get_cpu_mask(smp_processor_id());
	unsigned long flags;
	int vector, cpu;

	vector = icr_data & APIC_VECTOR_MASK;
	local_irq_save(flags);
	for_each_cpu_andnot(cpu, cpu_present_mask, self_cpu_mask)
		send_ipi(cpu, vector);
	savic_ghcb_msr_write(APIC_ICR, icr_data);
	local_irq_restore(flags);
}

static void send_ipi_allinc(u64 icr_data)
{
	int vector;

	send_ipi_allbut(icr_data);
	vector = icr_data & APIC_VECTOR_MASK;
	native_x2apic_icr_write(APIC_DEST_SELF | vector, 0);
}

static void x2apic_savic_icr_write(u32 icr_low, u32 icr_high)
{
	int dsh, vector;
	u64 icr_data;

	icr_data = ((u64)icr_high) << 32 | icr_low;
	dsh = icr_low & APIC_DEST_ALLBUT;

	switch (dsh) {
	case APIC_DEST_SELF:
		vector = icr_data & APIC_VECTOR_MASK;
		x2apic_savic_write(APIC_SELF_IPI, vector);
		break;
	case APIC_DEST_ALLINC:
		send_ipi_allinc(icr_data);
		break;
	case APIC_DEST_ALLBUT:
		send_ipi_allbut(icr_data);
		break;
	default:
		send_ipi_target(icr_data);
		savic_ghcb_msr_write(APIC_ICR, icr_data);
	}
}

static void __send_IPI_dest(unsigned int apicid, int vector, unsigned int dest)
{
	unsigned int cfg = __prepare_ICR(0, vector, dest);

	x2apic_savic_icr_write(cfg, apicid);
}

static void x2apic_savic_send_IPI(int cpu, int vector)
{
	u32 dest = per_cpu(x86_cpu_to_apicid, cpu);

	__send_IPI_dest(dest, vector, APIC_DEST_PHYSICAL);
}

static void
__send_IPI_mask(const struct cpumask *mask, int vector, int apic_dest)
{
	unsigned long query_cpu;
	unsigned long this_cpu;
	unsigned long flags;

	local_irq_save(flags);

	this_cpu = smp_processor_id();
	for_each_cpu(query_cpu, mask) {
		if (apic_dest == APIC_DEST_ALLBUT && this_cpu == query_cpu)
			continue;
		__send_IPI_dest(per_cpu(x86_cpu_to_apicid, query_cpu), vector,
				      APIC_DEST_PHYSICAL);
	}

	local_irq_restore(flags);
}

static void x2apic_savic_send_IPI_mask(const struct cpumask *mask, int vector)
{
	__send_IPI_mask(mask, vector, APIC_DEST_ALLINC);
}

static void x2apic_savic_send_IPI_mask_allbutself(const struct cpumask *mask, int vector)
{
	__send_IPI_mask(mask, vector, APIC_DEST_ALLBUT);
}

static void __send_IPI_shorthand(int vector, u32 which)
{
	unsigned int cfg = __prepare_ICR(which, vector, 0);

	x2apic_savic_icr_write(cfg, 0);
}

static void x2apic_savic_send_IPI_allbutself(int vector)
{
	__send_IPI_shorthand(vector, APIC_DEST_ALLBUT);
}

static void x2apic_savic_send_IPI_all(int vector)
{
	__send_IPI_shorthand(vector, APIC_DEST_ALLINC);
}

static void x2apic_savic_send_IPI_self(int vector)
{
	__send_IPI_shorthand(vector, APIC_DEST_SELF);
}

static void x2apic_savic_update_vector(unsigned int cpu, unsigned int vector, bool set)
{
	void *backing_page;
	unsigned long *reg;
	int reg_off;

	backing_page = per_cpu(apic_backing_page, cpu);
	reg_off = SAVIC_ALLOWED_IRR_OFFSET + REG_POS(vector);
	reg = (unsigned long *)((char *)backing_page + reg_off);

	if (set)
		test_and_set_bit(VEC_POS(vector), reg);
	else
		test_and_clear_bit(VEC_POS(vector), reg);
}

static void init_backing_page(void *backing_page)
{
	struct apic_id_node *next_node, *this_cpu_node;
	unsigned int apic_map_slot;
	u32 apic_id;
	int cpu;

	/*
	 * Before Secure AVIC is enabled, APIC msr reads are
	 * intercepted. APIC_ID msr read returns the value
	 * from hv.
	 */
	apic_id = native_apic_msr_read(APIC_ID);
	set_reg(backing_page, APIC_ID, apic_id);

	if (!apic_id_map)
		return;

	cpu = smp_processor_id();
	this_cpu_node = &per_cpu(apic_id_node, cpu);
	this_cpu_node->apic_id = apic_id;
	this_cpu_node->cpu = cpu;
	/*
	 * In common case, apic_ids for CPUs are sequentially numbered.
	 * So, each CPU should hash to a different slot in the apic id
	 * map.
	 */
	apic_map_slot = apic_id % nr_cpu_ids;
	llist_add(&this_cpu_node->node, &apic_id_map[apic_map_slot]);
	/* Each CPU checks only its next nodes for duplicates. */
	llist_for_each_entry(next_node, this_cpu_node->node.next, node) {
		if (WARN_ONCE(next_node->apic_id == apic_id,
			      "Duplicate APIC %u for cpu %d and cpu %d. IPI handling will suffer!",
			      apic_id, cpu, next_node->cpu))
			break;
	}
}

static void x2apic_savic_setup(void)
{
	void *backing_page;
	enum es_result ret;
	unsigned long gpa;

	if (this_cpu_read(apic_backing_page))
		return;

	backing_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!backing_page)
		snp_abort();
	this_cpu_write(apic_backing_page, backing_page);
	init_backing_page(backing_page);
	gpa = __pa(backing_page);

	/*
	 * The NPT entry for the vCPU's APIC backing page must always be
	 * present when the vCPU is running in order for Secure AVIC to
	 * function. A VMEXIT_BUSY is returned on VMRUN and the vCPU cannot
	 * be resumed if the NPT entry for the APIC backing page is not
	 * present. Notify GPA of the vCPU's APIC backing page to the
	 * hypervisor by calling savic_register_gpa(). Before executing
	 * VMRUN, the hypervisor makes use of this information to make sure
	 * the APIC backing page is mapped in NPT.
	 */
	ret = savic_register_gpa(-1ULL, gpa);
	if (ret != ES_OK)
		snp_abort();
}

static int x2apic_savic_probe(void)
{
	int i;

	if (!cc_platform_has(CC_ATTR_SNP_SECURE_AVIC))
		return 0;

	if (!x2apic_mode) {
		pr_err("Secure AVIC enabled in non x2APIC mode\n");
		snp_abort();
	}

	apic_id_map = kvmalloc(nr_cpu_ids * sizeof(*apic_id_map), GFP_KERNEL);

	if (apic_id_map)
		for (i = 0; i < nr_cpu_ids; i++)
			init_llist_head(&apic_id_map[i]);

	pr_info("Secure AVIC Enabled\n");

	return 1;
}

static struct apic apic_x2apic_savic __ro_after_init = {

	.name				= "secure avic x2apic",
	.probe				= x2apic_savic_probe,
	.acpi_madt_oem_check		= x2apic_savic_acpi_madt_oem_check,
	.setup				= x2apic_savic_setup,

	.dest_mode_logical		= false,

	.disable_esr			= 0,

	.cpu_present_to_apicid		= default_cpu_present_to_apicid,

	.max_apic_id			= UINT_MAX,
	.x2apic_set_max_apicid		= true,
	.get_apic_id			= x2apic_get_apic_id,

	.calc_dest_apicid		= apic_default_calc_apicid,

	.send_IPI			= x2apic_savic_send_IPI,
	.send_IPI_mask			= x2apic_savic_send_IPI_mask,
	.send_IPI_mask_allbutself	= x2apic_savic_send_IPI_mask_allbutself,
	.send_IPI_allbutself		= x2apic_savic_send_IPI_allbutself,
	.send_IPI_all			= x2apic_savic_send_IPI_all,
	.send_IPI_self			= x2apic_savic_send_IPI_self,
	.nmi_to_offline_cpu		= true,

	.read				= x2apic_savic_read,
	.write				= x2apic_savic_write,
	.eoi				= native_apic_msr_eoi,
	.icr_read			= native_x2apic_icr_read,
	.icr_write			= x2apic_savic_icr_write,

	.update_vector			= x2apic_savic_update_vector,
};

apic_driver(apic_x2apic_savic);
