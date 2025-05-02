// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Valve Corporation */

#include <linux/cgroup.h>
#include <linux/cgroup_drm.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <drm/drm_drv.h>

struct drm_cgroup_state {
	struct cgroup_subsys_state css;

	struct list_head clients;
	unsigned int num_clients; /* Whole branch */

	unsigned int sum_children_weights;

	unsigned int weight;
	unsigned int effective_weight;
};

struct drm_root_cgroup_state {
	struct drm_cgroup_state drmcs;

	struct delayed_work notify_work;
};

static struct drm_root_cgroup_state root_drmcs = {
	.drmcs.clients = LIST_HEAD_INIT(root_drmcs.drmcs.clients),
};

static DEFINE_MUTEX(drmcg_mutex);

static inline struct drm_cgroup_state *
css_to_drmcs(struct cgroup_subsys_state *css)
{
	return container_of(css, struct drm_cgroup_state, css);
}

static void
drmcs_notify_weight(struct drm_cgroup_state *drmcs)
{
	struct drm_file *fpriv;

	lockdep_assert_held(&drmcg_mutex);

	list_for_each_entry(fpriv, &drmcs->clients, clink) {
		const struct drm_cgroup_ops *cg_ops =
			fpriv->minor->dev->driver->cg_ops;

		if (cg_ops && cg_ops->notify_weight)
			cg_ops->notify_weight(fpriv, drmcs->effective_weight);
	}
}

static void drmcg_update_weights_locked(void)
{
	lockdep_assert_held(&drmcg_mutex);

	mod_delayed_work(system_wq,
			 &root_drmcs.notify_work,
			 usecs_to_jiffies(1000));
}

static void drmcg_update_weights(void)
{
	mutex_lock(&drmcg_mutex);
	drmcg_update_weights_locked();
	mutex_unlock(&drmcg_mutex);
}

static u64
drmcs_read_weight(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct drm_cgroup_state *drmcs = css_to_drmcs(css);

	return drmcs->weight;
}

static int
drmcs_write_weight(struct cgroup_subsys_state *css, struct cftype *cftype,
		   u64 weight)
{
	struct drm_cgroup_state *drmcs = css_to_drmcs(css);
	int ret;

	if (weight < CGROUP_WEIGHT_MIN || weight > CGROUP_WEIGHT_MAX)
		return -ERANGE;

	ret = mutex_lock_interruptible(&drmcg_mutex);
	if (ret)
		return ret;
	drmcs->weight = weight;
	drmcg_update_weights_locked();
	mutex_unlock(&drmcg_mutex);

	return 0;
}

static void notify_worker(struct work_struct *work)
{
	struct drm_cgroup_state *root = &root_drmcs.drmcs;
	struct cgroup_subsys_state *node;
	bool updated;

	mutex_lock(&drmcg_mutex);
	rcu_read_lock();

	/*
	 * Always come back later if we race with core cgroup management.
	 */
	updated = false;
	if (WARN_ON_ONCE(!css_tryget_online(&root->css)))
		goto out_unlock;

	css_for_each_descendant_post(node, &root->css) {
		struct drm_cgroup_state *drmcs = css_to_drmcs(node);

		if (!css_tryget_online(node))
			goto out_put;

		drmcs->sum_children_weights = 0;
		css_put(node);
	}

	css_for_each_descendant_post(node, &root->css) {
		struct drm_cgroup_state *drmcs = css_to_drmcs(node);
		struct drm_cgroup_state *parent;

		if (!css_tryget_online(node))
			goto out_put;
		if (!node->parent || !drmcs->num_clients) {
			css_put(node);
			continue;
		}
		if (!css_tryget_online(node->parent)) {
			css_put(node);
			goto out_put;
		}

		parent = css_to_drmcs(node->parent);
		parent->sum_children_weights += drmcs->weight;
		css_put(node);
		css_put(&parent->css);
	}

	css_for_each_descendant_pre(node, &root->css) {
		struct drm_cgroup_state *drmcs = css_to_drmcs(node);
		struct cgroup_subsys_state *css;

		if (!css_tryget_online(node))
			goto out_put;
		if (!drmcs->num_clients) {
			css_put(node);
			continue;
		}

		css_for_each_child(css, &drmcs->css) {
			struct drm_cgroup_state *sibling = css_to_drmcs(css);

			if (!css_tryget_online(css)) {
				css_put(node);
				goto out_put;
			}
			if (!sibling->num_clients) {
				css_put(css);
				continue;
			}

			sibling->effective_weight =
				DIV_ROUND_CLOSEST(sibling->weight <<
						  DRM_CGROUP_WEIGHT_SHIFT,
						  drmcs->sum_children_weights);
			drmcs_notify_weight(sibling);
			css_put(css);
		}

		css_put(node);
	}

	updated = true;

out_put:
	css_put(&root->css);
out_unlock:
	rcu_read_unlock();

	if (!updated)
		drmcg_update_weights_locked();

	mutex_unlock(&drmcg_mutex);
}

