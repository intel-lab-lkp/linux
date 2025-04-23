/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#ifndef _IONIC_API_H_
#define _IONIC_API_H_

#include <linux/auxiliary_bus.h>

/**
 * struct ionic_aux_dev - Auxiliary device information
 * @handle:     Handle for this auxiliary device
 * @idx:        Index identifier
 * @adev:       Auxiliary device
 */
struct ionic_aux_dev {
	void *handle;
	int idx;
	struct auxiliary_device adev;
};

#endif /* _IONIC_API_H_ */
