// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Socket management and hooks
 *
 * Copyright © 2024 Huawei Tech. Co., Ltd.
 */

#include <linux/net.h>
#include <linux/socket.h>
#include <linux/stddef.h>
#include <net/ipv6.h>

#include "cred.h"
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

static access_mask_t
get_raw_handled_socket_accesses(const struct landlock_ruleset *const domain)
{
	access_mask_t access_dom = 0;
	size_t layer_level;

	for (layer_level = 0; layer_level < domain->num_layers; layer_level++)
		access_dom |=
			landlock_get_socket_access_mask(domain, layer_level);
	return access_dom;
}

static const struct landlock_ruleset *get_current_socket_domain(void)
{
	const struct landlock_ruleset *const dom =
		landlock_get_current_domain();

	if (!dom || !get_raw_handled_socket_accesses(dom))
		return NULL;

	return dom;
}

static int current_check_access_socket(struct socket *const sock, int family,
				       int type,
				       const access_mask_t access_request)
{
	layer_mask_t layer_masks[LANDLOCK_NUM_ACCESS_SOCKET] = {};
	const struct landlock_rule *rule;
	access_mask_t handled_access;
	struct landlock_id id = {
		.type = LANDLOCK_KEY_SOCKET,
	};
	const struct landlock_ruleset *const dom = get_current_socket_domain();

	if (!dom)
		return 0;
	if (WARN_ON_ONCE(dom->num_layers < 1))
		return -EACCES;

	id.key.data = pack_socket_key(family, type);

	rule = landlock_find_rule(dom, id);
	handled_access = landlock_init_layer_masks(
		dom, access_request, &layer_masks, LANDLOCK_KEY_SOCKET);
	if (landlock_unmask_layers(rule, handled_access, &layer_masks,
				   ARRAY_SIZE(layer_masks)))
		return 0;
	return -EACCES;
}

static int hook_socket_create(struct socket *const sock, int family, int type,
			      int protocol, int kern)
{
	return current_check_access_socket(sock, family, type,
					   LANDLOCK_ACCESS_SOCKET_CREATE);
}

static struct security_hook_list landlock_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(socket_post_create, hook_socket_create),
};

__init void landlock_add_socket_hooks(void)
{
	security_add_hooks(landlock_hooks, ARRAY_SIZE(landlock_hooks),
			   &landlock_lsmid);
}
