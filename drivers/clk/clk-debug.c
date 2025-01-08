// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010-2011 Canonical Ltd <jeremy.kerr@canonical.com>
 * Copyright (C) 2011-2012 Linaro Ltd <mturquette@linaro.org>
 *
 * Debugfs functionality for the common clock framework.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/seq_file.h>

#include "clk-debug.h"

MODULE_IMPORT_NS("clk-debug");

static struct dentry *rootdir;

static void clk_summary_show_one(struct clk_hw *hw, int level, int next_level,
				 bool first, void *data)
{
	struct seq_file *s = data;
	int enable;
	int phase;
	struct clk *clk_user = NULL;
	int multi_node = 0;

	seq_printf(s, "%*s%-*s %-7d %-8d %-8d %-11lu %-10lu ",
		   level * 3 + 1, "",
		   35 - level * 3, clk_hw_get_name(hw),
		   clk_hw_enable_count(hw), clk_hw_prepare_count(hw),
		   clk_hw_protect_count(hw),
		   clk_hw_get_rate_recalc(hw),
		   clk_hw_get_accuracy_recalc(hw));

	phase = clk_hw_get_phase(hw);
	if (phase >= 0)
		seq_printf(s, "%-5d", phase);
	else
		seq_puts(s, "-----");

	seq_printf(s, " %-6d", clk_hw_get_scaled_duty_cycle(hw, 100000));

	enable = clk_hw_enable_state(hw);
	if (enable >= 0)
		seq_printf(s, " %5c ", enable ? 'Y' : 'N');
	else
		seq_printf(s, " %5c ", '?');

	while ((clk_user = clk_hw_next_consumer(hw, clk_user))) {
		seq_printf(s, "%*s%-*s  %-25s\n",
			   level * 3 + 2 + 105 * multi_node, "",
			   30,
			   clk_dev_id(clk_user) ? : "deviceless",
			   clk_con_id(clk_user) ? : "no_connection_id");

		multi_node = 1;
	}
}

static int clk_summary_show(struct seq_file *s, void *data)
{
	bool orphan_only = s->private;

	seq_puts(s, "                                 enable  prepare  protect                                duty  hardware                            connection\n");
	seq_puts(s, "   clock                          count    count    count        rate   accuracy phase  cycle    enable   consumer                         id\n");
	seq_puts(s, "---------------------------------------------------------------------------------------------------------------------------------------------\n");

	return clk_show_tree(clk_summary_show_one, s, orphan_only);
}
DEFINE_SHOW_ATTRIBUTE(clk_summary);

static void clk_dump_one(struct clk_hw *hw, int level, int next_level, bool first, void *data)
{
	struct seq_file *s = data;
	int phase;
	unsigned long min_rate, max_rate;

	clk_hw_get_rate_range(hw, &min_rate, &max_rate);

	if (!first)
		seq_putc(s, ',');

	/* This should be JSON format, i.e. elements separated with a comma */
	seq_printf(s, "\"%s\": { ", clk_hw_get_name(hw));
	seq_printf(s, "\"enable_count\": %d,", clk_hw_enable_count(hw));
	seq_printf(s, "\"prepare_count\": %d,", clk_hw_prepare_count(hw));
	seq_printf(s, "\"protect_count\": %d,", clk_hw_protect_count(hw));
	seq_printf(s, "\"rate\": %lu,", clk_hw_get_rate_recalc(hw));
	seq_printf(s, "\"min_rate\": %lu,", min_rate);
	seq_printf(s, "\"max_rate\": %lu,", max_rate);
	seq_printf(s, "\"accuracy\": %lu,", clk_hw_get_accuracy_recalc(hw));
	phase = clk_hw_get_phase(hw);
	if (phase >= 0)
		seq_printf(s, "\"phase\": %d,", phase);
	seq_printf(s, "\"duty_cycle\": %u",
		   clk_hw_get_scaled_duty_cycle(hw, 100000));

	while (level-- >= next_level)
		seq_putc(s, '}');
}

