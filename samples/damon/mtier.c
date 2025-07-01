// SPDX-License-Identifier: GPL-2.0
/*
 * memory tiering: migrate cold pages in node 0 and hot pages in node 1 to node
 * 1 and node 0, respectively.  Adjust the hotness/coldness threshold aiming
 * resulting 99.6 % node 0 utilization ratio.
 */

#define pr_fmt(fmt) "damon_sample_mtier: " fmt

#include <linux/damon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

static unsigned long node0_mem_used_bp __read_mostly = 9970;
module_param(node0_mem_used_bp, ulong, 0600);

static unsigned long node0_mem_free_bp __read_mostly = 50;
module_param(node0_mem_free_bp, ulong, 0600);

static int get_migrate_hot(
		char *val, const struct kernel_param *kp);

static const struct kernel_param_ops migrate_hot_ops = {
	.set = param_set_charp,
	.get = get_migrate_hot,
};

static char *migrate_hot __read_mostly = "";
module_param_cb(migrate_hot, &migrate_hot_ops, &migrate_hot, 0600);
MODULE_PARM_DESC(migrate_hot,
	"Specify source and destination node id as a comma-seperated pair");

static int get_migrate_cold(
		char *val, const struct kernel_param *kp);

static const struct kernel_param_ops migrate_cold_ops = {
	.set = param_set_charp,
	.get = get_migrate_cold,
};

static char *migrate_cold __read_mostly = "";
module_param_cb(migrate_cold, &migrate_cold_ops, &migrate_cold, 0600);
MODULE_PARM_DESC(migrate_cold,
	"Specify source and destination node id as a comma-seperated pair");

static int damon_sample_mtier_enable_store(
		const char *val, const struct kernel_param *kp);

static const struct kernel_param_ops enable_param_ops = {
	.set = damon_sample_mtier_enable_store,
	.get = param_get_bool,
};

static bool enable __read_mostly;
module_param_cb(enable, &enable_param_ops, &enable, 0600);
MODULE_PARM_DESC(enable, "Enable of disable DAMON_SAMPLE_MTIER");

static struct damon_ctx *ctxs[2];

struct region_range {
	phys_addr_t start;
	phys_addr_t end;
};

static int parse_migrate_node(int *src, int *dst, bool promote) {
	char *comma;
	char buf[32];

	if (promote)
		strscpy(buf, migrate_hot, sizeof(buf));
	else
		strscpy(buf, migrate_cold, sizeof(buf));

	comma = strchr(buf, ',');
	if (!comma)
		return -EINVAL;

	*comma = '\0';
	comma++;

	if (kstrtoint(buf, 0, src))
		return -EINVAL;
	if (kstrtoint(comma, 0, dst))
		return -EINVAL;

	return 0;
}

static int numa_info_init(int target_node, struct region_range *range) {

	if (!node_online(target_node)) {
		pr_err("NUMA node %d is not online\n", target_node);
		return -EINVAL;
	}

	/* TODO: Do we need to support more accurate region range?  */
	unsigned long start_pfn = node_start_pfn(target_node);
	unsigned long end_pfn   = node_end_pfn(target_node);

	range->start = (phys_addr_t)start_pfn << PAGE_SHIFT;
	range->end  = (phys_addr_t)end_pfn << PAGE_SHIFT;

	return 0;
}

static struct damon_ctx *damon_sample_mtier_build_ctx(bool promote)
{
	struct damon_ctx *ctx;
	struct damon_attrs attrs;
	struct damon_target *target;
	struct damon_region *region;
	struct damos *scheme;
	struct damos_quota_goal *quota_goal;
	struct damos_filter *filter;
	struct region_range addr;
	int src, dst;

	ctx = damon_new_ctx();
	if (!ctx)
		return NULL;
	attrs = (struct damon_attrs) {
		.sample_interval = 5 * USEC_PER_MSEC,
		.aggr_interval = 100 * USEC_PER_MSEC,
		.ops_update_interval = 60 * USEC_PER_MSEC * MSEC_PER_SEC,
		.min_nr_regions = 10,
		.max_nr_regions = 1000,
	};

