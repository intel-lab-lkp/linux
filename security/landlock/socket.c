// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Socket management and hooks
 *
 * Copyright © 2024 Huawei Tech. Co., Ltd.
 */

#include <linux/net.h>
#include <net/sock.h>

#include "cred.h"
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

static access_mask_t
get_raw_handled_socket_accesses(const struct landlock_ruleset *const domain)
{
	access_mask_t access_dom = 0;
	size_t layer_level;

	for (layer_level = 0; layer_level < domain->num_layers; layer_level++)
		access_dom |= landlock_get_socket_access_mask(domain, layer_level);
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

static int current_check_access_socket(struct socket *const sock,
				       const access_mask_t access_request)
{
	union socket_key socket_key;
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

	socket_key.content.type = sock->type;
	socket_key.content.domain = sock->sk->__sk_common.skc_family;
	id.key.data = socket_key.val;

	rule = landlock_find_rule(dom, id);
	handled_access = landlock_init_layer_masks(
		dom, access_request, &layer_masks, LANDLOCK_KEY_SOCKET);
	if (landlock_unmask_layers(rule, handled_access, &layer_masks,
				   ARRAY_SIZE(layer_masks)))
		return 0;
	return -EACCES;
}

static int hook_socket_create(struct socket *const sock,
			    int family, int type, int protocol, int kern)
{
	return current_check_access_socket(sock, LANDLOCK_ACCESS_SOCKET_CREATE);
}

static struct security_hook_list landlock_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(socket_post_create, hook_socket_create),
};

__init void landlock_add_socket_hooks(void)
{
	security_add_hooks(landlock_hooks, ARRAY_SIZE(landlock_hooks),
			   &landlock_lsmid);
}
