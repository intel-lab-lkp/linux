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

static int ksw_watch_alloc(void)
{
	int max_watch = ksw_get_config()->max_watch;
	struct ksw_watchpoint *wp;
	int success = 0;
	int ret;

	init_llist_head(&free_wp_list);

	//max_watch=0 means at most
	while (!max_watch || success < max_watch) {
		wp = kzalloc(sizeof(*wp), GFP_KERNEL);
		if (!wp)
			return success > 0 ? success : -EINVAL;

		hw_breakpoint_init(&wp->attr);
		wp->attr.bp_addr = (ulong)&holder;
		wp->attr.bp_len = sizeof(ulong);
		wp->attr.bp_type = HW_BREAKPOINT_W;
		wp->event = register_wide_hw_breakpoint(&wp->attr,
							ksw_watch_handler, wp);
		if (IS_ERR((void *)wp->event)) {
			ret = PTR_ERR((void *)wp->event);
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
