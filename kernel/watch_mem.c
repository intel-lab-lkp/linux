// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/ftrace.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/sys_info.h>

/* Time interval for SW watchpoin timer (default 100 ms). Setting to 0 will disable the timer */
static unsigned long watch_interval_ms = 100;
module_param(watch_interval_ms, ulong, 0644);

static unsigned long paddr_dram_to_watch;
module_param(paddr_dram_to_watch, ulong, 0644);
MODULE_PARM_DESC(paddr_dram_to_watch, "Physical DRAM address to watch");
static unsigned long *vaddr_dram;

static unsigned long target_dram_val;
module_param(target_dram_val, ulong, 0644);
MODULE_PARM_DESC(target_dram_val, "Target DRAM value to trigger watchpoint");

/* The MMIO address should be 32b aligned */
static unsigned long paddr_mmio_to_watch;
module_param(paddr_mmio_to_watch, ulong, 0644);
MODULE_PARM_DESC(paddr_mmio_to_watch, "Physical MMIO address to watch (32bit aligned)");
static void __iomem *vaddr_mmio;

static unsigned int target_mmio_val;
module_param(target_mmio_val, uint, 0644);
MODULE_PARM_DESC(target_mmio_val, "Target MMIO value to trigger watchpoint");

static bool panic_on_hit;
module_param(panic_on_hit, bool, 0644);

static bool hang_on_hit;
module_param(hang_on_hit, bool, 0644);

/* Stop the watchpoint after first hit */
static bool check_once = true;
module_param(check_once, bool, 0644);
MODULE_PARM_DESC(check_once, "Stop watching after first hit");

static struct timer_list sw_watchpoint_timer;

static void sw_watchpoint_timer_fn(struct timer_list *unused)
{
	bool hit = false;

	if (vaddr_mmio && (readl(vaddr_mmio) == target_mmio_val)) {
		pr_info("MMIO [@0x%lx] hit the target value [0x%x]!\n",
			paddr_mmio_to_watch, target_mmio_val);
		hit = true;
	}

	/*
	 * memremap for a RAM address could return a linear mapping address,
	 * and should be unpoisoned before accessing to avoid KASAN warnings.
	 */
	if (vaddr_dram)
		kasan_unpoison_pages(phys_to_page(paddr_dram_to_watch), 0, false);

	if (vaddr_dram && (READ_ONCE(*vaddr_dram) == target_dram_val)) {
		pr_info("DRAM [@0x%lx] hit the target value [0x%lx]!\n",
			paddr_dram_to_watch, target_dram_val);
		hit = true;
	}

	if (hit) {
		sys_info(0);

		/*
		 * Useful for stopping the OS (together with configuring system
		 * to UP with cmdline parameter 'nr_cpus=1') and waiting for HW
		 * debugger being attached.
		 */
		if (hang_on_hit) {
			pr_warn("Will dead loop on this CPU\n");
			while (1);
		}

		/* Could be used to trigger kexec/kdump */
		if (panic_on_hit)
			panic("SW watchpoint hit!");

		if (check_once)
			return;
	}

	mod_timer(&sw_watchpoint_timer, jiffies + msecs_to_jiffies(watch_interval_ms));
}

static int __init sw_watchpoint_timer_init(void)
{
	bool has_watch = false;

	if (!watch_interval_ms)
		return 0;

	if (paddr_dram_to_watch) {
		vaddr_dram = memremap(paddr_dram_to_watch, sizeof(*vaddr_dram),
				      MEMREMAP_WB);
		if (!vaddr_dram)
			return -ENOMEM;

		has_watch = true;
	}

	if (paddr_mmio_to_watch) {
		if (paddr_mmio_to_watch & 0x3) {
			pr_info("paddr_mmio_to_watch should be 32 bits aligned!\n");
			return -EINVAL;
		}

		vaddr_mmio = ioremap(paddr_mmio_to_watch, 4);
		if (!vaddr_mmio)
			return -ENOMEM;

		has_watch = true;
	}

	if (!has_watch)
		return 0;

	timer_setup(&sw_watchpoint_timer, sw_watchpoint_timer_fn, 0);
	sw_watchpoint_timer.expires = jiffies + msecs_to_jiffies(watch_interval_ms);
	add_timer(&sw_watchpoint_timer);
	return 0;
}
core_initcall(sw_watchpoint_timer_init);
