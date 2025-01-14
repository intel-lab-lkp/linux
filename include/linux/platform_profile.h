/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Platform profile sysfs interface
 *
 * See Documentation/userspace-api/sysfs-platform_profile.rst for more
 * information.
 */

#ifndef _PLATFORM_PROFILE_H_
#define _PLATFORM_PROFILE_H_

#include <linux/device.h>
#include <linux/bitops.h>

/*
 * If more options are added please update profile_names array in
 * platform_profile.c and sysfs-platform_profile documentation.
 */

enum platform_profile_option {
	PLATFORM_PROFILE_LOW_POWER,
	PLATFORM_PROFILE_COOL,
	PLATFORM_PROFILE_QUIET,
	PLATFORM_PROFILE_BALANCED,
	PLATFORM_PROFILE_BALANCED_PERFORMANCE,
	PLATFORM_PROFILE_PERFORMANCE,
	PLATFORM_PROFILE_CUSTOM,
	PLATFORM_PROFILE_LAST, /*must always be last */
};

/**
 * struct platform_profile_ops - platform profile operations
 * @probe:	Callback to setup choices available to the new class device.
 *		Parameters are:
 *		@drvdata: drvdata pointer passed to platform_profile_register.
 *		@choices: Empty choices bitmap which the driver has to manually
 *			  setup, by using set_bit() in bits corresponding to
 *			  platform_profile_option values. These values will only
 *			  be enforced when a new profile is selected from
 *			  user-space.
 * @profile_get: Callback that will be called when showing the current platform
 *		 profile.
 *		 Parameters are:
 *		 @dev: Class device.
 *		 @profile: Pointer to the profile which will be read from
 *			   user-space. Selected choices are not enforced when
 *			   modifying this value.
 * @profile_set: Callback that will be called when storing the new platform
 *		 profile.
 *		 Parameters are:
 *		 @dev: Class device.
 *		 @profile: New platform profile to be set. Guaranteed to be a
 *			   value selected in the @probe callback.
 */
struct platform_profile_ops {
	int (*probe)(void *drvdata, unsigned long *choices);
	int (*profile_get)(struct device *dev, enum platform_profile_option *profile);
	int (*profile_set)(struct device *dev, enum platform_profile_option profile);
};

struct device *platform_profile_register(struct device *dev, const char *name,
					 void *drvdata,
					 const struct platform_profile_ops *ops);
int platform_profile_remove(struct device *dev);
struct device *devm_platform_profile_register(struct device *dev, const char *name,
					      void *drvdata,
					      const struct platform_profile_ops *ops);
int platform_profile_cycle(void);
void platform_profile_notify(struct device *dev);

#endif  /*_PLATFORM_PROFILE_H_*/
