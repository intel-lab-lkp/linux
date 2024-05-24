// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Socket management and hooks
 *
 * Copyright © 2024 Huawei Tech. Co., Ltd.
 */

#include <linux/net.h>
#include <linux/socket.h>
#include <linux/stddef.h>

#include "limits.h"
#include "ruleset.h"
#include "socket.h"

static uintptr_t pack_socket_key(const int family, const int type)
{
	union {
		struct {
			unsigned short family, type;
		} __packed data;
		uintptr_t packed;
	} socket_key;

	/* Checks that all supported socket families and types can be stored in socket_key. */
	BUILD_BUG_ON(AF_MAX > (typeof(socket_key.data.family))~0);
	BUILD_BUG_ON(SOCK_MAX > (typeof(socket_key.data.type))~0);

	/* Checks that socket_key can be stored in landlock_key. */
	BUILD_BUG_ON(sizeof(socket_key.data) > sizeof(socket_key.packed));
	BUILD_BUG_ON(sizeof(socket_key.packed) >
		     sizeof_field(union landlock_key, data));

	socket_key.data.family = (unsigned short)family;
	socket_key.data.type = (unsigned short)type;

	return socket_key.packed;
}

int landlock_append_socket_rule(struct landlock_ruleset *const ruleset,
				const int family, const int type,
				access_mask_t access_rights)
{
	int err;

	const struct landlock_id id = {
		.key.data = pack_socket_key(family, type),
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
