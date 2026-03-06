// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Red Hat, Inc. All Rights Reserved.
 *
 * Author: Coiby Xu <coxu@redhat.com>
 */
#ifndef _ASM_SECURE_BOOT_H
#define _ASM_SECURE_BOOT_H


#include <linux/types.h>

#ifdef CONFIG_EFI

/*
 * Default implementation.
 * Architectures that support secure boot must override this.
 *
 * Returns true if the platform secure boot is enabled.
 * Returns false if disabled or not supported.
 */
bool arch_get_secureboot(void);

#else

/*
 * Default implementation.
 * Architectures that support secure boot must override this.
 */
static inline bool arch_get_secureboot(void)
{
	return false;
}

#endif

#endif