static void drmcs_free(struct cgroup_subsys_state *css)
{
	if (css != &root_drmcs.drmcs.css)
		kfree(css_to_drmcs(css));
}

static struct cgroup_subsys_state *
drmcs_alloc(struct cgroup_subsys_state *parent_css)
{
	struct drm_cgroup_state *drmcs;

	if (!parent_css) {
		drmcs = &root_drmcs.drmcs;
		INIT_DELAYED_WORK(&root_drmcs.notify_work, notify_worker);
	} else {
		drmcs = kzalloc(sizeof(*drmcs), GFP_KERNEL);
		if (!drmcs)
			return ERR_PTR(-ENOMEM);

		INIT_LIST_HEAD(&drmcs->clients);
	}

	drmcs->weight = CGROUP_WEIGHT_DFL;
	drmcs->effective_weight = (1 << DRM_CGROUP_WEIGHT_SHIFT) / 2;

	return &drmcs->css;
}

static int drmcs_online(struct cgroup_subsys_state *css)
{
	drmcg_update_weights();

	return 0;
}

static void drmcs_offline(struct cgroup_subsys_state *css)
{
	drmcg_update_weights();
}

static struct drm_cgroup_state *old_drmcs;

static int drmcs_can_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *task;

	task = cgroup_taskset_first(tset, &css);
	old_drmcs = css_to_drmcs(task_css(task, drm_cgrp_id));

	return 0;
}

static void __inc_clients(struct drm_cgroup_state *drmcs)
{
	struct cgroup_subsys_state *parent = NULL;

	rcu_read_lock();
	do {
		drmcs->num_clients++;
		WARN_ON_ONCE(!drmcs->num_clients);

		if (parent)
			css_put(parent);

		parent = drmcs->css.parent;
		if (parent) {
			if (WARN_ON_ONCE(!css_tryget(parent)))
				break;

			drmcs = css_to_drmcs(parent);
		}
	} while (parent);
	rcu_read_unlock();
}

static void __dec_clients(struct drm_cgroup_state *drmcs)
{
	struct cgroup_subsys_state *parent = NULL;

	rcu_read_lock();
	do {
		WARN_ON_ONCE(!drmcs->num_clients);
		drmcs->num_clients--;

		if (parent)
			css_put(parent);

		parent = drmcs->css.parent;
		if (parent) {
			if (WARN_ON_ONCE(!css_tryget(parent)))
				break;

			drmcs = css_to_drmcs(parent);
		}
	} while (parent);
	rcu_read_unlock();
}

