/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Komeda top-level driver structure
 *
 * (C) COPYRIGHT 2025 Rahul Kumar <rk0006818@gmail.com>
 */
#ifndef _KOMEDA_DRV_H_
#define _KOMEDA_DRV_H_

#include "komeda_dev.h"
#include "komeda_kms.h"

/**
 * struct komeda_drv - Komeda high-level driver glue
 *
 * This structure links the core Komeda hardware device (struct komeda_dev)
 * with the DRM/KMS integration layer (struct komeda_kms_dev).
 */
struct komeda_drv {
	struct komeda_dev     *mdev;
	struct komeda_kms_dev *kms;
};

#endif /* !_KOMEDA_DRV_H_ */
