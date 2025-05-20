/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RAR TLB shootdown
 */
#include <linux/sched.h>
#include <linux/bug.h>
#include <asm/current.h>
#include <asm/io.h>
#include <asm/sync_bitops.h>
#include <asm/rar.h>
#include <asm/tlbflush.h>

static DEFINE_PER_CPU(struct cpumask, rar_cpu_mask);

#define RAR_ACTION_OK		0x00
#define RAR_ACTION_START	0x01
#define RAR_ACTION_ACKED	0x02
#define RAR_ACTION_FAIL		0x80

#define RAR_MAX_PAYLOADS 32UL

static unsigned long rar_in_use = ~(RAR_MAX_PAYLOADS - 1);
static struct rar_payload rar_payload[RAR_MAX_PAYLOADS] __page_aligned_bss;
static DEFINE_PER_CPU_ALIGNED(u8[RAR_MAX_PAYLOADS], rar_action);

static unsigned long get_payload(void)
{
	while (1) {
		unsigned long bit;

		/*
		 * Find a free bit and confirm it with
		 * test_and_set_bit() below.
		 */
		bit = ffz(READ_ONCE(rar_in_use));

		if (bit >= RAR_MAX_PAYLOADS)
			continue;

		if (!test_and_set_bit((long)bit, &rar_in_use))
			return bit;
	}
}

static void free_payload(unsigned long idx)
{
	clear_bit(idx, &rar_in_use);
}

static void set_payload(unsigned long idx, u16 pcid, unsigned long start,
			uint32_t pages)
{
	struct rar_payload *p = &rar_payload[idx];

	p->must_be_zero_1	= 0;
	p->must_be_zero_2	= 0;
	p->must_be_zero_3	= 0;
	p->page_size		= RAR_INVLPG_PAGE_SIZE_4K;
	p->type			= RAR_TYPE_INVPCID;
	p->num_pages		= pages;
	p->initiator_cr3	= pcid;
	p->linear_address	= start;

	if (pcid) {
		/* RAR invalidation of the mapping of a specific process. */
		if (pages >= RAR_INVLPG_MAX_PAGES)
			p->subtype = RAR_INVPCID_PCID;
		else
			p->subtype = RAR_INVPCID_ADDR;
	} else {
		/*
		 * Unfortunately RAR_INVPCID_ADDR excludes global translations.
		 * Always do a full flush for kernel invalidations.
		 */
		p->subtype = RAR_INVPCID_ALL;
	}

	smp_wmb();
}

static void set_action_entry(unsigned long idx, int target_cpu)
{
	u8 *bitmap = per_cpu(rar_action, target_cpu);

	WRITE_ONCE(bitmap[idx], RAR_ACTION_START);
}

static void wait_for_done(unsigned long idx, int target_cpu)
{
	u8 status;
	u8 *rar_actions = per_cpu(rar_action, target_cpu);

	status = READ_ONCE(rar_actions[idx]);

	while ((status != RAR_ACTION_OK) && (status != RAR_ACTION_FAIL)) {
		cpu_relax();
		status = READ_ONCE(rar_actions[idx]);
	}

	WARN_ON_ONCE(rar_actions[idx] == RAR_ACTION_FAIL);
}

void rar_cpu_init(void)
{
	u64 r;
	u8 *bitmap;
	int this_cpu = smp_processor_id();

	cpumask_clear(&per_cpu(rar_cpu_mask, this_cpu));

	rdmsrl(MSR_IA32_RAR_INFO, r);
	pr_info_once("RAR: support %lld payloads\n", r >> 32);

	bitmap = (u8 *)per_cpu(rar_action, this_cpu);
	memset(bitmap, 0, RAR_MAX_PAYLOADS);
	wrmsrl(MSR_IA32_RAR_ACT_VEC, (u64)virt_to_phys(bitmap));
	wrmsrl(MSR_IA32_RAR_PAYLOAD_BASE, (u64)virt_to_phys(rar_payload));

	r = RAR_CTRL_ENABLE | RAR_CTRL_IGNORE_IF;
	// reserved bits!!! r |= (RAR_VECTOR & 0xff);
	wrmsrl(MSR_IA32_RAR_CTRL, r);
}

/*
 * This is a modified version of smp_call_function_many() of kernel/smp.c,
 * without a function pointer, because the RAR handler is the ucode.
 */
void smp_call_rar_many(const struct cpumask *mask, u16 pcid,
		       unsigned long start, unsigned long end)
{
	unsigned long pages = (end - start + PAGE_SIZE) / PAGE_SIZE;
	int cpu, next_cpu, this_cpu = smp_processor_id();
	cpumask_t *dest_mask;
	unsigned long idx;

	if (pages > RAR_INVLPG_MAX_PAGES || end == TLB_FLUSH_ALL)
		pages = RAR_INVLPG_MAX_PAGES;

	/*
	 * Can deadlock when called with interrupts disabled.
	 * We allow cpu's that are not yet online though, as no one else can
	 * send smp call function interrupt to this cpu and as such deadlocks
	 * can't happen.
	 */
	WARN_ON_ONCE(cpu_online(this_cpu) && irqs_disabled()
		     && !oops_in_progress && !early_boot_irqs_disabled);

	/* Try to fastpath.  So, what's a CPU they want?  Ignoring this one. */
	cpu = cpumask_first_and(mask, cpu_online_mask);
	if (cpu == this_cpu)
		cpu = cpumask_next_and(cpu, mask, cpu_online_mask);

	/* No online cpus?  We're done. */
	if (cpu >= nr_cpu_ids)
		return;

	/* Do we have another CPU which isn't us? */
	next_cpu = cpumask_next_and(cpu, mask, cpu_online_mask);
	if (next_cpu == this_cpu)
		next_cpu = cpumask_next_and(next_cpu, mask, cpu_online_mask);

	/* Fastpath: do that cpu by itself. */
	if (next_cpu >= nr_cpu_ids) {
		idx = get_payload();
		set_payload(idx, pcid, start, pages);
		set_action_entry(idx, cpu);
		arch_send_rar_single_ipi(cpu);
		wait_for_done(idx, cpu);
		free_payload(idx);
		return;
	}

	dest_mask = this_cpu_ptr(&rar_cpu_mask);
	cpumask_and(dest_mask, mask, cpu_online_mask);
	cpumask_clear_cpu(this_cpu, dest_mask);

	/* Some callers race with other cpus changing the passed mask */
	if (unlikely(!cpumask_weight(dest_mask)))
		return;

	idx = get_payload();
	set_payload(idx, pcid, start, pages);

	for_each_cpu(cpu, dest_mask)
		set_action_entry(idx, cpu);

	/* Send a message to all CPUs in the map */
	arch_send_rar_ipi_mask(dest_mask);

	for_each_cpu(cpu, dest_mask)
		wait_for_done(idx, cpu);

	free_payload(idx);
}
EXPORT_SYMBOL(smp_call_rar_many);
