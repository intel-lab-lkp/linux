#include <linux/acpi.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kexec.h>
#include <linux/memblock.h>
#include <linux/pgtable.h>
#include <linux/sched/hotplug.h>
#include <asm/apic.h>
#include <asm/barrier.h>
#include <asm/init.h>
#include <asm/intel_pt.h>
#include <asm/nmi.h>
#include <asm/processor.h>
#include <asm/reboot.h>

/* Physical address of the Multiprocessor Wakeup Structure mailbox */
static u64 acpi_mp_wake_mailbox_paddr __ro_after_init;

/* Virtual address of the Multiprocessor Wakeup Structure mailbox */
static struct acpi_madt_multiproc_wakeup_mailbox *acpi_mp_wake_mailbox __ro_after_init;

static u64 acpi_mp_pgd __ro_after_init;
static u64 acpi_mp_reset_vector_paddr __ro_after_init;

void asm_acpi_mp_play_dead(u64 reset_vector, u64 pgd_pa);

static void crash_acpi_mp_play_dead(void)
{
	asm_acpi_mp_play_dead(acpi_mp_reset_vector_paddr,
			      acpi_mp_pgd);
}

static void acpi_mp_play_dead(void)
{
	play_dead_common();
	asm_acpi_mp_play_dead(acpi_mp_reset_vector_paddr,
			      acpi_mp_pgd);
}

static void acpi_mp_cpu_die(unsigned int cpu)
{
	u32 apicid = per_cpu(x86_cpu_to_apicid, cpu);
	unsigned long timeout;

	/*
	 * Use TEST mailbox command to prove that BIOS got control over
	 * the CPU before declaring it dead.
	 *
	 * BIOS has to clear 'command' field of the mailbox.
	 */
	acpi_mp_wake_mailbox->apic_id = apicid;
	smp_store_release(&acpi_mp_wake_mailbox->command,
			  ACPI_MP_WAKE_COMMAND_TEST);

	/* Don't wait longer than a second. */
	timeout = USEC_PER_SEC;
	while (READ_ONCE(acpi_mp_wake_mailbox->command) && timeout--)
		udelay(1);
}

static void acpi_mp_stop_other_cpus(int wait)
{
	smp_shutdown_nonboot_cpus(smp_processor_id());
}

/* The argument is required to match type of x86_mapping_info::alloc_pgt_page */
static void __init *alloc_pgt_page(void *dummy)
{
	return memblock_alloc(PAGE_SIZE, PAGE_SIZE);
}

/*
 * Make sure asm_acpi_mp_play_dead() is present in the identity mapping at
 * the same place as in the kernel page tables. asm_acpi_mp_play_dead() switches
 * to the identity mapping and the function has be present at the same spot in
 * the virtual address space before and after switching page tables.
 */
static int __init init_transition_pgtable(pgd_t *pgd)
{
	pgprot_t prot = PAGE_KERNEL_EXEC_NOENC;
	unsigned long vaddr, paddr;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	vaddr = (unsigned long)asm_acpi_mp_play_dead;
	pgd += pgd_index(vaddr);
	if (!pgd_present(*pgd)) {
		p4d = (p4d_t *)alloc_pgt_page(NULL);
		if (!p4d)
			return -ENOMEM;
		set_pgd(pgd, __pgd(__pa(p4d) | _KERNPG_TABLE));
	}
	p4d = p4d_offset(pgd, vaddr);
	if (!p4d_present(*p4d)) {
		pud = (pud_t *)alloc_pgt_page(NULL);
		if (!pud)
			return -ENOMEM;
		set_p4d(p4d, __p4d(__pa(pud) | _KERNPG_TABLE));
	}
	pud = pud_offset(p4d, vaddr);
	if (!pud_present(*pud)) {
		pmd = (pmd_t *)alloc_pgt_page(NULL);
		if (!pmd)
			return -ENOMEM;
		set_pud(pud, __pud(__pa(pmd) | _KERNPG_TABLE));
	}
	pmd = pmd_offset(pud, vaddr);
	if (!pmd_present(*pmd)) {
		pte = (pte_t *)alloc_pgt_page(NULL);
		if (!pte)
			return -ENOMEM;
		set_pmd(pmd, __pmd(__pa(pte) | _KERNPG_TABLE));
	}
	pte = pte_offset_kernel(pmd, vaddr);

	paddr = __pa(vaddr);
	set_pte(pte, pfn_pte(paddr >> PAGE_SHIFT, prot));

	return 0;
}

