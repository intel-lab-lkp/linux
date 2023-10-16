/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2023, Intel Corporation. */

#ifndef _ICE_HWMON_H_
#define _ICE_HWMON_H_

#if IS_ENABLED(CONFIG_HWMON)
void ice_hwmon_init(struct ice_pf *pf);
void ice_hwmon_exit(struct ice_pf *pf);
#else
static void ice_hwmon_init(struct ice_pf *pf) { }
static void ice_hwmon_exit(struct ice_pf *pf) { }
#endif

#endif /* _ICE_HWMON_H_ */