static int clk_dump_show(struct seq_file *s, void *data)
{
	bool orphan_only = s->private;

	seq_putc(s, '{');
	clk_show_tree(clk_dump_one, s, orphan_only);
	seq_puts(s, "}\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_dump);

/*
 * This can be dangerous, therefore don't provide any real compile time
 * configuration option for this feature.
 * People who want to use this will need to modify the source code directly.
 */
#undef CLOCK_ALLOW_WRITE_DEBUGFS
#ifdef CLOCK_ALLOW_WRITE_DEBUGFS
static int clk_rate_set(void *data, u64 val)
{
	struct clk_hw *hw = data;
	struct clk *clk = clk_hw_get_clk(hw, "debugfs_rate_set");
	int ret;

	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ret = clk_set_rate(clk, val);
	clk_put(clk);

	return ret;
}

#define clk_rate_mode	0644

static int clk_phase_set(void *data, u64 val)
{
	struct clk_hw *hw = data;
	struct clk *clk = clk_hw_get_clk(hw, "debugfs_phase_set");
	int degrees = do_div(val, 360);
	int ret;

	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ret = clk_set_phase(clk, degrees);
	clk_put(clk);

	return ret;
}

#define clk_phase_mode	0644

static int clk_prepare_enable_set(void *data, u64 val)
{
	struct clk_hw *hw = data;
	struct clk *clk = clk_hw_get_clk(hw, "debugfs_prepare_enable_set");
	int ret = 0;

	if (IS_ERR(clk))
		return PTR_ERR(clk);

	if (val)
		ret = clk_prepare_enable(clk);
	else
		clk_disable_unprepare(clk);
	clk_put(clk);

	return ret;
}

static int clk_prepare_enable_get(void *data, u64 *val)
{
	struct clk_hw *hw = data;

	*val = clk_hw_is_prepared(hw) && clk_hw_is_enabled(hw);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(clk_prepare_enable_fops, clk_prepare_enable_get,
			 clk_prepare_enable_set, "%llu\n");

#else
#define clk_rate_set	NULL
#define clk_rate_mode	0444

#define clk_phase_set	NULL
#define clk_phase_mode	0644
#endif

static int clk_rate_get(void *data, u64 *val)
{
	struct clk_hw *hw = data;
	struct clk *clk = clk_hw_get_clk(hw, "debugfs_rate_get");

	if (IS_ERR(clk))
		return PTR_ERR(clk);

	*val = clk_get_rate(clk);
	clk_put(clk);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(clk_rate_fops, clk_rate_get, clk_rate_set, "%llu\n");

static int clk_phase_get(void *data, u64 *val)
{
	struct clk_hw *hw = data;
	struct clk *clk = clk_hw_get_clk(hw, "debugfs_phase_get");

	if (IS_ERR(clk))
		return PTR_ERR(clk);

	*val = clk_get_phase(clk);
	clk_put(clk);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(clk_phase_fops, clk_phase_get, clk_phase_set, "%llu\n");

static const struct {
	unsigned long flag;
	const char *name;
} clk_flags[] = {
#define ENTRY(f) { f, #f }
	ENTRY(CLK_SET_RATE_GATE),
	ENTRY(CLK_SET_PARENT_GATE),
	ENTRY(CLK_SET_RATE_PARENT),
	ENTRY(CLK_IGNORE_UNUSED),
	ENTRY(CLK_GET_RATE_NOCACHE),
	ENTRY(CLK_SET_RATE_NO_REPARENT),
	ENTRY(CLK_GET_ACCURACY_NOCACHE),
	ENTRY(CLK_RECALC_NEW_RATES),
	ENTRY(CLK_SET_RATE_UNGATE),
	ENTRY(CLK_IS_CRITICAL),
	ENTRY(CLK_OPS_PARENT_ENABLE),
	ENTRY(CLK_DUTY_CYCLE_PARENT),
#undef ENTRY
};

static int clk_flags_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	unsigned long flags = clk_hw_get_flags(hw);
	unsigned int i;

	for (i = 0; flags && i < ARRAY_SIZE(clk_flags); i++) {
		if (flags & clk_flags[i].flag) {
			seq_printf(s, "%s\n", clk_flags[i].name);
			flags &= ~clk_flags[i].flag;
		}
	}
	if (flags) {
		/* Unknown flags */
		seq_printf(s, "0x%lx\n", flags);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_flags);

static int possible_parents_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	int i;

	for (i = 0; i < clk_hw_get_num_parents(hw) - 1; i++)
		clk_hw_show_parent_by_index(s, hw, i, ' ');

	clk_hw_show_parent_by_index(s, hw, i, '\n');

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(possible_parents);

static int current_parent_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	struct clk_hw *parent = clk_hw_get_parent(hw);

	if (parent)
		seq_printf(s, "%s\n", clk_hw_get_name(parent));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(current_parent);

#ifdef CLOCK_ALLOW_WRITE_DEBUGFS
static ssize_t current_parent_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct clk *clk, *parent;
	struct clk_hw *hw = s->private;
	struct clk_hw *parent_hw;
	u8 idx;
	int ret;

	ret = kstrtou8_from_user(ubuf, count, 0, &idx);
	if (ret < 0)
		return ret;

	parent_hw = clk_hw_get_parent_by_index(hw, idx);
	if (!parent_hw)
		return -ENOENT;

	clk = clk_hw_get_clk(hw, "debugfs_write");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	parent = clk_hw_get_clk(parent_hw, "debugfs_write");
	if (IS_ERR(parent)) {
		ret = PTR_ERR(parent);
		goto err;
	}

	ret = clk_set_parent(clk, parent);
	if (!ret)
		ret = count;

	clk_put(parent);
err:
	clk_put(clk);
	return ret;
}

static const struct file_operations current_parent_rw_fops = {
	.open		= current_parent_open,
	.write		= current_parent_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

static int clk_duty_cycle_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	struct clk_duty duty = { };

	clk_hw_get_duty(hw, &duty);

	seq_printf(s, "%u/%u\n", duty.num, duty.den);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_duty_cycle);

static int clk_min_rate_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	unsigned long min_rate, max_rate;

	clk_debug_get_rate_range(hw, &min_rate, &max_rate);
	seq_printf(s, "%lu\n", min_rate);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_min_rate);

static int clk_max_rate_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	unsigned long min_rate, max_rate;

	clk_debug_get_rate_range(hw, &min_rate, &max_rate);
	seq_printf(s, "%lu\n", max_rate);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_max_rate);

static int clk_accuracy_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	struct clk *clk = clk_hw_get_clk(hw, "debugfs_accuracy");
	unsigned long accuracy;

	if (IS_ERR(clk))
		return PTR_ERR(clk);

	accuracy = clk_get_accuracy(clk);
	seq_printf(s, "%lu\n", accuracy);
	clk_put(clk);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_accuracy);

static int clk_prepare_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;

	seq_printf(s, "%u\n", clk_hw_prepare_count(hw));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_prepare);

