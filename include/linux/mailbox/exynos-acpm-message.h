/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2024 Linaro Ltd.
 */

#ifndef _LINUX_EXYNOS_ACPM_MESSAGE_H_
#define _LINUX_EXYNOS_ACPM_MESSAGE_H_

#include <linux/types.h>

/**
 * struct exynos_acpm_message - exynos ACPM mailbox message format.
 * @cmd: pointer to u32 command.
 * @len: length of the command.
 */
struct exynos_acpm_message {
	u32 *cmd;
	size_t len;
};

#endif /* _LINUX_EXYNOS_ACPM_MESSAGE_H_ */
