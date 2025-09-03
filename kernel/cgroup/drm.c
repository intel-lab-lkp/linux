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
};

struct drm_root_cgroup_state {
	struct drm_cgroup_state drmcs;
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

static void __maybe_unused
drmcs_notify_weight(struct drm_cgroup_state *drmcs)
{
	struct drm_file *fpriv;

	lockdep_assert_held(&drmcg_mutex);

	list_for_each_entry(fpriv, &drmcs->clients, clink) {
		const struct drm_cgroup_ops *cg_ops =
			fpriv->minor->dev->driver->cg_ops;

		if (cg_ops && cg_ops->notify_weight)
			cg_ops->notify_weight(fpriv, 0);
	}
}

static void drmcs_free(struct cgroup_subsys_state *css)
{
	struct drm_cgroup_state *drmcs = css_to_drmcs(css);

	if (drmcs != &root_drmcs.drmcs)
		kfree(drmcs);
}

static struct cgroup_subsys_state *
drmcs_alloc(struct cgroup_subsys_state *parent_css)
{
	struct drm_cgroup_state *drmcs;

	if (!parent_css) {
		drmcs = &root_drmcs.drmcs;
	} else {
		drmcs = kzalloc(sizeof(*drmcs), GFP_KERNEL);
		if (!drmcs)
			return ERR_PTR(-ENOMEM);

		INIT_LIST_HEAD(&drmcs->clients);
	}

	return &drmcs->css;
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
	file_priv->__css = NULL;
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
	}

	mutex_unlock(&drmcg_mutex);

	css_put(old);
}
EXPORT_SYMBOL_GPL(drmcgroup_client_migrate);

struct cftype files[] = {
	{ } /* Zero entry terminates. */
};

struct cgroup_subsys drm_cgrp_subsys = {
	.css_alloc	= drmcs_alloc,
	.css_free	= drmcs_free,
	.early_init	= false,
	.legacy_cftypes	= files,
	.dfl_cftypes	= files,
};