	/*
	 * auto-tune sampling and aggregation interval aiming 4% DAMON-observed
	 * accesses ratio, keeping sampling interval in [5ms, 10s] range.
	 */
	attrs.intervals_goal = (struct damon_intervals_goal) {
		.access_bp = 400, .aggrs = 3,
		.min_sample_us = 5000, .max_sample_us = 10000000,
	};
	if (damon_set_attrs(ctx, &attrs))
		goto free_out;
	if (damon_select_ops(ctx, DAMON_OPS_PADDR))
		goto free_out;

	target = damon_new_target();
	if (!target)
		goto free_out;
	damon_add_target(ctx, target);

	if (parse_migrate_node(&src, &dst, promote))
		goto free_out;

	if (numa_info_init(src, &addr))
		goto free_out;

	region = damon_new_region(addr.start, addr.end);
	if (!region)
		goto free_out;
	damon_add_region(region, target);

	scheme = damon_new_scheme(
			/* access pattern */
			&(struct damos_access_pattern) {
				.min_sz_region = PAGE_SIZE,
				.max_sz_region = ULONG_MAX,
				.min_nr_accesses = promote ? 1 : 0,
				.max_nr_accesses = promote ? UINT_MAX : 0,
				.min_age_region = 0,
				.max_age_region = UINT_MAX},
			/* action */
			promote ? DAMOS_MIGRATE_HOT : DAMOS_MIGRATE_COLD,
			1000000,	/* apply interval (1s) */
			&(struct damos_quota){
				/* 200 MiB per sec by most */
				.reset_interval = 1000,
				.sz = 200 * 1024 * 1024,
				/* ignore size of region when prioritizing */
				.weight_sz = 0,
				.weight_nr_accesses = 100,
				.weight_age = 100,
			},
			&(struct damos_watermarks){},
			promote ? 0 : 1);	/* migrate target node id */
	if (!scheme)
		goto free_out;
	damon_set_schemes(ctx, &scheme, 1);
	quota_goal = damos_new_quota_goal(
			promote ? DAMOS_QUOTA_NODE_MEM_USED_BP :
			DAMOS_QUOTA_NODE_MEM_FREE_BP,
			promote ? node0_mem_used_bp : node0_mem_free_bp);
	if (!quota_goal)
		goto free_out;
	quota_goal->nid = 0;
	damos_add_quota_goal(&scheme->quota, quota_goal);
	filter = damos_new_filter(DAMOS_FILTER_TYPE_YOUNG, true, promote);
	if (!filter)
		goto free_out;
	damos_add_filter(scheme, filter);
	return ctx;
free_out:
	damon_destroy_ctx(ctx);
	return NULL;
}

static int damon_sample_mtier_start(void)
{
	struct damon_ctx *ctx;

	ctx = damon_sample_mtier_build_ctx(true);
	if (!ctx)
		return -ENOMEM;
	ctxs[0] = ctx;
	ctx = damon_sample_mtier_build_ctx(false);
	if (!ctx) {
		damon_destroy_ctx(ctxs[0]);
		return -ENOMEM;
	}
	ctxs[1] = ctx;
	return damon_start(ctxs, 2, true);
}

static void damon_sample_mtier_stop(void)
{
	damon_stop(ctxs, 2);
	damon_destroy_ctx(ctxs[0]);
	damon_destroy_ctx(ctxs[1]);
}

static int get_migrate_hot(char *buf, const struct kernel_param *kp)
{
       return scnprintf(buf, PAGE_SIZE, "%s", migrate_hot);
}

static int get_migrate_cold(char *buf, const struct kernel_param *kp)
{
       return scnprintf(buf, PAGE_SIZE, "%s", migrate_cold);
}

static int damon_sample_mtier_enable_store(
		const char *val, const struct kernel_param *kp)
{
	bool enabled = enable;
	int err;

	err = kstrtobool(val, &enable);
	if (err)
		return err;

	if (enable == enabled)
		return 0;

	if (enable)
		return damon_sample_mtier_start();
	damon_sample_mtier_stop();
	return 0;
}

static int __init damon_sample_mtier_init(void)
{
	return 0;
}

module_init(damon_sample_mtier_init);
