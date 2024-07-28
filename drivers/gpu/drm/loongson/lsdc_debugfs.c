// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include <drm/drm_debugfs.h>

#include "loongson_drv.h"
#include "lsdc_benchmark.h"
#include "lsdc_drv.h"
#include "lsdc_gem.h"
#include "lsdc_probe.h"
#include "lsdc_ttm.h"

/* device level debugfs */

static int lsdc_identify(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	const struct loongson_gfx_desc *gfx = to_loongson_gfxinfo(ddev);
	u8 impl, rev;

	loongson_cpu_get_prid(&impl, &rev);

	seq_printf(m, "Running on cpu 0x%x, cpu revision: 0x%x\n",
		   impl, rev);

	seq_printf(m, "Contained in: %s\n", gfx->model);

	return 0;
}

static int lsdc_show_mm(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct drm_printer p = drm_seq_file_printer(m);

	drm_mm_print(&ddev->vma_offset_manager->vm_addr_space_mm, &p);

	return 0;
}

static int lsdc_show_gfxpll_clock(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct loongson_gfxpll *gfxpll = to_loongson_gfxpll(node->minor->dev);
	struct drm_printer printer = drm_seq_file_printer(m);

	gfxpll->funcs->print(gfxpll, &printer, true);

	return 0;
}

static int lsdc_show_benchmark(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct drm_printer printer = drm_seq_file_printer(m);

	lsdc_show_benchmark_copy(ddev, &printer);

	return 0;
}

static struct drm_info_list lsdc_debugfs_list[] = {
	{ "benchmark",   lsdc_show_benchmark, 0, NULL },
	{ "bos",         lsdc_show_buffer_object, 0, NULL },
	{ "chips",       lsdc_identify, 0, NULL },
	{ "clocks",      lsdc_show_gfxpll_clock, 0, NULL },
	{ "mm",          lsdc_show_mm, 0, NULL },
};

void loongson_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *ddev = minor->dev;
	unsigned int n = ARRAY_SIZE(lsdc_debugfs_list);
	unsigned int i;

	for (i = 0; i < n; ++i)
		lsdc_debugfs_list[i].data = to_loongson_drm(ddev);

	drm_debugfs_create_files(lsdc_debugfs_list, n, minor->debugfs_root, minor);

	lsdc_ttm_debugfs_init(ddev);
}
