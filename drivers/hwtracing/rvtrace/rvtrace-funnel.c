// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 SiFive Inc.
 *
 */

#include <linux/device.h>
#include <linux/rvtrace.h>
#include <linux/types.h>
#include "rvtrace-v0.h"

static int rvtrace_funnel_start(struct rvtrace_path_node *node)
{
	struct rvtrace_component *comp = node->comp;
	u32 comp_maj;
	int ret;

	/* Set pre-ratified comp's next sink */
	comp_maj = rvtrace_component_version_major(comp->id.version);
	if (comp_maj == 0) {
		ret = rvtrace_v0_sink_config(node);
		if (ret) {
			dev_err(&comp->dev, "failed to set next sink.\n");
			return ret;
		}
	} else if (comp_maj > 0) {
		return -EOPNOTSUPP;
	}

	ret = rvtrace_enable_component(comp->pdata);
	if (ret)
		return dev_err_probe(&comp->dev, ret, "failed to enable funnel.\n");

	return 0;
}

static int rvtrace_funnel_stop(struct rvtrace_component *comp)
{
	u32 comp_maj;
	int ret;

	comp_maj = rvtrace_component_version_major(comp->id.version);
	if (comp_maj > 0)
		return -EOPNOTSUPP;

	ret = rvtrace_disable_component(comp->pdata);
	if (ret)
		return dev_err_probe(&comp->dev, ret, "failed to disable funnel.\n");

	return rvtrace_comp_poll_empty(comp);
}

static int rvtrace_funnel_probe(struct rvtrace_component *comp)
{
	struct fwnode_handle *fwnode = dev_fwnode(comp->pdata->dev);
	int ret;

	ret = rvtrace_enable_component(comp->pdata);
	if (ret)
		return dev_err_probe(&comp->dev, ret, "failed to enable funnel.\n");

	dev_info(&comp->dev, "%s is available\n", fwnode_get_name(fwnode));

	return 0;
}

static void rvtrace_funnel_remove(struct rvtrace_component *comp)
{
	int ret;

	ret = rvtrace_disable_component(comp->pdata);
	if (ret)
		dev_err(&comp->dev, "failed to disable funnel.\n");
}

static struct rvtrace_component_id rvtrace_funnel_ids[] = {
	{ .type = RVTRACE_COMPONENT_TYPE_FUNNEL,
	  .version = rvtrace_component_mkversion(0, 0), },
	{},
};

static struct rvtrace_driver rvtrace_funnel_driver = {
	.id_table = rvtrace_funnel_ids,
	.start = rvtrace_funnel_start,
	.stop = rvtrace_funnel_stop,
	.probe = rvtrace_funnel_probe,
	.remove = rvtrace_funnel_remove,
	.driver = {
		.name = "rvtrace-funnel",
	},
};

static int __init rvtrace_funnel_init(void)
{
	return rvtrace_register_driver(&rvtrace_funnel_driver);
}

static void __exit rvtrace_funnel_exit(void)
{
	rvtrace_unregister_driver(&rvtrace_funnel_driver);
}

module_init(rvtrace_funnel_init);
module_exit(rvtrace_funnel_exit);

/* Module information */
MODULE_DESCRIPTION("RISC-V Trace Funnel Driver");
MODULE_LICENSE("GPL");
