/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PSCR_H__
#define __PSCR_H__

enum power_state_change_reason {
	PSCR_UNKNOWN,
	PSCR_UNDER_VOLTAGE,
	PSCR_OVER_CURRENT,
	PSCR_REGULATOR_FAILURE,
	PSCR_OVERTEMPERATURE,
};

struct pscr_driver {
	struct device *dev;
	struct list_head head;
	int (*write)(struct pscr_driver *pscr_drv, u32 magic);
	struct notifier_block reboot_notifier;
	u32 max_magic;
};

int pscr_register(struct pscr_driver *pscr_drv);
void pscr_unregister(struct pscr_driver *pscr_drv);
int devm_pscr_register(struct device *dev,
		       struct pscr_driver *pscr_drv);
void devm_pscr_unregister(struct device *dev,
			  struct pscr_driver *pscr_drv);


#if IS_ENABLED(CONFIG_PSCR)

void set_power_state_change_reason(enum power_state_change_reason reason);

#else

static inline void set_power_state_change_reason(enum power_state_change_reason reason)
{
}
#endif

#endif
