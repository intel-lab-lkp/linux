/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Socket management and hooks
 *
 * Copyright © 2024 Huawei Tech. Co., Ltd.
 */

#ifndef _SECURITY_LANDLOCK_SOCKET_H
#define _SECURITY_LANDLOCK_SOCKET_H

#include "ruleset.h"

__init void landlock_add_socket_hooks(void);

int landlock_append_socket_rule(struct landlock_ruleset *const ruleset,
				const int family, const int type,
				access_mask_t access_rights);

#endif /* _SECURITY_LANDLOCK_SOCKET_H */
