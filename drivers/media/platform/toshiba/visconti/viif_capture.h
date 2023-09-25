/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_CAPTURE_H
#define VIIF_CAPTURE_H

struct viif_device;
struct cap_dev;
struct viif_l2_roi_config;

int visconti_viif_capture_register_ctrl_handlers(struct viif_device *viif_dev);
void visconti_viif_capture_switch_buffer(struct cap_dev *cap_dev, u32 status_err,
					 u32 l2_transfer_status, u64 timestamp);

int visconti_viif_capture_register(struct viif_device *viif_dev);
void visconti_viif_capture_unregister(struct viif_device *viif_dev);

#endif /* VIIF_CAPTURE_H */
