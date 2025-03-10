/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pscrr.h - Public header for Power State Change Reason Recording (PSCRR).
 */

#ifndef __PSCRR_H__
#define __PSCRR_H__

/*
 * enum pscr - Enumerates reasons for power state changes.
 *
 * This enum lists the various reasons why a power state change might
 * occur in a system. Each value represents a specific condition that
 * could trigger a change in power state, such as shutdown or reboot.
 *
 * PSCR_UNKNOWN: Represents an unknown or unspecified reason.
 * PSCR_UNDER_VOLTAGE: Indicates a power state change due to under-voltage.
 * PSCR_OVER_CURRENT: Indicates a power state change due to over-current.
 * PSCR_REGULATOR_FAILURE: Indicates a failure in a voltage regulator.
 * PSCR_OVERTEMPERATURE: Indicates an over-temperature condition.
 */
enum pscr_reason {
	PSCR_UNKNOWN,
	PSCR_UNDER_VOLTAGE,
	PSCR_OVER_CURRENT,
	PSCR_REGULATOR_FAILURE,
	PSCR_OVERTEMPERATURE,
	PSCR_REASON_COUNT,
};

#define PSCR_MAX_REASON	(PSCR_REASON_COUNT - 1)

/**
 * struct pscrr_backend_ops - Backend operations for storing power state change
 *			      reasons.
 *
 * This structure defines the interface for backend implementations that handle
 * the persistent storage of power state change reasons. Different backends
 * (e.g., NVMEM, EEPROM, battery-backed RAM) can implement these operations to
 * store and retrieve shutdown reasons across reboots.
 *
 * @write_reason: Function pointer to store the specified `pscr_reason` in
 *		  persistent storage. This function is called before a reboot
 *		  to record the last power state change reason.
 * @read_reason:  Function pointer to retrieve the last stored `pscr_reason`
 *		  from persistent storage. This function is called at boot to
 *		  restore the shutdown reason.
 */
struct pscrr_backend_ops {
	int (*write_reason)(enum pscr_reason reason);
	int (*read_reason)(enum pscr_reason *reason);
};

/**
 * pscrr_core_init - Initialize the PSCRR core with a given backend
 * @ops: Backend operations that the core will call
 *
 * Return: 0 on success, negative error code on failure.
 * The core sets up sysfs, registers reboot notifier, etc.
 */
int pscrr_core_init(const struct pscrr_backend_ops *ops);

/**
 * pscrr_core_exit - De-initialize the PSCRR core
 *
 * Unregisters the reboot notifier, removes the sysfs entries, etc.
 * Should be called by the backend driver at removal/shutdown.
 */
void pscrr_core_exit(void);

#if IS_ENABLED(CONFIG_PSCRR)

/**
 * set_power_state_change_reason - Record reason for next reboot/shutdown
 * @reason: The enumerated reason code to record
 *
 * Other drivers (e.g. regulator, thermal) call this whenever they detect
 * a condition that may lead to or cause a reboot.
 */
void set_power_state_change_reason(enum pscr_reason reason);

#else

static inline void set_power_state_change_reason(enum pscr_reason pscr)
{
}
#endif

#endif /* __PSCRR_H__ */
