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

static int check_socket_access(const struct landlock_ruleset *dom,
			       uintptr_t key,
			       layer_mask_t (*const layer_masks)[],
			       access_mask_t handled_access)
{
	const struct landlock_rule *rule;
	struct landlock_id id = {
		.type = LANDLOCK_KEY_SOCKET,
	};

	id.key.data = key;
	rule = landlock_find_rule(dom, id);
	if (landlock_unmask_layers(rule, handled_access, layer_masks,
				   LANDLOCK_NUM_ACCESS_SOCKET))
		return 0;
	return -EACCES;
}

static int hook_socket_create(int family, int type, int protocol, int kern)
{
	layer_mask_t layer_masks[LANDLOCK_NUM_ACCESS_SOCKET] = {};
	access_mask_t handled_access;
	const struct access_masks masks = {
		.socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_cred_security *const subject =
		landlock_get_applicable_subject(current_cred(), masks, NULL);
	uintptr_t key;

	if (!subject)
		return 0;
	/* Checks only user space sockets. */
	if (kern)
		return 0;

	handled_access = landlock_init_layer_masks(
		subject->domain, LANDLOCK_ACCESS_SOCKET_CREATE, &layer_masks,
		LANDLOCK_KEY_SOCKET);
	/*
	 * Error could happen due to parameters are outside of the allowed range,
	 * so this combination couldn't be added in ruleset previously.
	 * Therefore, it's not permitted.
	 */
	if (pack_socket_key(family, type, protocol, &key) == -EACCES)
		return -EACCES;
	if (check_socket_access(subject->domain, key, &layer_masks,
				handled_access) == 0)
		return 0;

	/* Ranges were already checked. */
	(void)pack_socket_key(family, TYPE_ALL, protocol, &key);
	if (check_socket_access(subject->domain, key, &layer_masks,
				handled_access) == 0)
		return 0;

	(void)pack_socket_key(family, type, PROTOCOL_ALL, &key);
	if (check_socket_access(subject->domain, key, &layer_masks,
				handled_access) == 0)
		return 0;

	(void)pack_socket_key(family, TYPE_ALL, PROTOCOL_ALL, &key);
	if (check_socket_access(subject->domain, key, &layer_masks,
				handled_access) == 0)
		return 0;

	return -EACCES;
}

static struct security_hook_list landlock_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(socket_create, hook_socket_create),
};

__init void landlock_add_socket_hooks(void)
{
	security_add_hooks(landlock_hooks, ARRAY_SIZE(landlock_hooks),
			   &landlock_lsmid);
}
