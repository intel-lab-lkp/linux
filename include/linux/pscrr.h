/* SPDX-License-Identifier: GPL-2.0 */
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
enum pscr {
	PSCR_UNKNOWN,
	PSCR_UNDER_VOLTAGE,
	PSCR_OVER_CURRENT,
	PSCR_REGULATOR_FAILURE,
	PSCR_OVERTEMPERATURE,
};

/*
 * struct pscrr_device - Manages a Power State Change Reason Recorder device.
 *
 * This structure is utilized for controlling a device responsible for
 * recording reasons for power state changes (PSCR). It includes mechanisms
 * for mapping PSCR values to specific magic codes, storing these mappings,
 * and recovering the last PSCR value from storage during system start-up.
 *
 * @dev: Device structure pointer.
 * @pscr_map_list: List head for structs holding PSCR to magic code mappings.
 * @write: Function pointer to write a new mapped PSCR value.
 * @read: Function pointer to read the current mapped PSCR value.
 * @reboot_notifier: Notifier block for recording PSCR at reboot.
 * @max_magic_val: Maximum permissible magic code, used for verifying storage
 *                 capacity and mapping integrity.
 * @last_pscr: Last PSCR value recovered from storage at system start,
 *             representing the reason for the last system power cycle.
 */
struct pscrr_device {
	struct device *dev;
	struct list_head pscr_map_list;
	int (*write)(struct pscrr_device *pscrr_dev, u32 magic);
	int (*read)(struct pscrr_device *pscrr_dev, u32 *magic);
	struct notifier_block reboot_notifier;
	u32 max_magic_val;
	enum pscr last_pscr;
};

int pscrr_register(struct pscrr_device *pscrr_dev);
void pscrr_unregister(struct pscrr_device *pscrr_dev);
int devm_pscrr_register(struct device *dev,
			struct pscrr_device *pscrr_dev);
void devm_pscrr_unregister(struct device *dev,
			   struct pscrr_device *pscrr_dev);
int handle_last_pscr(struct pscrr_device *pscrr_dev);

#if IS_ENABLED(CONFIG_PSCRR)

void set_power_state_change_reason(enum pscr pscr);

#else

static inline void set_power_state_change_reason(enum pscr pscr)
{
}
#endif

#endif