static int clk_enable_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;

	seq_printf(s, "%u\n", clk_hw_enable_count(hw));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_enable);

static int clk_protect_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;

	seq_printf(s, "%u\n", clk_hw_protect_count(hw));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_protect);

static int clk_notifier_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;

	seq_printf(s, "%u\n", clk_hw_notifier_count(hw));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(clk_notifier);

static struct dentry *clk_hw_debug_create_one(struct clk_hw *hw)
{
	struct dentry *root;

	root = debugfs_create_dir(clk_hw_get_name(hw), rootdir);

	debugfs_create_file("clk_rate", clk_rate_mode, root, hw, &clk_rate_fops);
	debugfs_create_file("clk_min_rate", 0444, root, hw, &clk_min_rate_fops);
	debugfs_create_file("clk_max_rate", 0444, root, hw, &clk_max_rate_fops);
	debugfs_create_file("clk_accuracy", 0444, root, hw, &clk_accuracy_fops);
	debugfs_create_file("clk_phase", clk_phase_mode, root, hw, &clk_phase_fops);
	debugfs_create_file("clk_flags", 0444, root, hw, &clk_flags_fops);
	debugfs_create_file("clk_prepare_count", 0444, root, hw, &clk_prepare_fops);
	debugfs_create_file("clk_enable_count", 0444, root, hw, &clk_enable_fops);
	debugfs_create_file("clk_protect_count", 0444, root, hw, &clk_protect_fops);
	debugfs_create_file("clk_notifier_count", 0444, root, hw, &clk_notifier_fops);
	debugfs_create_file("clk_duty_cycle", 0444, root, hw, &clk_duty_cycle_fops);
#ifdef CLOCK_ALLOW_WRITE_DEBUGFS
	debugfs_create_file("clk_prepare_enable", 0644, root, hw,
			    &clk_prepare_enable_fops);

