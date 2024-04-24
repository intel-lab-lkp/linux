/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_PARAMS_H
#define VIIF_PARAMS_H

struct viif_device;

void visconti_viif_params_eval_queue(struct viif_device *viif_dev);
void visconti_viif_params_isr(struct viif_device *viif_dev);
int visconti_viif_params_register(struct viif_device *viif_dev);
void visconti_viif_params_unregister(struct viif_device *viif_dev);

int visconti_viif_l2_undist_through(struct viif_device *viif_dev);
#endif /* VIIF_PARAMS_H */