static void __init free_pte(pmd_t *pmd)
{
	pte_t *pte = pte_offset_kernel(pmd, 0);

	memblock_free(pte, PAGE_SIZE);
}

static void __init free_pmd(pud_t *pud)
{
	pmd_t *pmd = pmd_offset(pud, 0);
	int i;

	for (i = 0; i < PTRS_PER_PMD; i++) {
		if (!pmd_present(pmd[i]))
		    continue;

		if (pmd_leaf(pmd[i]))
		    continue;

		free_pte(&pmd[i]);
	}

	memblock_free(pmd, PAGE_SIZE);
}

static void __init free_pud(p4d_t *p4d)
{
	pud_t *pud = pud_offset(p4d, 0);
	int i;

	for (i = 0; i < PTRS_PER_PUD; i++) {
		if (!pud_present(pud[i]))
			continue;

		if (pud_leaf(pud[i]))
		    continue;

		free_pmd(&pud[i]);
	}

	memblock_free(pud, PAGE_SIZE);
}

static void __init free_p4d(pgd_t *pgd)
{
	p4d_t *p4d = p4d_offset(pgd, 0);
	int i;

	for (i = 0; i < PTRS_PER_P4D; i++) {
		if (!p4d_present(p4d[i]))
			continue;

		free_pud(&p4d[i]);
	}

	if (pgtable_l5_enabled())
		memblock_free(p4d, PAGE_SIZE);
}

static void __init free_pgd(pgd_t *pgd)
{
	int i;

	for (i = 0; i < PTRS_PER_PGD; i++) {
		if (!pgd_present(pgd[i]))
			continue;

		free_p4d(&pgd[i]);
	}

	memblock_free(pgd, PAGE_SIZE);
}

static int __init acpi_mp_setup_reset(u64 reset_vector)
{
	pgd_t *pgd;
	struct x86_mapping_info info = {
		.alloc_pgt_page = alloc_pgt_page,
		.page_flag      = __PAGE_KERNEL_LARGE_EXEC,
		.kernpg_flag    = _KERNPG_TABLE_NOENC,
	};

	pgd = alloc_pgt_page(NULL);
	if (!pgd)
		return -ENOMEM;

	for (int i = 0; i < nr_pfn_mapped; i++) {
		unsigned long mstart, mend;

		mstart = pfn_mapped[i].start << PAGE_SHIFT;
		mend   = pfn_mapped[i].end << PAGE_SHIFT;
		if (kernel_ident_mapping_init(&info, pgd, mstart, mend)) {
			free_pgd(pgd);
			return -ENOMEM;
		}
	}

	if (kernel_ident_mapping_init(&info, pgd,
				      PAGE_ALIGN_DOWN(reset_vector),
				      PAGE_ALIGN(reset_vector + 1))) {
		free_pgd(pgd);
		return -ENOMEM;
	}

	if (init_transition_pgtable(pgd)) {
		free_pgd(pgd);
		return -ENOMEM;
	}

	smp_ops.play_dead = acpi_mp_play_dead;
	smp_ops.crash_play_dead = crash_acpi_mp_play_dead;
	smp_ops.cpu_die = acpi_mp_cpu_die;
	smp_ops.stop_other_cpus = acpi_mp_stop_other_cpus;

	acpi_mp_reset_vector_paddr = reset_vector;
	acpi_mp_pgd = __pa(pgd);

	return 0;
}

