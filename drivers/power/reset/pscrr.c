// SPDX-License-Identifier: GPL-2.0
/*
 * pscrr_core.c - Core Power State Change Reason Recording
 *
 * This framework provides a method for recording the cause of the last system
 * reboot, particularly in scenarios where **hardware protection events** (e.g.,
 * undervoltage, overcurrent, thermal shutdown) force an immediate reset. Unlike
 * traditional logging mechanisms that rely on block storage (e.g., NAND, eMMC),
 * PSCRR ensures shutdown reasons are preserved in a way that survives power
 * loss for later analysis.
 *
 * Purpose:
 * --------
 * The primary goal of PSCRR is to help developers and system operators analyze
 * real-world failures by identifying what conditions embedded devices
 * experience in the field. By persisting power state change reasons across
 * reboots, engineers can gain insight into why and how systems fail, enabling
 * better debugging and long-term system improvements.
 *
 * At the time of developing this framework, no specific recovery strategies
 * were designed. Instead, the focus is on reliable event recording to support
 * future diagnostic and recovery efforts.
 *
 * Sysfs Interface:
 * ----------------
 *    /sys/kernel/pscrr/reason       - Read/write current power state change
 *				       reason
 *    /sys/kernel/pscrr/reason_boot  - Read-only last recorded reason from
 *				       previous boot
 *
 * Why is this needed?
 * --------------------
 * Many embedded systems experience power-related faults where **safe shutdown
 * of block storage (e.g., NAND, eMMC) is not possible**:
 *   - Undervoltage protection triggers a hard shutdown before data can be
 *     written.
 *   - eMMC/NAND cannot be safely updated during power failure.
 *
 * To ensure post-mortem analysis is possible, alternate non-volatile storage
 * should be used, such as:
 *   - Battery-backed RTC scratchpad
 *   - EEPROM or small NVMEM regions
 *   - FRAM or other fast, low-power persistent memory
 *
 * How PSCRR Works:
 * ----------------
 *   - A driver detects a problem (e.g., overtemperature) and calls:
 *       set_power_state_change_reason(PSCR_OVERTEMPERATURE).
 *   - Before reboot, PSCRR writes the reason to hardware storage
 *     via the backend's `.write_reason()` callback.
 *   - On the next boot, the stored reason is retrieved from persistent storage
 *     and exposed via `/sys/kernel/pscrr/reason_boot` for analysis.
 *   - Userspace can dynamically set `/sys/kernel/pscrr/reason` to
 *     update the shutdown reason before a reboot.
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/power/power_on_reason.h>
#include <linux/pscrr.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

struct pscrr_data {
	struct notifier_block reboot_nb;
	const struct pscrr_backend_ops *ops;

	enum pscr_reason last_reason;
	enum pscr_reason last_boot_reason;

	/* Kobject for sysfs */
	struct kobject *kobj;
};

static struct pscrr_data *g_pscrr;

/**
 * pscrr_reason_to_str - Converts a power state change reason enum to a string.
 * @reason: The `pscr_reason` enum value to be converted.
 *
 * This function provides a human-readable string representation of the power
 * state change reason, making it easier to interpret logs and debug messages.
 *
 * Return:
 * - A string corresponding to the given `pscr_reason` value.
 * - `"Invalid"` if the value is not recognized.
 */
static const char *pscrr_reason_to_str(enum pscr_reason reason)
{
	switch (reason) {
	case PSCR_UNKNOWN:
		return POWER_ON_REASON_UNKNOWN;
	case PSCR_UNDER_VOLTAGE:
		return POWER_ON_REASON_BROWN_OUT;
	case PSCR_OVER_CURRENT:
		return POWER_ON_REASON_OVER_CURRENT;
	case PSCR_REGULATOR_FAILURE:
		return POWER_ON_REASON_REGULATOR_FAILURE;
	case PSCR_OVERTEMPERATURE:
		return POWER_ON_REASON_OVERTEMPERATURE;
	default:
		return "Invalid";
	}
}

/**
 * pscrr_reboot_notifier - Stores the last power state change reason before
 *			   reboot.
 * @nb: Notifier block structure (unused in this function).
 * @action: The type of reboot action (unused in this function).
 * @unused: Unused parameter.
 *
 * This function is called when the system is about to reboot or shut down. It
 * writes the last recorded power state change reason to persistent storage
 * using the registered backend’s write_reason() function.
 *
 * If writing fails, an error message is logged, but the reboot sequence is
 * not blocked. The function always returns `NOTIFY_OK` to ensure that the
 * system can reboot safely even if the reason cannot be stored.
 *
 * Return:
 * - `NOTIFY_OK` on success or failure, allowing reboot to proceed.
 * - `NOTIFY_DONE` if the PSCRR subsystem is not initialized.
 */
