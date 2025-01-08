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
#include <linux/lsm_audit.h>
#include <linux/lsm_hooks.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <net/af_unix.h>
#include <net/sock.h>

#include "audit.h"
#include "common.h"
#include "cred.h"
#include "domain.h"
#include "fs.h"
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

	/* Quick return for non-landlocked tasks. */
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

static int domain_ptrace(const struct landlock_ruleset *const parent,
			 const struct landlock_ruleset *const child)
{
	if (domain_scope_le(parent, child))
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
	const struct landlock_ruleset *parent_dom, *child_dom;
	struct landlock_request request = {
		.type = LANDLOCK_REQUEST_PTRACE,
		.audit = {
			.type = LSM_AUDIT_DATA_TASK,
			.u.tsk = child,
		},
	};
	int err;

	/* Quick return for non-landlocked tasks. */
	parent_dom = landlock_get_current_domain();
	if (!parent_dom)
		return 0;

	rcu_read_lock();
	child_dom = landlock_get_task_domain(child);
	err = domain_ptrace(parent_dom, child_dom);
	rcu_read_unlock();

	/*
	 * For the ptrace_access_check case, we log the current/parent domain
	 * and the child task.
	 */
	if (err && !(mode & PTRACE_MODE_NOAUDIT)) {
		request.layer_plus_one = parent_dom->num_layers;
		landlock_log_denial(parent_dom, &request);
	}

	return err;
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
	const struct landlock_ruleset *parent_dom, *child_dom;
	struct landlock_request request = {
		.type = LANDLOCK_REQUEST_PTRACE,
		.audit = {
			.type = LSM_AUDIT_DATA_TASK,
			.u.tsk = parent,
		},
	};
	int err;

	child_dom = landlock_get_current_domain();
	rcu_read_lock();
	parent_dom = landlock_get_task_domain(parent);
	err = domain_ptrace(parent_dom, child_dom);

	/*
	 * For the ptrace_traceme case, we log the domain which is the cause of
	 * the denial, which means the parent domain instead of the current
	 * domain.  This may look weird because the ptrace_traceme action is a
	 * request to be traced, but the semantic is consistent with
	 * hook_ptrace_access_check().
	 */
	if (err) {
		request.layer_plus_one = parent_dom->num_layers;
		landlock_log_denial(parent_dom, &request);
	}

	rcu_read_unlock();
	return err;
}

/**
 * domain_is_scoped - Checks if the client domain is scoped in the same
 *		      domain as the server.
 *
 * @client: IPC sender domain.
 * @server: IPC receiver domain.
 * @scope: The scope restriction criteria.
 *
 * Returns: True if the @client domain is scoped to access the @server,
 * unless the @server is also scoped in the same domain as @client.
 */
static bool domain_is_scoped(const struct landlock_ruleset *const client,
			     const struct landlock_ruleset *const server,
			     access_mask_t scope)
{
	int client_layer, server_layer;
	const struct landlock_hierarchy *client_walker, *server_walker;

	/* Quick return if client has no domain */
	if (WARN_ON_ONCE(!client))
		return false;

	client_layer = client->num_layers - 1;
	client_walker = client->hierarchy;
	/*
	 * client_layer must be a signed integer with greater capacity
	 * than client->num_layers to ensure the following loop stops.
	 */
	BUILD_BUG_ON(sizeof(client_layer) > sizeof(client->num_layers));

	server_layer = server ? (server->num_layers - 1) : -1;
	server_walker = server ? server->hierarchy : NULL;

	/*
	 * Walks client's parent domains down to the same hierarchy level
	 * as the server's domain, and checks that none of these client's
	 * parent domains are scoped.
	 */
	for (; client_layer > server_layer; client_layer--) {
		if (landlock_get_scope_mask(client, client_layer) & scope)
			return true;

		client_walker = client_walker->parent;
	}
	/*
	 * Walks server's parent domains down to the same hierarchy level as
	 * the client's domain.
	 */
	for (; server_layer > client_layer; server_layer--)
		server_walker = server_walker->parent;

	for (; client_layer >= 0; client_layer--) {
		if (landlock_get_scope_mask(client, client_layer) & scope) {
			/*
			 * Client and server are at the same level in the
			 * hierarchy. If the client is scoped, the request is
			 * only allowed if this domain is also a server's
			 * ancestor.
			 */
			return server_walker != client_walker;
		}
		client_walker = client_walker->parent;
		server_walker = server_walker->parent;
	}
	return false;
}

static bool sock_is_scoped(struct sock *const other,
			   const struct landlock_ruleset *const domain)
{
	const struct landlock_ruleset *dom_other;

	/* The credentials will not change. */
	lockdep_assert_held(&unix_sk(other)->lock);
	dom_other = landlock_cred(other->sk_socket->file->f_cred)->domain;
	return domain_is_scoped(domain, dom_other,
				LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET);
}

static bool is_abstract_socket(struct sock *const sock)
{
	struct unix_address *addr = unix_sk(sock)->addr;

	if (!addr)
		return false;

	if (addr->len >= offsetof(struct sockaddr_un, sun_path) + 1 &&
	    addr->name->sun_path[0] == '\0')
		return true;

	return false;
}

