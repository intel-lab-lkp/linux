/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CZ.NIC's Turris Omnia MCU driver
 *
 * 2024 by Marek Behún <kabel@kernel.org>
 */

#ifndef __TURRIS_OMNIA_MCU_H
#define __TURRIS_OMNIA_MCU_H

#include <linux/completion.h>
#include <linux/gpio/driver.h>
#include <linux/hw_random.h>
#include <linux/if_ether.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/workqueue.h>

struct i2c_client;
struct rtc_device;

struct omnia_mcu {
	struct i2c_client *client;
	const char *type;
	u32 features;

	/* board information */
	u64 board_serial_number;
	u8 board_first_mac[ETH_ALEN];
	u8 board_revision;

#ifdef CONFIG_TURRIS_OMNIA_MCU_GPIO
	/* GPIO chip */
	struct gpio_chip gc;
	struct mutex lock;
	unsigned long mask, rising, falling, both, cached, is_cached;
	/* Old MCU firmware handling needs the following */
	struct delayed_work button_release_emul_work;
	unsigned long last_status;
	bool button_pressed_emul;
#endif

#ifdef CONFIG_TURRIS_OMNIA_MCU_SYSOFF_WAKEUP
	/* RTC device for configuring wake-up */
	struct rtc_device *rtcdev;
	u32 rtc_alarm;
	bool front_button_poweron;
#endif

#ifdef CONFIG_TURRIS_OMNIA_MCU_WATCHDOG
	/* MCU watchdog */
	struct watchdog_device wdt;
#endif

#ifdef CONFIG_TURRIS_OMNIA_MCU_TRNG
	/* true random number generator */
	struct hwrng trng;
	struct completion trng_entropy_ready;
#endif
};

#ifdef CONFIG_TURRIS_OMNIA_MCU_GPIO
extern const u8 omnia_int_to_gpio_idx[32];
extern const struct attribute_group omnia_mcu_gpio_group;
int omnia_mcu_register_gpiochip(struct omnia_mcu *mcu);
#else
static inline int omnia_mcu_register_gpiochip(struct omnia_mcu *mcu)
{
	return 0;
}
#endif

#ifdef CONFIG_TURRIS_OMNIA_MCU_SYSOFF_WAKEUP
extern const struct attribute_group omnia_mcu_poweroff_group;
int omnia_mcu_register_sys_off_and_wakeup(struct omnia_mcu *mcu);
#else
static inline int omnia_mcu_register_sys_off_and_wakeup(struct omnia_mcu *mcu)
{
	return 0;
}
#endif

#ifdef CONFIG_TURRIS_OMNIA_MCU_TRNG
int omnia_mcu_register_trng(struct omnia_mcu *mcu);
#else
static inline int omnia_mcu_register_trng(struct omnia_mcu *mcu)
{
	return 0;
}
#endif

#ifdef CONFIG_TURRIS_OMNIA_MCU_WATCHDOG
int omnia_mcu_register_watchdog(struct omnia_mcu *mcu);
#else
static inline int omnia_mcu_register_watchdog(struct omnia_mcu *mcu)
{
	return 0;
}
#endif

#endif /* __TURRIS_OMNIA_MCU_H */
