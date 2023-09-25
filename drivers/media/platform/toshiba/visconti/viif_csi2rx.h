/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_CSI2RX_H
#define VIIF_CSI2RX_H

struct viif_device;
struct viif_csi2rx_dphy_calibration_status;

int visconti_viif_csi2rx_get_calibration_status(
	struct viif_device *viif_dev,
	struct viif_csi2rx_dphy_calibration_status *calibration_status);
int visconti_viif_csi2rx_get_err_status(struct viif_device *viif_dev,
					struct viif_csi2rx_err_status *csi_err);
u32 visconti_viif_csi2rx_err_irq_handler(struct viif_device *viif_dev);

int visconti_viif_csi2rx_register(struct viif_device *viif_dev);
void visconti_viif_csi2rx_unregister(struct viif_device *viif_dev);

#endif /* VIIF_CSI2RX_H */
