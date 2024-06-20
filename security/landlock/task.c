// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Ptrace hooks
 *
 * Copyright © 2017-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2019-2020 ANSSI
 */

#include <asm/current.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/lsm_hooks.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <net/sock.h>
#include <net/af_unix.h>

#include "common.h"
#include "cred.h"
#include "ruleset.h"
#include "setup.h"
#include "task.h"

/**
 * domain_scope_le - Checks domain ordering for scoped ptrace
 *
 * @parent: Parent domain.
 * @child: Potential child of @parent.
 *
 * Checks if the @parent domain is less or equal to (i.e. an ancestor, which
 * means a subset of) the @child domain.
 */
static bool domain_scope_le(const struct landlock_ruleset *const parent,
			    const struct landlock_ruleset *const child)
{
	const struct landlock_hierarchy *walker;

	if (!parent)
		return true;
	if (!child)
		return false;
	for (walker = child->hierarchy; walker; walker = walker->parent) {
		if (walker == parent->hierarchy)
			/* @parent is in the scoped hierarchy of @child. */
			return true;
	}
	/* There is no relationship between @parent and @child. */
	return false;
}

static bool task_is_scoped(const struct task_struct *const parent,
			   const struct task_struct *const child)
{
	bool is_scoped;
	const struct landlock_ruleset *dom_parent, *dom_child;

	rcu_read_lock();
	dom_parent = landlock_get_task_domain(parent);
	dom_child = landlock_get_task_domain(child);
	is_scoped = domain_scope_le(dom_parent, dom_child);
	rcu_read_unlock();
	return is_scoped;
}

static int task_ptrace(const struct task_struct *const parent,
		       const struct task_struct *const child)
{
	/* Quick return for non-landlocked tasks. */
	if (!landlocked(parent))
		return 0;
	if (task_is_scoped(parent, child))
		return 0;
	return -EPERM;
}

/**
 * hook_ptrace_access_check - Determines whether the current process may access
 *			      another
 *
 * @child: Process to be accessed.
 * @mode: Mode of attachment.
 *
 * If the current task has Landlock rules, then the child must have at least
 * the same rules.  Else denied.
 *
 * Determines whether a process may access another, returning 0 if permission
 * granted, -errno if denied.
 */
static int hook_ptrace_access_check(struct task_struct *const child,
				    const unsigned int mode)
{
	return task_ptrace(current, child);
}

/**
 * hook_ptrace_traceme - Determines whether another process may trace the
 *			 current one
 *
 * @parent: Task proposed to be the tracer.
 *
 * If the parent has Landlock rules, then the current task must have the same
 * or more rules.  Else denied.
 *
 * Determines whether the nominated task is permitted to trace the current
 * process, returning 0 if permission is granted, -errno if denied.
 */
static int hook_ptrace_traceme(struct task_struct *const parent)
{
	return task_ptrace(parent, current);
}

static access_mask_t
get_scoped_accesses(const struct landlock_ruleset *const domain)
{
	access_mask_t access_dom = 0;
	size_t layer_level;

	for (layer_level = 0; layer_level < domain->num_layers; layer_level++)
		access_dom |= landlock_get_scope_mask(domain, layer_level);
	return access_dom;
}

/**
 * optional_domain_scope - Checks domain ordering for scoped unix sockets
 *
 * @client: client domain.
 * @server: Potential child of @client.
 *
 * Checks if the @client domain is less or equal to (i.e. an ancestor, which
 * means a subset of) the @server domain.
 * Same as domain_scope_le, only for optional scoping unix sockets.
 */
static bool optional_domain_scope(const struct landlock_ruleset *const client,
				  const struct landlock_ruleset *const server)
{
	const struct landlock_hierarchy *walker;
	access_mask_t scoped;

	if (!client)
		return true;

	/* quick return if server does not have domain */
	if (!server)
		return false;

	for (walker = server->hierarchy; walker; walker = walker->parent) {
		scoped = get_scoped_accesses(walker->curr_ruleset);
		if (walker == client->hierarchy)
			/* @client is in the scoped hierarchy of @server. */
			return true;
		if (scoped)
			/* There is a node between client and server that is scoped */
			return false;
	}
	/* There is no relationship between @parent and @child. */
	return false;
}

static bool sock_is_scoped(struct sock *const other)
{
	bool is_scoped = true;
	const struct landlock_ruleset *dom_other;
	const struct landlock_ruleset *const dom =
		landlock_get_current_domain();

	/* quick return if there is no domain or .scoped is not set */
	if (!dom || !get_scoped_accesses(dom))
		return true;

	/* the credentials will not change */
	lockdep_assert_held(&unix_sk(other)->lock);
	if (other->sk_type != SOCK_DGRAM) {
		dom_other = landlock_cred(other->sk_peer_cred)->domain;
	} else {
		dom_other =
			landlock_cred(other->sk_socket->file->f_cred)->domain;
	}
	is_scoped = optional_domain_scope(dom, dom_other);
	return is_scoped;
}

static int hook_unix_stream_connect(struct sock *const sock,
				    struct sock *const other,
				    struct sock *const newsk)
{
	if (sock_is_scoped(other))
		return 0;

	return -EPERM;
}

static int hook_unix_may_send(struct socket *const sock,
			      struct socket *const other)
{
	pr_warn("XXX %s:%d sock->file:%p other->file:%p\n", __func__, __LINE__,
		sock->file, other->file);
	if (sock_is_scoped(other->sk))
		return 0;

	return -EPERM;
}

static struct security_hook_list landlock_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(ptrace_access_check, hook_ptrace_access_check),
	LSM_HOOK_INIT(ptrace_traceme, hook_ptrace_traceme),
	LSM_HOOK_INIT(unix_stream_connect, hook_unix_stream_connect),
	LSM_HOOK_INIT(unix_may_send, hook_unix_may_send),
};

__init void landlock_add_task_hooks(void)
{
	security_add_hooks(landlock_hooks, ARRAY_SIZE(landlock_hooks),
			   &landlock_lsmid);
}
