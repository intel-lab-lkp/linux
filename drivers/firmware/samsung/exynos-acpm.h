/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2020 Samsung Electronics Co., Ltd.
 * Copyright 2020 Google LLC.
 * Copyright 2024 Linaro Ltd.
 */
#ifndef __EXYNOS_ACPM_H__
#define __EXYNOS_ACPM_H__

struct acpm_handle;
struct acpm_xfer;

int acpm_do_xfer(const struct acpm_handle *handle, struct acpm_xfer *xfer);

#endif /* __EXYNOS_ACPM_H__ */
