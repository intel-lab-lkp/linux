/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pscrr.h - Public header for Power State Change Reason Recording (PSCRR).
 */

#ifndef __PSCRR_H__
#define __PSCRR_H__

#include <linux/reboot.h>

/**
 * struct pscrr_backend_ops - Backend operations for storing power state change
 *                            reasons.
 *
 * This structure defines the interface for backend implementations that handle
 * the persistent storage of power state change reasons. Different backends
 * (e.g., NVMEM, EEPROM, battery-backed RAM) can implement these operations to
 * store and retrieve shutdown reasons across reboots.
 *
 * Some systems may have **read-only** hardware-based providers, such as PMICs
 * (Power Management ICs), that automatically log reset reasons without software
 * intervention. In such cases, the backend may implement only the `read_reason`
 * function, while `write_reason` remains unused or unimplemented.
 *
 * @write_reason: Function pointer to store the specified `psc_reason` in
 *                persistent storage. This function is called before a reboot
 *                to record the last power state change reason. Some hardware
 *                may not support software-initiated writes, in which case
 *                this function may not be required.
 * @read_reason:  Function pointer to retrieve the last stored `psc_reason`
 *                from persistent storage. This function is called at boot to
 *                restore the shutdown reason. On read-only hardware providers
 *                (e.g., PMICs with built-in reset reason registers), this may
 *                be the only function implemented.
 */
struct pscrr_backend_ops {
	int (*write_reason)(enum psc_reason reason);
	int (*read_reason)(enum psc_reason *reason);
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

#endif /* __PSCRR_H__ */
