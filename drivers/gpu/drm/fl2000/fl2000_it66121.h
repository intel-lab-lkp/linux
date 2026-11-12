/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2019, Artem Mygaiev
 */

#ifndef FL2000_IT66121_H
#define FL2000_IT66121_H

struct drm_bridge;
struct i2c_adapter;

struct drm_bridge *it66121_create(struct i2c_adapter *adapter);
void it66121_destroy(struct drm_bridge *bridge);
int it66121_hpd_connected(struct drm_bridge *bridge);
int it66121_ack_interrupts(struct drm_bridge *bridge);
void it66121_resume(struct drm_bridge *bridge);

#endif /* FL2000_IT66121_H */
