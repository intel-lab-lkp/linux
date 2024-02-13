/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2023, Intel Corporation. */

#ifndef _ICE_DEVLINK_PORT_H_
#define _ICE_DEVLINK_PORT_H_

int ice_devlink_create_pf_port(struct ice_pf *pf);
void ice_devlink_destroy_pf_port(struct ice_pf *pf);
int ice_devlink_create_vf_port(struct ice_vf *vf);
void ice_devlink_destroy_vf_port(struct ice_vf *vf);

#endif /* _ICE_DEVLINK_PORT_H_ */
