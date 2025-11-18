// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Socket management and hooks
 *
 * Copyright © 2025 Huawei Tech. Co., Ltd.
 */

#include <linux/net.h>
#include <linux/socket.h>
#include <linux/stddef.h>
#include <net/ipv6.h>

#include "limits.h"
#include "ruleset.h"
#include "socket.h"
#include "cred.h"

#define TYPE_ALL (-1)
#define PROTOCOL_ALL (-1)

static int pack_socket_key(const s32 family, const s32 type, const s32 protocol,
			   uintptr_t *val)
{
	int err = -EINVAL;
	union {
		struct {
			u8 family;
			u8 type;
			u16 protocol;
		} __packed data;
		u32 packed;
	} socket_key;

	/* Checks that socket_key content can be stored in struct landlock_key. */
	BUILD_BUG_ON(sizeof(socket_key.data) > sizeof(socket_key.packed));
	BUILD_BUG_ON(sizeof(socket_key.packed) >
		     sizeof_field(union landlock_key, data));

	/*
	 * Checks that all supported protocol families and socket types can be
	 * stored in socket_key fields.
	 */
	BUILD_BUG_ON(AF_MAX - 1 > U8_MAX);
	BUILD_BUG_ON(SOCK_MAX - 1 > U8_MAX);

	/* Checks ranges and handles wildcard type and protocol value mapping. */
	if (family >= 0 && family < U8_MAX)
		socket_key.data.family = family;
	else
		goto out;

	BUILD_BUG_ON(TYPE_ALL != -1);
	if (type == TYPE_ALL)
		socket_key.data.type = U8_MAX;
	else if (type >= 0 && type < U8_MAX)
		socket_key.data.type = type;
	else
		goto out;

	BUILD_BUG_ON(PROTOCOL_ALL != -1);
	if (protocol == PROTOCOL_ALL)
		socket_key.data.protocol = U16_MAX;
	else if (protocol >= 0 && protocol < U16_MAX)
		socket_key.data.protocol = protocol;
	else
		goto out;

	*val = socket_key.packed;
	err = 0;
out:
	return err;
}

int landlock_append_socket_rule(struct landlock_ruleset *const ruleset,
				s32 family, s32 type, s32 protocol,
				access_mask_t access_rights)
{
	int err;
	uintptr_t key;
	/*
	 * (AF_INET, SOCK_PACKET) is an alias for (AF_PACKET, SOCK_PACKET)
	 * (cf. __sock_create).
	 */
	if (family == AF_INET && type == SOCK_PACKET)
		family = AF_PACKET;

	err = pack_socket_key(family, type, protocol, &key);
	if (err)
		return err;

	const struct landlock_id id = {
		.key.data = key,
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