static int pscrr_reboot_notifier(struct notifier_block *nb,
				 unsigned long action, void *unused)
{
	int ret;

	if (!g_pscrr || !g_pscrr->ops || !g_pscrr->ops->write_reason)
		return NOTIFY_DONE;

	ret = g_pscrr->ops->write_reason(g_pscrr->last_reason);
	if (ret) {
		pr_err("PSCRR: Failed to store reason %d (%s) at reboot, err=%pe\n",
		       g_pscrr->last_reason,
		       pscrr_reason_to_str(g_pscrr->last_reason),
		       ERR_PTR(ret));
	} else {
		pr_info("PSCRR: Stored reason %d (%s) at reboot.\n",
			g_pscrr->last_reason,
			pscrr_reason_to_str(g_pscrr->last_reason));
	}

	/*
	 * Return NOTIFY_OK to allow reboot to proceed despite failure, in
	 * case there is any.
	 */
	return NOTIFY_OK;
}

/**
 * set_power_state_change_reason - Sets the power state change reason for
 *				   reboot.
 * @reason: The `pscr_reason` enum value indicating the reason for reboot.
 *
 * This function updates the last recorded power state change reason, which will
 * be stored in persistent storage when the system reboots. It allows various
 * subsystems (e.g., power management, thermal management) to indicate the cause
 * of a system reset.
 *
 * The reason is only updated if the PSCRR core is initialized.
 */
void set_power_state_change_reason(enum pscr_reason reason)
{
	if (g_pscrr)
		g_pscrr->last_reason = reason;
}
EXPORT_SYMBOL_GPL(set_power_state_change_reason);

/*----------------------------------------------------------------------*/
/* Sysfs Interface */
/*----------------------------------------------------------------------*/

/**
 * reason_show - Retrieves the current power state change reason via sysfs.
 * @kobj: Kernel object associated with this attribute (unused).
 * @attr: The sysfs attribute being accessed (unused).
 * @buf: Buffer to store the output string.
 *
 * This function is used to read the current power state change reason from
 * the `/sys/kernel/pscrr/reason` sysfs entry.
 *
 * If the PSCRR subsystem is not initialized, the function returns a message
 * indicating that no backend is registered.
 *
 * The returned value is formatted as an integer (`enum pscr_reason`) followed
 * by a newline (`\n`) for compatibility with standard sysfs behavior.
 *
 * Return:
 * - Number of bytes written to `buf` (formatted integer string).
 * - `"No backend registered\n"` if the PSCRR subsystem is uninitialized.
 */
static ssize_t reason_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	enum pscr_reason r;

	if (!g_pscrr || !g_pscrr->ops)
		return scnprintf(buf, PAGE_SIZE, "No backend registered\n");

	/* If the backend can read from hardware, do so. Otherwise, use our cached value. */
	if (g_pscrr->ops->read_reason) {
		if (g_pscrr->ops->read_reason(&r) == 0) {
			/* Also update our cached value for consistency */
			g_pscrr->last_reason = r;
		} else {
			/* If read fails, fallback to cached. */
			r = g_pscrr->last_reason;
		}
	} else {
		r = g_pscrr->last_reason;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", r);
}

/**
 * reason_store - Updates the current power state change reason via sysfs.
 * @kobj: Kernel object associated with this attribute (unused).
 * @attr: The sysfs attribute being modified (unused).
 * @buf: User-provided input buffer containing the reason value.
 * @count: Number of bytes written to the attribute.
 *
 * This function allows users to set the power state change reason through
 * the `/sys/kernel/pscrr/reason` sysfs entry.
 *
 * If the reason is out of range, a warning is logged but the write is still
 * attempted. If the backend write fails, an error is logged, and the function
 * returns the error code.
 *
 * Return:
 * - `count` on success (indicating the number of bytes processed).
 * - `-ENODEV` if the PSCRR subsystem is not initialized.
 * - Any other error code returned by the backend’s `write_reason()`.
 */
static ssize_t reason_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	int ret;
	long val;

	if (!g_pscrr || !g_pscrr->ops || !g_pscrr->ops->write_reason)
		return -ENODEV;

	ret = kstrtol(buf, 0, &val);
	if (ret)
		return ret;

	if (val < PSCR_UNKNOWN || val > PSCR_MAX_REASON)
		/*
		 * Log a warning, but still attempt to write the value. In
		 * case the backend can handle it, we don't want to block it.
		 */
		pr_warn("PSCRR: writing unknown reason %ld (out of range)\n",
			val);

	ret = g_pscrr->ops->write_reason((enum pscr_reason)val);
	if (ret) {
		pr_err("PSCRR: write_reason(%ld) failed, err=%d\n", val, ret);
		return ret;
	}

	g_pscrr->last_reason = (enum pscr_reason)val;

	return count; /* number of bytes consumed */
}

