// SPDX-License-Identifier: GPL-2.0
/*
 * pscrr_core.c - Core Power State Change Reason Recording
 *
 * This framework provides a method for recording the cause of the last system
 * reboot or shutdown, in scenarios where software can detect the shutdown cause
 * early enough, or where hardware components (e.g., PMICs) retain this
 * information across reboots.
 *
 * Unlike traditional logging mechanisms that rely on block storage (e.g., NAND,
 * eMMC), PSCRR enables persistent recording of shutdown reasons using
 * lightweight non-volatile memory, suitable for early boot diagnostics.
 *
 * Purpose:
 * --------
 * The primary goal of PSCRR is to help developers and system operators analyze
 * field failures by tracking the reason for each power state change. This
 * improves root cause analysis and can aid in future recovery strategies.
 *
 * The framework is useful when the system includes backup power (e.g.,
 * capacitors) and early power-fail detection, allowing software enough time to
 * record the reason. It can also support hardware-reported reasons, if
 * available.
 *
 * Sysfs Interface:
 * ----------------
 *   /sys/kernel/pscrr/reason       - Read/write current power state change
 *				      reason
 *   /sys/kernel/pscrr/reason_boot  - Read-only last recorded reason from
 *				      previous boot
 *
 * Why is this needed?
 * -------------------
 * On many embedded systems:
 *   - Block storage cannot be updated safely during power loss.
 *   - Power-down may be too fast to allow clean shutdown.
 *
 * To enable reliable postmortem diagnostics, alternate non-volatile storage
 * should be used, such as:
 *   - Battery-backed RTC scratchpads
 *   - EEPROM or NVMEM cells
 *   - FRAM or other persistent low-power memory
 *
 * How PSCRR Works:
 * ----------------
 *   - A driver detects a protection event (e.g., regulator failure) and calls:
 *       hw_protection_trigger(PSCR_REGULATOR_FAILURE,
 *                              REGULATOR_FORCED_SAFETY_SHUTDOWN_WAIT_MS);
 *   - Or, userspace sets the reason before shutdown:
 *       echo 3 > /sys/kernel/pscrr/reason
 *   - On reboot, PSCRR stores the reason using the backend’s .write_reason().
 *   - The next boot reads it via .read_reason() and exposes it through sysfs.
 *
 * If only hardware-backed sources are used (e.g., PMIC or watchdog), the reason
 * is not written by PSCRR but read from the hardware after the system restarts.
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/power/power_on_reason.h>
#include <linux/pscrr.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

struct pscrr_backend {
	const struct pscrr_backend_ops *ops;

	enum psc_reason last_boot_reason;
};

struct pscrr_core {
	struct mutex lock;
	struct pscrr_backend *backend;
	/* Kobject for sysfs */
	struct kobject *kobj;
	struct notifier_block reboot_nb;
} g_pscrr = {
	.lock = __MUTEX_INITIALIZER(g_pscrr.lock),
};

DEFINE_GUARD(g_pscrr, struct pscrr_core *, mutex_lock(&_T->lock),
	     mutex_unlock(&_T->lock));

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
	struct pscrr_backend *backend;
	int ret;

	guard(g_pscrr)(&g_pscrr);

	backend = g_pscrr.backend;

	if (!backend || !backend->ops || !backend->ops->write_reason)
		return NOTIFY_DONE;

	ret = backend->ops->write_reason(get_psc_reason());
	if (ret) {
		pr_err("PSCRR: Failed to store reason %d (%s) at reboot, err=%pe\n",
		       get_psc_reason(), psc_reason_to_str(get_psc_reason()),
		       ERR_PTR(ret));
	} else {
		pr_info("PSCRR: Stored reason %d (%s) at reboot.\n",
			get_psc_reason(), psc_reason_to_str(get_psc_reason()));
	}

	/*
	 * Return NOTIFY_OK to allow reboot to proceed despite failure, in
	 * case there is any.
	 */
	return NOTIFY_OK;
}

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
 * The returned value is formatted as an integer (`enum psc_reason`) followed
 * by a newline (`\n`) for compatibility with standard sysfs behavior.
 *
 * Return:
 * - Number of bytes written to `buf` (formatted integer string).
 * - `"No backend registered\n"` if the PSCRR subsystem is uninitialized.
 */
