// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Socket management and hooks
 *
 * Copyright © 2024 Huawei Tech. Co., Ltd.
 */

#include "limits.h"
#include "ruleset.h"
#include "socket.h"

union socket_key {
	struct {
		int domain;
		int type;
	} __packed content;
	u64 val;
};

int landlock_append_socket_rule(struct landlock_ruleset *const ruleset,
			     const int domain, const int type, access_mask_t access_rights)
{
	int err;
	const union socket_key socket_key = {
		.content.domain = domain,
		.content.type = type
	};

	const struct landlock_id id = {
		.key.data = socket_key.val,
		.type = LANDLOCK_KEY_SOCKET,
	};

	/* Transforms relative access rights to absolute ones. */
	access_rights |= LANDLOCK_MASK_ACCESS_SOCKET &
			 ~landlock_get_socket_access_mask(ruleset, 0);

	mutex_lock(&ruleset->lock);
	err = landlock_insert_rule(ruleset, id, access_rights);
	mutex_unlock(&ruleset->lock);

	return err;
}