	if (clk_hw_get_num_parents(hw) > 1)
		debugfs_create_file("clk_parent", 0644, root, hw,
				    &current_parent_rw_fops);
	else
#endif
	if (clk_hw_get_num_parents(hw) > 0)
		debugfs_create_file("clk_parent", 0444, root, hw,
				    &current_parent_fops);

	if (clk_hw_get_num_parents(hw) > 1)
		debugfs_create_file("clk_possible_parents", 0444, root, hw,
				    &possible_parents_fops);

	return root;
}

/**
 * clk_debug_init - lazily populate the debugfs clk directory
 *
 * clks are often initialized very early during boot before memory can be
 * dynamically allocated and well before debugfs is setup. This function
 * populates the debugfs clk directory once at boot-time when we know that
 * debugfs is setup. It should only be called once at boot-time, all other clks
 * added dynamically will be done so with clk_debug_register.
 */
static int __init clk_debug_init(void)
{
#ifdef CLOCK_ALLOW_WRITE_DEBUGFS
	pr_warn("\n");
	pr_warn("********************************************************************\n");
	pr_warn("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE           **\n");
	pr_warn("**                                                                **\n");
	pr_warn("**  WRITEABLE clk DebugFS SUPPORT HAS BEEN ENABLED IN THIS KERNEL **\n");
	pr_warn("**                                                                **\n");
	pr_warn("** This means that this kernel is built to expose clk operations  **\n");
	pr_warn("** such as parent or rate setting, enabling, disabling, etc.      **\n");
	pr_warn("** to userspace, which may compromise security on your system.    **\n");
	pr_warn("**                                                                **\n");
	pr_warn("** If you see this message and you are not debugging the          **\n");
	pr_warn("** kernel, report this immediately to your vendor!                **\n");
	pr_warn("**                                                                **\n");
	pr_warn("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE           **\n");
	pr_warn("********************************************************************\n");
#endif

	rootdir = debugfs_create_dir("clk", NULL);

	debugfs_create_file("clk_summary", 0444, rootdir, (void *)0UL,
			    &clk_summary_fops);
	debugfs_create_file("clk_dump", 0444, rootdir, (void *)0UL,
			    &clk_dump_fops);
	debugfs_create_file("clk_orphan_summary", 0444, rootdir, (void *)1UL,
			    &clk_summary_fops);
	debugfs_create_file("clk_orphan_dump", 0444, rootdir, (void *)1UL,
			    &clk_dump_fops);

	clk_hw_debug_for_each_init(clk_hw_debug_create_one);

	return 0;
}
late_initcall(clk_debug_init);

static void __exit clk_debug_exit(void)
{
	clk_hw_debug_exit();
	debugfs_remove_recursive(rootdir);
}
module_exit(clk_debug_exit);

MODULE_LICENSE("GPL");
