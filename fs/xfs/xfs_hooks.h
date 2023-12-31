// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_HOOKS_H_
#define XFS_HOOKS_H_

#if defined(CONFIG_XFS_LIVE_HOOKS_SRCU)
struct xfs_hooks {
	struct srcu_notifier_head	head;
};
#elif defined(CONFIG_XFS_LIVE_HOOKS_BLOCKING)
struct xfs_hooks {
	struct blocking_notifier_head	head;
};
#else
struct xfs_hooks { /* empty */ };
#endif

/*
 * If hooks and jump labels are enabled, we use jump labels (aka patching of
 * the code segment) to avoid the minute overhead of calling an empty notifier
 * chain when we know there are no callers.  If hooks are enabled without jump
 * labels, hardwire the predicate to true because calling an empty srcu
 * notifier chain isn't so expensive.
 */
#if defined(CONFIG_JUMP_LABEL) && defined(CONFIG_XFS_LIVE_HOOKS)
# define DEFINE_STATIC_XFS_HOOK_SWITCH(name) \
	static DEFINE_STATIC_KEY_FALSE(name)
# define xfs_hooks_switch_on(name)	static_branch_inc(name)
# define xfs_hooks_switch_off(name)	static_branch_dec(name)
# define xfs_hooks_switched_on(name)	static_branch_unlikely(name)
#elif defined(CONFIG_XFS_LIVE_HOOKS)
# define DEFINE_STATIC_XFS_HOOK_SWITCH(name)
# define xfs_hooks_switch_on(name)	((void)0)
# define xfs_hooks_switch_off(name)	((void)0)
# define xfs_hooks_switched_on(name)	(true)
#else
# define DEFINE_STATIC_XFS_HOOK_SWITCH(name)
# define xfs_hooks_switch_on(name)	((void)0)
# define xfs_hooks_switch_off(name)	((void)0)
# define xfs_hooks_switched_on(name)	(false)
#endif /* JUMP_LABEL && XFS_LIVE_HOOKS */

#ifdef CONFIG_XFS_LIVE_HOOKS
struct xfs_hook {
	/* This must come at the start of the structure. */
	struct notifier_block		nb;
};

typedef	int (*xfs_hook_fn_t)(struct xfs_hook *hook, unsigned long action,
		void *data);

void xfs_hooks_init(struct xfs_hooks *chain);
int xfs_hooks_add(struct xfs_hooks *chain, struct xfs_hook *hook);
void xfs_hooks_del(struct xfs_hooks *chain, struct xfs_hook *hook);
int xfs_hooks_call(struct xfs_hooks *chain, unsigned long action,
		void *priv);

static inline void xfs_hook_setup(struct xfs_hook *hook, notifier_fn_t fn)
{
	hook->nb.notifier_call = fn;
	hook->nb.priority = 0;
}

#else
# define xfs_hooks_init(chain)			((void)0)
# define xfs_hooks_call(chain, val, priv)	(NOTIFY_DONE)
#endif

#endif /* XFS_HOOKS_H_ */