static int acpi_wakeup_cpu(u32 apicid, unsigned long start_ip)
{
	if (!acpi_mp_wake_mailbox_paddr) {
		pr_warn_once("No MADT mailbox: cannot bringup secondary CPUs. Booting with kexec?\n");
		return -EOPNOTSUPP;
	}

	/*
	 * Remap mailbox memory only for the first call to acpi_wakeup_cpu().
	 *
	 * Wakeup of secondary CPUs is fully serialized in the core code.
	 * No need to protect acpi_mp_wake_mailbox from concurrent accesses.
	 */
	if (!acpi_mp_wake_mailbox) {
		acpi_mp_wake_mailbox = memremap(acpi_mp_wake_mailbox_paddr,
						sizeof(*acpi_mp_wake_mailbox),
						MEMREMAP_WB);
	}

	/*
	 * Mailbox memory is shared between the firmware and OS. Firmware will
	 * listen on mailbox command address, and once it receives the wakeup
	 * command, the CPU associated with the given apicid will be booted.
	 *
	 * The value of 'apic_id' and 'wakeup_vector' must be visible to the
	 * firmware before the wakeup command is visible.  smp_store_release()
	 * ensures ordering and visibility.
	 */
	acpi_mp_wake_mailbox->apic_id	    = apicid;
	acpi_mp_wake_mailbox->wakeup_vector = start_ip;
	smp_store_release(&acpi_mp_wake_mailbox->command,
			  ACPI_MP_WAKE_COMMAND_WAKEUP);

	/*
	 * Wait for the CPU to wake up.
	 *
	 * The CPU being woken up is essentially in a spin loop waiting to be
	 * woken up. It should not take long for it wake up and acknowledge by
	 * zeroing out ->command.
	 *
	 * ACPI specification doesn't provide any guidance on how long kernel
	 * has to wait for a wake up acknowledgement. It also doesn't provide
	 * a way to cancel a wake up request if it takes too long.
	 *
	 * In TDX environment, the VMM has control over how long it takes to
	 * wake up secondary. It can postpone scheduling secondary vCPU
	 * indefinitely. Giving up on wake up request and reporting error opens
	 * possible attack vector for VMM: it can wake up a secondary CPU when
	 * kernel doesn't expect it. Wait until positive result of the wake up
	 * request.
	 */
	while (READ_ONCE(acpi_mp_wake_mailbox->command))
		cpu_relax();

	return 0;
}

static void acpi_mp_disable_offlining(struct acpi_madt_multiproc_wakeup *mp_wake)
{
	cpu_hotplug_disable_offlining();

	/*
	 * Zero out mailbox address in the ACPI MADT wakeup structure
	 * to indicate that the mailbox is not usable.  This prevents
	 * the kexec()-ed kernel from reading a vaild mailbox, which in
	 * turn makes the kexec()-ed kernel only be able to use the boot
	 * CPU.
	 *
	 * This is Linux-specific protocol and not reflected in ACPI spec.
	 *
	 * acpi_mp_wake_mailbox_paddr already has the mailbox address.
	 * The acpi_wakeup_cpu() will use it to bring up secondary cpus for
	 * the current kernel.
	 */
	mp_wake->mailbox_address = 0;
}

int __init acpi_parse_mp_wake(union acpi_subtable_headers *header,
			      const unsigned long end)
{
	struct acpi_madt_multiproc_wakeup *mp_wake;

	mp_wake = (struct acpi_madt_multiproc_wakeup *)header;

        /*
         * Cannot use the standard BAD_MADT_ENTRY() to sanity check the @mp_wake
         * entry.  'sizeof (struct acpi_madt_multiproc_wakeup)' can be larger
         * than the actual size of the MP wakeup entry in ACPI table because the
	 * 'reset_vector' is only available in the V1 MP wakeup structure.
         */
	if (!mp_wake)
		return -EINVAL;
	if (end - (unsigned long)mp_wake < ACPI_MADT_MP_WAKEUP_SIZE_V0)
		return -EINVAL;
	if (mp_wake->header.length < ACPI_MADT_MP_WAKEUP_SIZE_V0)
		return -EINVAL;

	acpi_table_print_madt_entry(&header->common);

	acpi_mp_wake_mailbox_paddr = mp_wake->mailbox_address;

	if (mp_wake->version >= ACPI_MADT_MP_WAKEUP_VERSION_V1 &&
	    mp_wake->header.length >= ACPI_MADT_MP_WAKEUP_SIZE_V1) {
		if (acpi_mp_setup_reset(mp_wake->reset_vector)) {
			pr_warn("Failed to setup MADT reset vector\n");
			acpi_mp_disable_offlining(mp_wake);
		}
	} else {
		/*
		 * CPU offlining requires version 1 of the ACPI MADT wakeup
		 * structure.
		 */
		acpi_mp_disable_offlining(mp_wake);
	}

	apic_update_callback(wakeup_secondary_cpu_64, acpi_wakeup_cpu);

	return 0;
}
