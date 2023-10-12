/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2022, Intel Corporation. */

#ifndef _ICE_HWMON_H_
#define _ICE_HWMON_H_

void ice_hwmon_init(struct ice_pf *pf);
void ice_hwmon_exit(struct ice_pf *pf);

#endif /* _ICE_HWMON_H_ */
