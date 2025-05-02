// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Valve Corporation */

#include <linux/cgroup.h>
#include <linux/cgroup_drm.h>
#include <linux/slab.h>

struct drm_cgroup_state {
	struct cgroup_subsys_state css;
};

struct drm_root_cgroup_state {
	struct drm_cgroup_state drmcs;
};

static struct drm_root_cgroup_state root_drmcs;

static inline struct drm_cgroup_state *
css_to_drmcs(struct cgroup_subsys_state *css)
{
	return container_of(css, struct drm_cgroup_state, css);
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
	}

	return &drmcs->css;
}

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
