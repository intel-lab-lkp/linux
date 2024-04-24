/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_STATS_H
#define VIIF_STATS_H

void visconti_viif_stats_isr(struct viif_device *viif_dev, unsigned int sequence, u64 timestamp);
int visconti_viif_stats_register(struct viif_device *viif_dev);
void visconti_viif_stats_unregister(struct viif_device *viif_dev);
#endif /* VIIF_STATS_H */
