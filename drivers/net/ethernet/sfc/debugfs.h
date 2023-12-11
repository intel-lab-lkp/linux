/* SPDX-License-Identifier: GPL-2.0-only */
/****************************************************************************
 * Driver for Solarflare network controllers and boards
 * Copyright 2023, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#ifndef EFX_DEBUGFS_H
#define EFX_DEBUGFS_H
#include "net_driver.h"

#ifdef CONFIG_DEBUG_FS

/**
 * DOC: Directory layout for sfc debugfs
 *
 * At top level ([/sys/kernel]/debug/sfc) are per-netdev symlinks "nic_$name"
 * and the "cards" directory.  For each PCI device to which the driver has
 * bound and created a &struct efx_nic, there is a directory &efx_nic.debug_dir
 * in "cards" whose name is the PCI address of the device; it is to this
 * directory that the netdev symlink points.
 *
 * Under this directory, besides top-level parameter files, are:
 *
 * * "channels/" (&efx_nic.debug_channels_dir).  For each channel, this will
 *   contain a directory (&efx_channel.debug_dir), whose name is the channel
 *   index (in decimal).
 * * "queues/" (&efx_nic.debug_queues_dir).
 *
 *   * For each NIC RX queue, this will contain a directory
 *     (&efx_rx_queue.debug_dir), whose name is "rx-N" where N is the RX queue
 *     index.  (This may not be the same as the kernel core RX queue index.)
 *     The directory will contain a symlink to the owning channel.
 */

void efx_fini_debugfs_netdev(struct net_device *net_dev);
void efx_update_debugfs_netdev(struct efx_nic *efx);

int efx_init_debugfs_rx_queue(struct efx_rx_queue *rx_queue);
void efx_fini_debugfs_rx_queue(struct efx_rx_queue *rx_queue);

int efx_init_debugfs_channel(struct efx_channel *channel);
void efx_fini_debugfs_channel(struct efx_channel *channel);

int efx_init_debugfs_nic(struct efx_nic *efx);
void efx_fini_debugfs_nic(struct efx_nic *efx);

int efx_init_debugfs(void);
void efx_fini_debugfs(void);

#else /* CONFIG_DEBUG_FS */

static inline void efx_fini_debugfs_netdev(struct net_device *net_dev) {}

static inline void efx_update_debugfs_netdev(struct efx_nic *efx) {}

int efx_init_debugfs_rx_queue(struct efx_rx_queue *rx_queue)
{
	return 0;
}
void efx_fini_debugfs_rx_queue(struct efx_rx_queue *rx_queue) {}

int efx_init_debugfs_channel(struct efx_channel *channel)
{
	return 0;
}
void efx_fini_debugfs_channel(struct efx_channel *channel) {}

static inline int efx_init_debugfs_nic(struct efx_nic *efx)
{
	return 0;
}
static inline void efx_fini_debugfs_nic(struct efx_nic *efx) {}

static inline int efx_init_debugfs(void)
{
	return 0;
}
static inline void efx_fini_debugfs(void) {}

#endif /* CONFIG_DEBUG_FS */

#endif /* EFX_DEBUGFS_H */
