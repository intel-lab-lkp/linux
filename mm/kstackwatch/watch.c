// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpuhotplug.h>
#include <linux/hw_breakpoint.h>
#include <linux/irqflags.h>
#include <linux/kstackwatch.h>
#include <linux/mutex.h>
#include <linux/printk.h>

static LLIST_HEAD(free_wp_list);
static LIST_HEAD(all_wp_list);
static DEFINE_MUTEX(all_wp_mutex);

static ulong holder;

static void ksw_watch_handler(struct perf_event *bp,
			      struct perf_sample_data *data,
			      struct pt_regs *regs)
{
	pr_err("========== KStackWatch: Caught stack corruption =======\n");
	pr_err("config %s\n", ksw_get_config()->user_input);
	dump_stack();
	pr_err("=================== KStackWatch End ===================\n");

	if (ksw_get_config()->panic_hit)
		panic("Stack corruption detected");
}

static void ksw_watch_on_local_cpu(void *info)
{
	struct ksw_watchpoint *wp = info;
	struct perf_event *bp;
	ulong flags;
	int cpu;
	int ret;

	local_irq_save(flags);
	cpu = raw_smp_processor_id();
	bp = per_cpu(*wp->event, cpu);
	if (!bp) {
		local_irq_restore(flags);
		return;
	}

	ret = modify_wide_hw_breakpoint_local(bp, &wp->attr);
	local_irq_restore(flags);
	WARN(ret, "fail to reinstall HWBP on CPU%d ret %d", cpu, ret);
}

static void ksw_watch_update(struct ksw_watchpoint *wp, ulong addr, u16 len)
{
	call_single_data_t *csd;
	int cur_cpu;
	int cpu;

	wp->attr.bp_addr = addr;
	wp->attr.bp_len = len;

	cur_cpu = raw_smp_processor_id();
	for_each_online_cpu(cpu) {
		/* remote cpu first */
		if (cpu == cur_cpu)
			continue;
		csd = per_cpu_ptr(wp->csd, cpu);
		smp_call_function_single_async(cpu, csd);
	}
	ksw_watch_on_local_cpu(wp);
}

int ksw_watch_get(struct ksw_watchpoint **out_wp)
{
	struct ksw_watchpoint *wp;
	struct llist_node *node;

	node = llist_del_first(&free_wp_list);
	if (!node)
		return -EBUSY;

	wp = llist_entry(node, struct ksw_watchpoint, node);
	WARN_ON_ONCE(wp->attr.bp_addr != (u64)&holder);

	*out_wp = wp;
	return 0;
}
int ksw_watch_on(struct ksw_watchpoint *wp, ulong watch_addr, u16 watch_len)
{
	ksw_watch_update(wp, watch_addr, watch_len);
	return 0;
}

int ksw_watch_off(struct ksw_watchpoint *wp)
{
	WARN_ON_ONCE(wp->attr.bp_addr == (u64)&holder);
	ksw_watch_update(wp, (ulong)&holder, sizeof(ulong));
	llist_add(&wp->node, &free_wp_list);
	return 0;
}

static int ksw_watch_alloc(void)
{
	int max_watch = ksw_get_config()->max_watch;
	struct ksw_watchpoint *wp;
	call_single_data_t *csd;
	int success = 0;
	int cpu;
	int ret;

	init_llist_head(&free_wp_list);

	//max_watch=0 means at most
	while (!max_watch || success < max_watch) {
		wp = kzalloc(sizeof(*wp), GFP_KERNEL);
		if (!wp)
			return success > 0 ? success : -EINVAL;
		wp->csd = alloc_percpu(call_single_data_t);
		if (!wp->csd) {
			kfree(wp);
			return success > 0 ? success : -EINVAL;
		}

		for_each_possible_cpu(cpu) {
			csd = per_cpu_ptr(wp->csd, cpu);
			INIT_CSD(csd, ksw_watch_on_local_cpu, wp);
		}

		hw_breakpoint_init(&wp->attr);
		wp->attr.bp_addr = (ulong)&holder;
		wp->attr.bp_len = sizeof(ulong);
		wp->attr.bp_type = HW_BREAKPOINT_W;
		wp->event = register_wide_hw_breakpoint(&wp->attr,
							ksw_watch_handler, wp);
		if (IS_ERR((void *)wp->event)) {
			ret = PTR_ERR((void *)wp->event);
			free_percpu(wp->csd);
			kfree(wp);
			return success > 0 ? success : ret;
		}
		llist_add(&wp->node, &free_wp_list);
		mutex_lock(&all_wp_mutex);
		list_add(&wp->list, &all_wp_list);
		mutex_unlock(&all_wp_mutex);
		success++;
	}

	return success;
}

static void ksw_watch_free(void)
{
	struct ksw_watchpoint *wp, *tmp;

	mutex_lock(&all_wp_mutex);
	list_for_each_entry_safe(wp, tmp, &all_wp_list, list) {
		list_del(&wp->list);
		unregister_wide_hw_breakpoint(wp->event);
		free_percpu(wp->csd);
		kfree(wp);
	}
	mutex_unlock(&all_wp_mutex);
}

int ksw_watch_init(void)
{
	int ret;

	ret = ksw_watch_alloc();
	if (ret <= 0)
		return -EBUSY;

	return 0;
}

void ksw_watch_exit(void)
{
	ksw_watch_free();
}
