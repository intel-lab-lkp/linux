/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *
 * Copyright (C) 2015 ARM Limited
 */

#ifndef __LINUX_PSCI_H
#define __LINUX_PSCI_H

#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/types.h>

#define PSCI_POWER_STATE_TYPE_STANDBY		0
#define PSCI_POWER_STATE_TYPE_POWER_DOWN	1

bool psci_tos_resident_on(int cpu);

int psci_cpu_suspend_enter(u32 state);
bool psci_power_state_is_valid(u32 state);
int psci_set_osi_mode(bool enable);
bool psci_has_osi_support(void);

/**
 * enum psci_standard_resets - Standard reset selectors for PSCI reset
 * @PSCI_SYSTEM_RESET_COLD_RESET: Standard SYSTEM_RESET command.
 * @PSCI_SYSTEM_RESET2_ARCH_WARM_RESET: SYSTEM_RESET2 architectural warm reset.
 */
enum psci_standard_resets {
	PSCI_SYSTEM_RESET_COLD_RESET = 1,
	PSCI_SYSTEM_RESET2_ARCH_WARM_RESET,
};

struct psci_operations {
	u32 (*get_version)(void);
	int (*cpu_suspend)(u32 state, unsigned long entry_point);
	int (*cpu_off)(u32 state);
	int (*cpu_on)(unsigned long cpuid, unsigned long entry_point);
	int (*migrate)(unsigned long cpuid);
	int (*affinity_info)(unsigned long target_affinity,
			unsigned long lowest_affinity_level);
	int (*migrate_info_type)(void);
};

extern struct psci_operations psci_ops;

struct psci_0_1_function_ids {
	u32 cpu_suspend;
	u32 cpu_on;
	u32 cpu_off;
	u32 migrate;
};

struct psci_0_1_function_ids get_psci_0_1_function_ids(void);

#if defined(CONFIG_ARM_PSCI_FW)
int __init psci_dt_init(void);
/**
 * psci_set_reset_cmd() - Configure PSCI reset command
 * @reset_type: SYSTEM_RESET2 vendor-specific reset_type as defined by
 *		firmware, or 0 for standard resets
 * @cookie: SYSTEM_RESET2 vendor-specific cookie as defined by firmware or one
 *		of enum psci_standard_resets when @reset_type is set to 0
 *
 * Supported commands:
 * - PSCI SYSTEM_RESET2 vendor-specific reset:
 *   - @reset_type and @cookie must follow platform-specific SYSTEM_RESET2
 *     vendor-reset encoding.
 * - Standard reset selector:
 *   - @reset_type must be 0.
 *   - @cookie must be one of enum psci_standard_resets.
 *
 * This is an in-kernel helper intended for built-in reboot flow callers.
 * reset command can be set only one time per boot cycle.
 *
 * Return: 0 on success, -EINVAL if both inputs are zero, -EBUSY if reset
 * command is already set.
 */
int psci_set_reset_cmd(u32 reset_type, u64 cookie);
bool psci_has_system_reset2_support(void);
#else
static inline int psci_dt_init(void) { return 0; }
static inline int psci_set_reset_cmd(u32 reset_type, u64 cookie) { return 0; }
static inline bool psci_has_system_reset2_support(void) { return false; }
#endif

#if defined(CONFIG_ARM_PSCI_FW) && defined(CONFIG_ACPI)
int __init psci_acpi_init(void);
bool __init acpi_psci_present(void);
bool acpi_psci_use_hvc(void);
#else
static inline int psci_acpi_init(void) { return 0; }
static inline bool acpi_psci_present(void) { return false; }
static inline bool acpi_psci_use_hvc(void) {return false; }
#endif

#endif /* __LINUX_PSCI_H */
