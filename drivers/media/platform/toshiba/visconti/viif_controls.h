/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_CONTROLS_H
#define VIIF_CONTROLS_H

struct viif_device;
struct viif_l2_undist;

int visconti_viif_l2_undist_through(struct viif_device *viif_dev);
int visconti_viif_isp_init_controls(struct viif_device *viif_dev);
void visconti_viif_save_l1_info(struct viif_device *viif_dev);

#endif /* VIIF_CONTROLS_H */
