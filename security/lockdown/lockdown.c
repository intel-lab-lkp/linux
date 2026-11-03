// SPDX-License-Identifier: GPL-2.0
/* Lock down the kernel
 *
 * Copyright (C) 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#include <linux/security.h>
#include <linux/lsm_hooks.h>
#include <uapi/linux/lsm.h>

/*
 * These are descriptions of the reasons that can be passed to the
 * security_locked_down() LSM hook, used for the denial message and the
 * securityfs status file.
 */
static const char *const lockdown_reasons[LOCKDOWN_CONFIDENTIALITY_MAX + 1] = {
	[LOCKDOWN_NONE] = "none",
	[LOCKDOWN_MODULE_SIGNATURE] = "unsigned module loading",
	[LOCKDOWN_DEV_MEM] = "/dev/mem,kmem,port",
	[LOCKDOWN_EFI_TEST] = "/dev/efi_test access",
	[LOCKDOWN_KEXEC] = "kexec of unsigned images",
	[LOCKDOWN_HIBERNATION] = "hibernation",
	[LOCKDOWN_PCI_ACCESS] = "direct PCI access",
	[LOCKDOWN_IOPORT] = "raw io port access",
	[LOCKDOWN_MSR] = "raw MSR access",
	[LOCKDOWN_ACPI_TABLES] = "modifying ACPI tables",
	[LOCKDOWN_DEVICE_TREE] = "modifying device tree contents",
	[LOCKDOWN_PCMCIA_CIS] = "direct PCMCIA CIS storage",
	[LOCKDOWN_TIOCSSERIAL] = "reconfiguration of serial port IO",
	[LOCKDOWN_MODULE_PARAMETERS] = "unsafe module parameters",
	[LOCKDOWN_MMIOTRACE] = "unsafe mmio",
	[LOCKDOWN_DEBUGFS] = "debugfs access",
	[LOCKDOWN_XMON_WR] = "xmon write access",
	[LOCKDOWN_BPF_WRITE_USER] = "use of bpf to write user RAM",
	[LOCKDOWN_DBG_WRITE_KERNEL] = "use of kgdb/kdb to write kernel RAM",
	[LOCKDOWN_RTAS_ERROR_INJECTION] = "RTAS error injection",
	[LOCKDOWN_XEN_USER_ACTIONS] = "Xen guest user action",
	[LOCKDOWN_INTEGRITY_MAX] = "integrity",
	[LOCKDOWN_KCORE] = "/proc/kcore access",
	[LOCKDOWN_KPROBES] = "use of kprobes",
	[LOCKDOWN_BPF_READ_KERNEL] = "use of bpf to read kernel RAM",
	[LOCKDOWN_DBG_READ_KERNEL] = "use of kgdb/kdb to read kernel RAM",
	[LOCKDOWN_PERF] = "unsafe use of perf",
	[LOCKDOWN_TRACEFS] = "use of tracefs",
	[LOCKDOWN_XMON_RW] = "xmon read and write access",
	[LOCKDOWN_XFRM_SECRET] = "xfrm SA secret",
	[LOCKDOWN_CONFIDENTIALITY_MAX] = "confidentiality",
};

static enum lockdown_reason kernel_locked_down;

static const enum lockdown_reason lockdown_levels[] = {LOCKDOWN_NONE,
						 LOCKDOWN_INTEGRITY_MAX,
						 LOCKDOWN_CONFIDENTIALITY_MAX};

/*
 * Put the kernel into lock-down mode.
 */
static int lock_kernel_down(const char *where, enum lockdown_reason level)
{
	if (kernel_locked_down >= level)
		return -EPERM;

	kernel_locked_down = level;
	pr_notice("Kernel is locked down from %s; see man kernel_lockdown.7\n",
		  where);
	return 0;
}