static const struct access_masks unix_scope = {
	.scope = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET,
};

static int hook_unix_stream_connect(struct sock *const sock,
				    struct sock *const other,
				    struct sock *const newsk)
{
	const struct landlock_ruleset *const dom =
		landlock_get_applicable_domain(landlock_get_current_domain(),
					       unix_scope);
	struct lsm_network_audit audit_net = {
		.sk = other,
	};
	struct landlock_request request = {
		.type = LANDLOCK_REQUEST_SCOPE_ABSTRACT_UNIX_SOCKET,
		.audit = {
			.type = LSM_AUDIT_DATA_NET,
			.u.net = &audit_net,
		},
	};

	/* Quick return for non-landlocked tasks. */
	if (!dom)
		return 0;

	if (is_abstract_socket(other) && sock_is_scoped(other, dom)) {
		request.layer_plus_one =
			landlock_match_layer_level(dom, unix_scope) + 1;
		landlock_log_denial(dom, &request);
		return -EPERM;
	}

	return 0;
}

static int hook_unix_may_send(struct socket *const sock,
			      struct socket *const other)
{
	const struct landlock_ruleset *const dom =
		landlock_get_applicable_domain(landlock_get_current_domain(),
					       unix_scope);
	struct lsm_network_audit audit_net = {
		.sk = other->sk,
	};
	struct landlock_request request = {
		.type = LANDLOCK_REQUEST_SCOPE_ABSTRACT_UNIX_SOCKET,
		.audit = {
			.type = LSM_AUDIT_DATA_NET,
			.u.net = &audit_net,
		},
	};

	if (!dom)
		return 0;

	/*
	 * Checks if this datagram socket was already allowed to be connected
	 * to other.
	 */
	if (unix_peer(sock->sk) == other->sk)
		return 0;

	if (is_abstract_socket(other->sk) && sock_is_scoped(other->sk, dom)) {
		request.layer_plus_one =
			landlock_match_layer_level(dom, unix_scope) + 1;
		landlock_log_denial(dom, &request);
		return -EPERM;
	}

	return 0;
}

static const struct access_masks signal_scope = {
	.scope = LANDLOCK_SCOPE_SIGNAL,
};

static int hook_task_kill(struct task_struct *const p,
			  struct kernel_siginfo *const info, const int sig,
			  const struct cred *const cred)
{
	bool is_scoped;
	const struct landlock_ruleset *dom;
	struct landlock_request request = {
		.type = LANDLOCK_REQUEST_SCOPE_SIGNAL,
		.audit = {
			.type = LSM_AUDIT_DATA_TASK,
			.u.tsk = p,
		},
	};

	if (cred) {
		/* Dealing with USB IO. */
		dom = landlock_cred(cred)->domain;
	} else {
		dom = landlock_get_current_domain();
	}
	dom = landlock_get_applicable_domain(dom, signal_scope);

	/* Quick return for non-landlocked tasks. */
	if (!dom)
		return 0;

	rcu_read_lock();
	is_scoped = domain_is_scoped(dom, landlock_get_task_domain(p),
				     LANDLOCK_SCOPE_SIGNAL);
	rcu_read_unlock();
	if (is_scoped) {
		request.layer_plus_one =
			landlock_match_layer_level(dom, signal_scope) + 1;
		landlock_log_denial(dom, &request);
		return -EPERM;
	}

	return 0;
}

static int hook_file_send_sigiotask(struct task_struct *tsk,
				    struct fown_struct *fown, int signum)
{
	const struct landlock_ruleset *dom;
	struct landlock_request request = {
		.type = LANDLOCK_REQUEST_SCOPE_SIGNAL,
		.audit = {
			.type = LSM_AUDIT_DATA_TASK,
			.u.tsk = tsk,
		},
	};
	bool is_scoped = false;

	/* Lock already held by send_sigio() and send_sigurg(). */
	lockdep_assert_held(&fown->lock);
	dom = landlock_get_applicable_domain(
		landlock_file(fown->file)->fown_domain, signal_scope);

	/* Quick return for unowned socket. */
	if (!dom)
		return 0;

	rcu_read_lock();
	is_scoped = domain_is_scoped(dom, landlock_get_task_domain(tsk),
				     LANDLOCK_SCOPE_SIGNAL);
	rcu_read_unlock();
	if (is_scoped) {
		request.layer_plus_one =
			landlock_match_layer_level(dom, signal_scope) + 1;
		landlock_log_denial(dom, &request);
		return -EPERM;
	}

	return 0;
}

static struct security_hook_list landlock_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(ptrace_access_check, hook_ptrace_access_check),
	LSM_HOOK_INIT(ptrace_traceme, hook_ptrace_traceme),

	LSM_HOOK_INIT(unix_stream_connect, hook_unix_stream_connect),
	LSM_HOOK_INIT(unix_may_send, hook_unix_may_send),

	LSM_HOOK_INIT(task_kill, hook_task_kill),
	LSM_HOOK_INIT(file_send_sigiotask, hook_file_send_sigiotask),
};

__init void landlock_add_task_hooks(void)
{
	security_add_hooks(landlock_hooks, ARRAY_SIZE(landlock_hooks),
			   &landlock_lsmid);
}
