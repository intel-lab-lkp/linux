/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023, Intel Corporation.
 * Author: Michal Michalik <michal.michalik@intel.com>
 */

#ifndef NSIM_DPLL_H
#define NSIM_DPLL_H

#include <linux/types.h>
#include <linux/netlink.h>

#include <linux/dpll.h>
#include <uapi/linux/dpll.h>

#define EEC_DPLL_DEV 0
#define EEC_DPLL_TEMPERATURE 20
#define PPS_DPLL_DEV 1
#define PPS_DPLL_TEMPERATURE 30
#define DPLLS_CLOCK_ID 234

#define PIN_GNSS 0
#define PIN_GNSS_CAPABILITIES 2 /* DPLL_PIN_CAPS_PRIORITY_CAN_CHANGE */
#define PIN_GNSS_PRIORITY 5

#define PIN_PPS 1
#define PIN_PPS_CAPABILITIES 7 /* DPLL_PIN_CAPS_DIRECTION_CAN_CHANGE
				* || DPLL_PIN_CAPS_PRIORITY_CAN_CHANGE
				* || DPLL_PIN_CAPS_STATE_CAN_CHANGE
				*/
#define PIN_PPS_PRIORITY 6

#define PIN_RCLK 2
#define PIN_RCLK_CAPABILITIES 6 /* DPLL_PIN_CAPS_PRIORITY_CAN_CHANGE
				 * || DPLL_PIN_CAPS_STATE_CAN_CHANGE
				 */
#define PIN_RCLK_PRIORITY 7

#define EEC_PINS_NUMBER 3
#define PPS_PINS_NUMBER 2

struct dpll_pd {
	enum dpll_mode mode;
	int temperature;
};

struct pin_pd {
	u64 frequency;
	enum dpll_pin_direction direction;
	enum dpll_pin_state state_pin;
	enum dpll_pin_state state_dpll;
	u32 prio;
};

struct nsim_dpll_info {
	bool owner;

	struct dpll_device *dpll_e;
	struct dpll_pd *dpll_e_pd;
	struct dpll_device *dpll_p;
	struct dpll_pd *dpll_p_pd;

	struct dpll_pin_properties *pp_gnss;
	struct dpll_pin *p_gnss;
	struct pin_pd *p_gnss_pd;

	struct dpll_pin_properties *pp_pps;
	struct dpll_pin *p_pps;
	struct pin_pd *p_pps_pd;

	struct dpll_pin_properties *pp_rclk;
	struct dpll_pin *p_rclk;
	struct pin_pd *p_rclk_pd;
};

int nsim_dpll_init_owner(struct nsim_dpll_info *dpll, int devid);
void nsim_dpll_free_owner(struct nsim_dpll_info *dpll);
int nsim_rclk_init(struct nsim_dpll_info *dpll, int devid, unsigned int index);
void nsim_rclk_free(struct nsim_dpll_info *dpll);

#endif /* NSIM_DPLL_H */