static void drmcs_attach(struct cgroup_taskset *tset)
{
	struct drm_cgroup_state *old = old_drmcs;
	struct cgroup_subsys_state *css;
	struct drm_file *fpriv, *next;
	struct drm_cgroup_state *new;
	struct task_struct *task;
	bool migrated = false;

	if (!old)
		return;

	task = cgroup_taskset_first(tset, &css);
	new = css_to_drmcs(task_css(task, drm_cgrp_id));
	if (new == old)
		return;

	mutex_lock(&drmcg_mutex);

	list_for_each_entry_safe(fpriv, next, &old->clients, clink) {
		cgroup_taskset_for_each(task, css, tset) {
			struct cgroup_subsys_state *old_css;
			struct drm_cgroup_state *old_;

			if (task->flags & PF_KTHREAD)
				continue;
			if (!thread_group_leader(task))
				continue;

			new = css_to_drmcs(task_css(task, drm_cgrp_id));
			if (WARN_ON_ONCE(new == old))
				continue;

			if (rcu_access_pointer(fpriv->pid) != task_tgid(task))
				continue;

			if (WARN_ON_ONCE(fpriv->__css != &old->css))
				continue;

			old_css = fpriv->__css;
			old_ = css_to_drmcs(old_css);
			fpriv->__css = &new->css;
			css_get(fpriv->__css);
			list_move_tail(&fpriv->clink, &new->clients);
			__dec_clients(old);
			__inc_clients(new);
			css_put(old_css);
			migrated = true;
		}
	}

	if (migrated)
		drmcg_update_weights_locked();

	mutex_unlock(&drmcg_mutex);

	old_drmcs = NULL;
}

static void drmcs_cancel_attach(struct cgroup_taskset *tset)
{
	old_drmcs = NULL;
}

void drmcgroup_client_open(struct drm_file *file_priv)
{
	struct drm_cgroup_state *drmcs;

	if (!file_priv->minor->dev->driver->cg_ops)
		return;

	drmcs = css_to_drmcs(task_get_css(current, drm_cgrp_id));

	mutex_lock(&drmcg_mutex);
	file_priv->__css = &drmcs->css; /* Keeps the reference. */
	list_add_tail(&file_priv->clink, &drmcs->clients);
	__inc_clients(drmcs);
	drmcg_update_weights_locked();
	mutex_unlock(&drmcg_mutex);
}
EXPORT_SYMBOL_GPL(drmcgroup_client_open);

void drmcgroup_client_close(struct drm_file *file_priv)
{
	struct drm_cgroup_state *drmcs;

	drmcs = css_to_drmcs(file_priv->__css);

	if (!file_priv->minor->dev->driver->cg_ops)
		return;

	mutex_lock(&drmcg_mutex);
	list_del(&file_priv->clink);
	__dec_clients(drmcs);
	file_priv->__css = NULL;
	drmcg_update_weights_locked();
	mutex_unlock(&drmcg_mutex);

	css_put(&drmcs->css);
}
EXPORT_SYMBOL_GPL(drmcgroup_client_close);

void drmcgroup_client_migrate(struct drm_file *file_priv)
{
	struct drm_cgroup_state *src, *dst;
	struct cgroup_subsys_state *old;

	if (!file_priv->minor->dev->driver->cg_ops)
		return;

	mutex_lock(&drmcg_mutex);

	old = file_priv->__css;
	src = css_to_drmcs(old);
	dst = css_to_drmcs(task_get_css(current, drm_cgrp_id));

	if (src != dst) {
		file_priv->__css = &dst->css; /* Keeps the reference. */
		list_move_tail(&file_priv->clink, &dst->clients);
		__dec_clients(src);
		__inc_clients(dst);
		drmcg_update_weights_locked();
	}

	mutex_unlock(&drmcg_mutex);

	css_put(old);
}
EXPORT_SYMBOL_GPL(drmcgroup_client_migrate);

struct cftype files[] = {
	{
		.name = "weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = drmcs_read_weight,
		.write_u64 = drmcs_write_weight,
	},
	{ } /* Zero entry terminates. */
};

struct cgroup_subsys drm_cgrp_subsys = {
	.css_alloc	= drmcs_alloc,
	.css_free	= drmcs_free,
	.css_online	= drmcs_online,
	.css_offline	= drmcs_offline,
	.can_attach     = drmcs_can_attach,
	.attach		= drmcs_attach,
	.cancel_attach  = drmcs_cancel_attach,
	.early_init	= false,
	.legacy_cftypes	= files,
	.dfl_cftypes	= files,
};
