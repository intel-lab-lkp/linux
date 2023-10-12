/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_ISP_H
#define VIIF_ISP_H

struct viif_device;
struct viif_l2_roi_config;

void visconti_viif_l2_set_roi_path(struct viif_device *viif_dev);
void visconti_viif_l2_set_roi(struct viif_device *viif_dev, const struct viif_l2_roi_config *param);
int visconti_viif_isp_main_set_unit(struct viif_device *viif_dev);
int visconti_viif_isp_sub_set_unit(struct viif_device *viif_dev);
void visconti_viif_isp_set_compose_rect(struct viif_device *viif_dev,
					struct viif_l2_roi_config *roi);

int visconti_viif_isp_register(struct viif_device *viif_dev);
void visconti_viif_isp_unregister(struct viif_device *viif_dev);

#endif /* VIIF_ISP_H */
