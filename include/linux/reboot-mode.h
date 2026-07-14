/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __REBOOT_MODE_H__
#define __REBOOT_MODE_H__

#include <linux/types.h>

struct reboot_mode_entry {
	const char *name;
	u32 magic[3];
	int count;
};

struct reboot_mode_driver {
	struct device *dev;
	struct list_head head;
	/* List of predefined reboot-modes, populated via reboot_mode_add_predefined_modes(). */
	struct list_head predefined_modes;
	int (*write)(struct reboot_mode_driver *reboot, u32 *magic, int count);
	struct notifier_block reboot_notifier;
};

void reboot_mode_driver_init(struct reboot_mode_driver *reboot,
			     struct device *dev,
			     int (*write)(struct reboot_mode_driver *reboot, u32 *magic,
					  int count));
int reboot_mode_register(struct reboot_mode_driver *reboot);
int reboot_mode_unregister(struct reboot_mode_driver *reboot);
int devm_reboot_mode_register(struct device *dev,
			      struct reboot_mode_driver *reboot);
void devm_reboot_mode_unregister(struct device *dev,
				 struct reboot_mode_driver *reboot);
int reboot_mode_add_predefined_modes(struct reboot_mode_driver *reboot,
				     const struct reboot_mode_entry *modes,
				     size_t count);
void reboot_mode_reset_predefined_modes(struct reboot_mode_driver *reboot);

#endif