static int __init lockdown_param(char *level)
{
	if (!level)
		return -EINVAL;

	if (strcmp(level, "integrity") == 0)
		lock_kernel_down("command line", LOCKDOWN_INTEGRITY_MAX);
	else if (strcmp(level, "confidentiality") == 0)
		lock_kernel_down("command line", LOCKDOWN_CONFIDENTIALITY_MAX);
	else
		return -EINVAL;

	return 0;
}

early_param("lockdown", lockdown_param);

/**
 * lockdown_is_locked_down - Find out if the kernel is locked down
 * @what: Tag to use in notice generated if lockdown is in effect
 */
static int lockdown_is_locked_down(enum lockdown_reason what)
{
	if (WARN(what >= LOCKDOWN_CONFIDENTIALITY_MAX,
		 "Invalid lockdown reason"))
		return -EPERM;

	if (kernel_locked_down >= what) {
		if (lockdown_reasons[what])
			pr_notice_ratelimited("Lockdown: %s: %s is restricted; see man kernel_lockdown.7\n",
				  current->comm, lockdown_reasons[what]);
		return -EPERM;
	}

	return 0;
}

static struct security_hook_list lockdown_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(locked_down, lockdown_is_locked_down),
};

static const struct lsm_id lockdown_lsmid = {
	.name = "lockdown",
	.id = LSM_ID_LOCKDOWN,
};

static int __init lockdown_lsm_init(void)
{
#if defined(CONFIG_LOCK_DOWN_KERNEL_FORCE_INTEGRITY)
	lock_kernel_down("Kernel configuration", LOCKDOWN_INTEGRITY_MAX);
#elif defined(CONFIG_LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY)
	lock_kernel_down("Kernel configuration", LOCKDOWN_CONFIDENTIALITY_MAX);
#endif
	security_add_hooks(lockdown_hooks, ARRAY_SIZE(lockdown_hooks),
			   &lockdown_lsmid);
	return 0;
}

static ssize_t lockdown_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *ppos)
{
	char temp[80] = "";
	int i, offset = 0;

	for (i = 0; i < ARRAY_SIZE(lockdown_levels); i++) {
		enum lockdown_reason level = lockdown_levels[i];

		if (lockdown_reasons[level]) {
			const char *label = lockdown_reasons[level];

			if (kernel_locked_down == level)
				offset += scnprintf(temp + offset,
						    sizeof(temp) - offset,
						    "[%s] ", label);
			else
				offset += scnprintf(temp + offset,
						    sizeof(temp) - offset,
						    "%s ", label);
		}
	}

	/* Convert the last space to a newline if needed. */
	if (offset > 0)
		temp[offset-1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, temp, strlen(temp));
}

static ssize_t lockdown_write(struct file *file, const char __user *buf,
			      size_t n, loff_t *ppos)
{
	char *state;
	int i, len, err = -EINVAL;

	state = memdup_user_nul(buf, n);
	if (IS_ERR(state))
		return PTR_ERR(state);

	len = strlen(state);
	if (len && state[len-1] == '\n')
		state[len-1] = '\0';

	for (i = 0; i < ARRAY_SIZE(lockdown_levels); i++) {
		enum lockdown_reason level = lockdown_levels[i];
		const char *label = lockdown_reasons[level];

		if (label && !strcmp(state, label)) {
			err = lock_kernel_down("securityfs", level);
			break;
		}
	}

	kfree(state);
	return err ? err : n;
}

static const struct file_operations lockdown_ops = {
	.read  = lockdown_read,
	.write = lockdown_write,
};

static int __init lockdown_secfs_init(void)
{
	return PTR_ERR_OR_ZERO(securityfs_create_file("lockdown", 0644, NULL,
						      NULL, &lockdown_ops));
}

#ifdef CONFIG_SECURITY_LOCKDOWN_LSM_EARLY
DEFINE_EARLY_LSM(lockdown) = {
#else
DEFINE_LSM(lockdown) = {
#endif
	.id = &lockdown_lsmid,
	.init = lockdown_lsm_init,
	.initcall_core = lockdown_secfs_init,
};