static struct kobj_attribute reason_attr = __ATTR(reason, 0664, reason_show,
						  reason_store);

/**
 * reason_boot_show - Retrieves the last recorded power state change reason.
 * @kobj: Kernel object associated with this attribute (unused).
 * @attr: The sysfs attribute being accessed (unused).
 * @buf: Buffer to store the output string.
 *
 * This function provides access to the `/sys/kernel/pscrr/reason_boot` sysfs
 * entry, which contains the last recorded power state change reason from the
 * **previous boot**. The value is retrieved from `priv->last_boot_reason`,
 * which is initialized at module load time by reading from persistent storage.
 *
 * If the PSCRR NVMEM backend (`priv`) is not initialized, the function returns
 * `-ENODEV` to indicate that the value is unavailable.
 *
 * The returned value is formatted as an integer (`enum pscr_reason`) followed
 * by a newline (`\n`) for sysfs compatibility.
 *
 * Return:
 * - Number of bytes written to `buf` (formatted integer string).
 * - `-ENODEV` if the PSCRR backend is not initialized.
 */
static ssize_t reason_boot_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	if (!g_pscrr)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%d\n", g_pscrr->last_boot_reason);
}

static struct kobj_attribute reason_boot_attr =
	__ATTR(reason_boot, 0444, reason_boot_show, NULL); /* Read-only */

static struct attribute *pscrr_attrs[] = {
	&reason_attr.attr,
	&reason_boot_attr.attr,
	NULL,
};

static struct attribute_group pscrr_attr_group = {
	.attrs = pscrr_attrs,
};

int pscrr_core_init(const struct pscrr_backend_ops *ops)
{
	enum pscr_reason stored_val;
	int err;

	if (g_pscrr) {
		pr_err("PSCRR: Core is already initialized!\n");
		return -EBUSY;
	}


	if (!ops->read_reason || !ops->write_reason) {
		pr_err("PSCRR: Backend must provide read and write callbacks\n");
		return -EINVAL;
	}

	g_pscrr = kzalloc(sizeof(*g_pscrr), GFP_KERNEL);
	if (!g_pscrr)
		return -ENOMEM;

	g_pscrr->ops = ops;
	g_pscrr->last_reason = PSCR_UNKNOWN;
	g_pscrr->last_boot_reason = PSCR_UNKNOWN;

	err = ops->read_reason(&stored_val);
	if (!err) {
		g_pscrr->last_boot_reason = stored_val;
		pr_info("PSCRR: Initial read_reason: %d (%s)\n",
			stored_val, pscrr_reason_to_str(stored_val));
	} else {
		pr_warn("PSCRR: read_reason failed, err=%pe\n",
			ERR_PTR(err));
	}

	/* Setup the reboot notifier */
	g_pscrr->reboot_nb.notifier_call = pscrr_reboot_notifier;
	err = register_reboot_notifier(&g_pscrr->reboot_nb);
	if (err) {
		pr_err("PSCRR: Failed to register reboot notifier, err=%pe\n",
		       ERR_PTR(err));
		goto err_free;
	}

	/* Create a kobject and sysfs group under /sys/kernel/pscrr */
	g_pscrr->kobj = kobject_create_and_add("pscrr", kernel_kobj);
	if (!g_pscrr->kobj) {
		pr_err("PSCRR: Failed to create /sys/kernel/pscrr\n");
		err = -ENOMEM;
		goto err_unreg_reboot;
	}

	err = sysfs_create_group(g_pscrr->kobj, &pscrr_attr_group);
	if (err) {
		pr_err("PSCRR: Failed to create sysfs group, err=%pe\n",
		       ERR_PTR(err));
		goto err_kobj_put;
	}

	pr_info("PSCRR: initialized successfully.\n");
	return 0;

err_kobj_put:
	kobject_put(g_pscrr->kobj);
err_unreg_reboot:
	unregister_reboot_notifier(&g_pscrr->reboot_nb);
err_free:
	kfree(g_pscrr);
	g_pscrr = NULL;
	return err;
}
EXPORT_SYMBOL_GPL(pscrr_core_init);

void pscrr_core_exit(void)
{
	if (!g_pscrr)
		return;

	sysfs_remove_group(g_pscrr->kobj, &pscrr_attr_group);
	kobject_put(g_pscrr->kobj);

	unregister_reboot_notifier(&g_pscrr->reboot_nb);

	kfree(g_pscrr);
	g_pscrr = NULL;
	pr_info("PSCRR: exited.\n");
}
EXPORT_SYMBOL_GPL(pscrr_core_exit);

MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("Power State Change Reason Recording (PSCRR) core");
MODULE_LICENSE("GPL");
