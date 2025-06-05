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

#define RAR_ACTION_SUCCESS	0x00
#define RAR_ACTION_PENDING	0x01
#define RAR_ACTION_FAILURE	0x80

#define RAR_MAX_PAYLOADS 64UL

/* How many RAR payloads are supported by this CPU */
static int rar_max_payloads = RAR_MAX_PAYLOADS;

/* Bitmap describing which RAR payload slots are in use. */
static unsigned long rar_in_use = ~(RAR_MAX_PAYLOADS - 1);

/*
 * RAR payloads telling CPUs what to do. This table is shared between
 * all CPUs; it is possible to have multiple payload tables shared between
 * different subsets of CPUs, but that adds a lot of complexity.
 */
static struct rar_payload rar_payload[RAR_MAX_PAYLOADS] __page_aligned_bss;

/*
 * The action vector tells each CPU which payload table entries
 * have work for that CPU.
 */
static DEFINE_PER_CPU_ALIGNED(u8[RAR_MAX_PAYLOADS], rar_action);

static unsigned long get_payload_slot(void)
{
	while (1) {
		unsigned long bit;

		/*
		 * Find a free bit and confirm it with test_and_set_bit()
		 * below. If no slot is free, spin until one becomes free.
		 */
		bit = ffz(READ_ONCE(rar_in_use));

		if (bit >= rar_max_payloads)
			continue;

		if (!test_and_set_bit((long)bit, &rar_in_use))
			return bit;
	}
}

static void free_payload_slot(unsigned long payload_nr)
{
	clear_bit(payload_nr, &rar_in_use);
}

static void set_payload(struct rar_payload *p, u16 pcid, unsigned long start,
			uint32_t pages)
{
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

	/* Ensure all writes are visible before the action entry is set. */
	smp_wmb();
}

static void set_action_entry(unsigned long payload_nr, int target_cpu)
{
	u8 *bitmap = per_cpu(rar_action, target_cpu);

	/*
	 * Given a remote CPU, "arm" its action vector to ensure it handles
	 * the request at payload_nr when it receives a RAR signal.
	 * The remote CPU will overwrite RAR_ACTION_PENDING when it handles
	 * the request.
	 */
	WRITE_ONCE(bitmap[payload_nr], RAR_ACTION_PENDING);
}

static void wait_for_action_done(unsigned long payload_nr, int target_cpu)
{
	u8 status;
	u8 *rar_actions = per_cpu(rar_action, target_cpu);

	status = READ_ONCE(rar_actions[payload_nr]);

	while (status == RAR_ACTION_PENDING) {
		cpu_relax();
		status = READ_ONCE(rar_actions[payload_nr]);
	}

	WARN_ON_ONCE(rar_actions[payload_nr] != RAR_ACTION_SUCCESS);
}

void rar_cpu_init(void)
{
	u64 r;
	u8 *bitmap;
	int max_payloads;
	int this_cpu = smp_processor_id();

	cpumask_clear(&per_cpu(rar_cpu_mask, this_cpu));

	/* The MSR contains N defining the max [0-N] rar payload slots. */
	rdmsrl(MSR_IA32_RAR_INFO, r);
	max_payloads = (r >> 32) + 1;

	/* If this CPU supports less than RAR_MAX_PAYLOADS, lower our limit. */
	if (max_payloads < rar_max_payloads)
		rar_max_payloads = max_payloads;
	pr_info_once("RAR: support %d payloads\n", max_payloads);

	bitmap = (u8 *)per_cpu(rar_action, this_cpu);
	memset(bitmap, 0, RAR_MAX_PAYLOADS);
	wrmsrl(MSR_IA32_RAR_ACT_VEC, (u64)virt_to_phys(bitmap));
	wrmsrl(MSR_IA32_RAR_PAYLOAD_BASE, (u64)virt_to_phys(rar_payload));

	/*
	 * Allow RAR events to be processed while interrupts are disabled on
	 * a target CPU. This prevents "pileups" where many CPUs are waiting
	 * on one CPU that has IRQs blocked for too long, and should reduce
	 * contention on the rar_payload table.
	 */
	r = RAR_CTRL_ENABLE | RAR_CTRL_IGNORE_IF;
	wrmsrl(MSR_IA32_RAR_CTRL, r);
}

/*
 * Inspired by smp_call_function_many(), but RAR requires a global payload
 * table rather than per-CPU payloads in the CSD table, because the action
 * handler is microcode rather than software.
 */
void smp_call_rar_many(const struct cpumask *mask, u16 pcid,
		       unsigned long start, unsigned long end)
{
	unsigned long pages = (end - start + PAGE_SIZE) / PAGE_SIZE;
	int cpu, this_cpu = smp_processor_id();
	cpumask_t *dest_mask;
	unsigned long payload_nr;

	if (pages > RAR_INVLPG_MAX_PAGES || end == TLB_FLUSH_ALL)
		pages = RAR_INVLPG_MAX_PAGES;

	/*
	 * Can deadlock when called with interrupts disabled.
	 * Allow CPUs that are not yet online though, as no one else can
	 * send smp call function interrupt to this CPU and as such deadlocks
	 * can't happen.
	 */
	if (cpu_online(this_cpu) && !oops_in_progress && !early_boot_irqs_disabled) {
		lockdep_assert_irqs_enabled();
		lockdep_assert_preemption_disabled();
	}

	/*
	 * A CPU needs to be initialized in order to process RARs.
	 * Skip offline CPUs.
	 *
	 * TODO:
	 * - Use RAR to flush our own TLB so it can all happen in parallel
	 *   (need to resolve a chicken-egg issue with the boot CPU)
	 * - Skip RAR to CPUs that are in a deeper C-state, with an empty TLB
	 *
	 * This code cannot use the should_flush_tlb() logic here because
	 * RAR flushes do not update the tlb_gen, resulting in unnecessary
	 * flushes at context switch time.
	 */
	dest_mask = this_cpu_ptr(&rar_cpu_mask);
	cpumask_and(dest_mask, mask, cpu_online_mask);
	__cpumask_clear_cpu(this_cpu, dest_mask);

	/* Some callers race with other CPUs changing the passed mask */
	if (unlikely(!cpumask_weight(dest_mask)))
		return;

	payload_nr = get_payload_slot();
	set_payload(&rar_payload[payload_nr], pcid, start, pages);

	for_each_cpu(cpu, dest_mask)
		set_action_entry(payload_nr, cpu);

	/* Send a message to all CPUs in the map */
	native_send_rar_ipi(dest_mask);

	for_each_cpu(cpu, dest_mask)
		wait_for_action_done(payload_nr, cpu);

	free_payload_slot(payload_nr);
}
EXPORT_SYMBOL(smp_call_rar_many);
