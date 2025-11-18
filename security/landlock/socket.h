/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Socket management and hooks
 *
 * Copyright © 2025 Huawei Tech. Co., Ltd.
 */

#ifndef _SECURITY_LANDLOCK_SOCKET_H
#define _SECURITY_LANDLOCK_SOCKET_H

#include "ruleset.h"

int landlock_append_socket_rule(struct landlock_ruleset *const ruleset,
				const s32 family, const s32 type,
				const s32 protocol,
				access_mask_t access_rights);

#endif /* _SECURITY_LANDLOCK_SOCKET_H */
