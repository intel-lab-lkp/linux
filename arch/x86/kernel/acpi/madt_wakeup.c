#include <linux/acpi.h>
#include <linux/cpu.h>
#include <linux/io.h>
#include <asm/apic.h>
#include <asm/barrier.h>
#include <asm/processor.h>

/* Physical address of the Multiprocessor Wakeup Structure mailbox */
static u64 acpi_mp_wake_mailbox_paddr __ro_after_init;

/* Virtual address of the Multiprocessor Wakeup Structure mailbox */
static struct acpi_madt_multiproc_wakeup_mailbox *acpi_mp_wake_mailbox __ro_after_init;

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

int __init acpi_parse_mp_wake(union acpi_subtable_headers *header,
			      const unsigned long end)
{
	struct acpi_madt_multiproc_wakeup *mp_wake;

	mp_wake = (struct acpi_madt_multiproc_wakeup *)header;
	if (BAD_MADT_ENTRY(mp_wake, end))
		return -EINVAL;

	acpi_table_print_madt_entry(&header->common);

	acpi_mp_wake_mailbox_paddr = mp_wake->mailbox_address;

	cpu_hotplug_disable_offlining();

	/*
	 * ACPI MADT doesn't allow to offline CPU after it got woke up.
	 * It limits kexec: the second kernel won't be able to use more than
	 * one CPU.
	 *
	 * Now acpi_mp_wake_mailbox_paddr already has the mailbox address.
	 * The acpi_wakeup_cpu() will use it to bring up secondary cpus.
	 *
	 * Zero out mailbox address in the ACPI MADT wakeup structure to
	 * indicate that the mailbox is not usable.  This prevents the
	 * kexec()-ed kernel from reading a vaild mailbox, which in turn
	 * makes the kexec()-ed kernel only be able to use the boot CPU.
	 *
	 * This is Linux-specific protocol and not reflected in ACPI spec.
	 */
	mp_wake->mailbox_address = 0;

	apic_update_callback(wakeup_secondary_cpu_64, acpi_wakeup_cpu);

	return 0;
}