static ssize_t reason_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct pscrr_backend *backend;
	enum psc_reason r;

	guard(g_pscrr)(&g_pscrr);

	backend = g_pscrr.backend;

	if (!backend || !backend->ops)
		return scnprintf(buf, PAGE_SIZE, "No backend registered\n");

	/* If the backend can read from hardware, do so. Otherwise, use our cached value. */
	if (backend->ops->read_reason) {
		if (backend->ops->read_reason(&r) == 0) {
			/* Also update our cached value for consistency */
			set_psc_reason(r);
		} else {
			/* If read fails, fallback to cached. */
			r = get_psc_reason();
		}
	} else {
		r = get_psc_reason();
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
	struct pscrr_backend *backend;
	long val;
	int ret;

	guard(g_pscrr)(&g_pscrr);

	backend = g_pscrr.backend;

	if (!backend || !backend->ops || !backend->ops->write_reason)
		return -ENODEV;

	ret = kstrtol(buf, 10, &val);
	if (ret)
		return ret;

	if (val > U32_MAX)
		return -ERANGE;

	if (val < PSCR_UNKNOWN || val > PSCR_MAX_REASON)
		/*
		 * Log a warning, but still attempt to write the value. In
		 * case the backend can handle it, we don't want to block it.
		 */
		pr_warn("PSCRR: writing unknown reason %ld (out of range)\n",
			val);

	ret = backend->ops->write_reason((enum psc_reason)val);
	if (ret) {
		pr_err("PSCRR: write_reason(%ld) failed, err=%d\n", val, ret);
		return ret;
	}

	set_psc_reason((enum psc_reason)val);

	return count; /* number of bytes consumed */
}

static struct kobj_attribute reason_attr = __ATTR(reason, 0644, reason_show,
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
 * The returned value is formatted as an integer (`enum psc_reason`) followed
 * by a newline (`\n`) for sysfs compatibility.
 *
 * Return:
 * - Number of bytes written to `buf` (formatted integer string).
 * - `-ENODEV` if the PSCRR backend is not initialized.
 */
static ssize_t reason_boot_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	enum psc_reason last_boot_reason;
	struct pscrr_backend *backend;

	guard(g_pscrr)(&g_pscrr);

	backend = g_pscrr.backend;

	if (!backend)
		return -ENODEV;

	last_boot_reason = backend->last_boot_reason;

	return scnprintf(buf, PAGE_SIZE, "%d\n", last_boot_reason);
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
	enum psc_reason stored_val = PSCR_UNKNOWN;
	struct pscrr_backend *backend;
	int ret;

	guard(g_pscrr)(&g_pscrr);

	backend = g_pscrr.backend;

	if (backend) {
		pr_err("PSCRR: Core is already initialized!\n");
		return -EBUSY;
	}

	if (!ops->read_reason) {
		pr_err("PSCRR: Backend must provide read callbacks\n");
		return -EINVAL;
	}

	backend = kzalloc(sizeof(*backend), GFP_KERNEL);
	if (!backend)
		return -ENOMEM;

	backend->ops = ops;
	backend->last_boot_reason = PSCR_UNKNOWN;
	g_pscrr.backend = backend;

	ret = ops->read_reason(&stored_val);
	if (!ret) {
		backend->last_boot_reason = stored_val;
		pr_info("PSCRR: Initial read_reason: %d (%s)\n",
			stored_val, psc_reason_to_str(stored_val));
	} else {
		pr_warn("PSCRR: read_reason failed, err=%pe\n",
			ERR_PTR(ret));
	}

	/* Setup the reboot notifier */
	g_pscrr.reboot_nb.notifier_call = pscrr_reboot_notifier;
	ret = register_reboot_notifier(&g_pscrr.reboot_nb);
	if (ret) {
		pr_err("PSCRR: Failed to register reboot notifier, err=%pe\n",
		       ERR_PTR(ret));
		goto err_free;
	}

	/* Create a kobject and sysfs group under /sys/kernel/pscrr */
	g_pscrr.kobj = kobject_create_and_add("pscrr", kernel_kobj);
	if (!g_pscrr.kobj) {
		pr_err("PSCRR: Failed to create /sys/kernel/pscrr\n");
		ret = -ENOMEM;
		goto err_unreg_reboot;
	}

	ret = sysfs_create_group(g_pscrr.kobj, &pscrr_attr_group);
	if (ret) {
		pr_err("PSCRR: Failed to create sysfs group, err=%pe\n",
		       ERR_PTR(ret));
		goto err_kobj_put;
	}

	pr_info("PSCRR: initialized successfully.\n");

	return 0;

err_kobj_put:
	kobject_put(g_pscrr.kobj);
err_unreg_reboot:
	unregister_reboot_notifier(&g_pscrr.reboot_nb);
err_free:
	kfree(g_pscrr.backend);
	g_pscrr.backend = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(pscrr_core_init);

void pscrr_core_exit(void)
{
	guard(g_pscrr)(&g_pscrr);

	if (!g_pscrr.backend)
		return;

	if (g_pscrr.kobj) {
		sysfs_remove_group(g_pscrr.kobj, &pscrr_attr_group);
		kobject_put(g_pscrr.kobj);
	}

	unregister_reboot_notifier(&g_pscrr.reboot_nb);

	kfree(g_pscrr.backend);
	g_pscrr.backend = NULL;

	pr_info("PSCRR: exited.\n");
}
EXPORT_SYMBOL_GPL(pscrr_core_exit);

MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("Power State Change Reason Recording (PSCRR) core");
MODULE_LICENSE("GPL");
