/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2019-2024 Intel Corporation. All rights reserved.
 */

#ifndef __XE_SPI_H__
#define __XE_SPI_H__

struct xe_device;

void xe_spi_init(struct xe_device *xe);

void xe_spi_fini(struct xe_device *xe);

#endif /* __XE_SPI_H__ */
