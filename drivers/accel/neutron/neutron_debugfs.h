/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright 2025 NXP */

#ifndef __NEUTRON_DEBUGFS_H__
#define __NEUTRON_DEBUGFS_H__

struct neutron_device;

#if defined(CONFIG_DEBUG_FS)
void neutron_debugfs_init(struct neutron_device *ndev);
#else
static inline void neutron_debugfs_init(struct neutron_device *ndev) {}
#endif

#endif /* __NEUTRON_DEBUGFS_H__ */
