/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __REBOOT_MODE_H__
#define __REBOOT_MODE_H__

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/types.h>

/* Construct 64-bit reboot magic: arg2 in upper 32 bits, arg1 in lower 32 */
#define REBOOT_MODE_MAGIC(arg1, arg2) \
	(FIELD_PREP(GENMASK_ULL(31, 0), (arg1)) | \
	 FIELD_PREP(GENMASK_ULL(63, 32), (arg2)))
/* Get 32 bit arg1 from 64 bit magic */
#define REBOOT_MODE_ARG1(magic) FIELD_GET(GENMASK_ULL(31, 0), magic)
/* Get 32 bit arg2 from 64 bit magic */
#define REBOOT_MODE_ARG2(magic) FIELD_GET(GENMASK_ULL(63, 32), magic)

struct reboot_mode_driver {
	struct device *dev;
	struct list_head head;
	int (*write)(struct reboot_mode_driver *reboot, u64 magic);
	struct notifier_block reboot_notifier;
};

int reboot_mode_register(struct reboot_mode_driver *reboot);
int reboot_mode_unregister(struct reboot_mode_driver *reboot);
int devm_reboot_mode_register(struct device *dev,
			      struct reboot_mode_driver *reboot);
void devm_reboot_mode_unregister(struct device *dev,
				 struct reboot_mode_driver *reboot);

#endif
