// SPDX-License-Identifier: GPL-2.0-only
/*
 * s2idle.c - ACPI suspend-to-idle support.
 *
 */

#define pr_fmt(fmt) "ACPI: PM: " fmt

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/suspend.h>
#include <linux/irq.h>

#include "internal.h"
#include "sleep.h"

static bool s2idle_wakeup;

int acpi_s2idle_begin(void)
{
	acpi_scan_lock_acquire();
	return 0;
}

int acpi_s2idle_prepare(void)
{
	if (acpi_sci_irq_valid()) {
		int error;

		error = enable_irq_wake(acpi_sci_irq);
		if (error)
			pr_warn("Failed to enable wakeup from IRQ %d: %d\n",
				acpi_sci_irq, error);

		acpi_ec_set_gpe_wake_mask(ACPI_GPE_ENABLE);
	}

	acpi_enable_wakeup_devices(ACPI_STATE_S0);

	/* Change the configuration of GPEs to avoid spurious wakeup. */
	acpi_enable_all_wakeup_gpes();
	acpi_os_wait_events_complete();

	s2idle_wakeup = true;
	return 0;
}

bool acpi_s2idle_wake(void)
{
	if (!acpi_sci_irq_valid())
		return pm_wakeup_pending();

	while (pm_wakeup_pending()) {
		/*
		 * If IRQD_WAKEUP_ARMED is set for the SCI at this point, the
		 * SCI has not triggered while suspended, so bail out (the
		 * wakeup is pending anyway and the SCI is not the source of
		 * it).
		 */
		if (irqd_is_wakeup_armed(irq_get_irq_data(acpi_sci_irq))) {
			pm_pr_dbg("Wakeup unrelated to ACPI SCI\n");
			return true;
		}

		/*
		 * If the status bit of any enabled fixed event is set, the
		 * wakeup is regarded as valid.
		 */
		if (acpi_any_fixed_event_status_set()) {
			pm_pr_dbg("ACPI fixed event wakeup\n");
			return true;
		}

		/* Check wakeups from drivers sharing the SCI. */
		if (acpi_check_wakeup_handlers()) {
			pm_pr_dbg("ACPI custom handler wakeup\n");
			return true;
		}

		/*
		 * Check non-EC GPE wakeups and if there are none, cancel the
		 * SCI-related wakeup and dispatch the EC GPE.
		 */
		if (acpi_ec_dispatch_gpe()) {
			pm_pr_dbg("ACPI non-EC GPE wakeup\n");
			return true;
		}

		acpi_os_wait_events_complete();

		/*
		 * The SCI is in the "suspended" state now and it cannot produce
		 * new wakeup events till the rearming below, so if any of them
		 * are pending here, they must be resulting from the processing
		 * of EC events above or coming from somewhere else.
		 */
		if (pm_wakeup_pending()) {
			pm_pr_dbg("Wakeup after ACPI Notify sync\n");
			return true;
		}

		pm_pr_dbg("Rearming ACPI SCI for wakeup\n");

		pm_wakeup_clear(acpi_sci_irq);
		rearm_wake_irq(acpi_sci_irq);
	}

	return false;
}

void acpi_s2idle_restore(void)
{
	/*
	 * Drain pending events before restoring the working-state configuration
	 * of GPEs.
	 */
	acpi_os_wait_events_complete(); /* synchronize GPE processing */
	acpi_ec_flush_work(); /* flush the EC driver's workqueues */
	acpi_os_wait_events_complete(); /* synchronize Notify handling */

	s2idle_wakeup = false;

	acpi_enable_all_runtime_gpes();

	acpi_disable_wakeup_devices(ACPI_STATE_S0);

	if (acpi_sci_irq_valid()) {
		acpi_ec_set_gpe_wake_mask(ACPI_GPE_DISABLE);
		disable_irq_wake(acpi_sci_irq);
	}
}

void acpi_s2idle_end(void)
{
	acpi_scan_lock_release();
}

static const struct platform_s2idle_ops acpi_s2idle_ops = {
	.begin = acpi_s2idle_begin,
	.prepare = acpi_s2idle_prepare,
	.wake = acpi_s2idle_wake,
	.restore = acpi_s2idle_restore,
	.end = acpi_s2idle_end,
};

void __init __weak acpi_s2idle_setup(void)
{
	if (acpi_gbl_FADT.flags & ACPI_FADT_LOW_POWER_S0)
		pr_info("Efficient low-power S0 idle declared\n");

	s2idle_set_ops(&acpi_s2idle_ops);
}

bool acpi_s2idle_wakeup(void)
{
	return s2idle_wakeup;
}

void __init acpi_s2idle_init(void)
{
	acpi_s2idle_setup();
}
