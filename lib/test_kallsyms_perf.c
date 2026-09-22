// SPDX-License-Identifier: GPL-2.0-only
/*
 * Microbenchmark and correctness test module for kallsyms subsystem
 *
 * Measures CPU latency across:
 *  - Name-to-Address binary search (hits & misses)
 *  - Address-to-Name symbol resolution (sprint_symbol, buildid)
 *  - Full kernel symbol iteration (kallsyms_on_each_symbol)
 */

#define pr_fmt(fmt) "test_kallsyms: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/ktime.h>
#include <linux/compiler.h>

static unsigned int num_iters = 100000;
module_param(num_iters, uint, 0644);
MODULE_PARM_DESC(num_iters, "Number of iterations per microbenchmark");

static const char * const hit_symbols[] = {
	"_printk",
	"schedule",
	"vfs_read",
	"do_sys_openat2",
	"kernel_clone",
	"tcp_v4_rcv",
	"kallsyms_lookup_names",
	"vm_area_alloc",
};

static const char * const miss_symbols[] = {
	"nonexistent_symbol_0001",
	"xyz_dummy_missing_symbol",
	"__never_compiled_in_kernel",
	"ext4_nonexistent_func_xyz",
	"bpf_not_real_helper_stub",
	"vfs_missing_handler_probe",
	"tcp_v4_unimplemented_path",
	"driver_fake_init_routine",
};

static int match_cb(void *data, unsigned long addr)
{
	unsigned long *out = data;

	*out = addr;
	return 1;
}

static int count_cb(void *data, const char *name, unsigned long addr)
{
	unsigned long *cnt = data;

	(*cnt)++;
	return 0;
}

static void run_name_lookup_bench(void)
{
	u64 t0, t1, dt_hit, dt_miss;
	unsigned long addr = 0;
	unsigned int i, nr_hits, nr_misses;

	nr_hits = ARRAY_SIZE(hit_symbols);
	nr_misses = ARRAY_SIZE(miss_symbols);

	/* 0. Correctness validation */
	for (i = 0; i < nr_hits; i++) {
		const char *sym = hit_symbols[i];
		unsigned long a1 = 0;

		kallsyms_on_each_match_symbol(match_cb, sym, &a1);
		if (!a1)
			pr_err("CORRECTNESS FAILURE: hit sym '%s' not found\n", sym);
	}
	for (i = 0; i < nr_misses; i++) {
		const char *sym = miss_symbols[i];
		unsigned long a1 = 0;

		kallsyms_on_each_match_symbol(match_cb, sym, &a1);
		if (a1)
			pr_err("CORRECTNESS FAILURE: miss sym '%s' unexpectedly found a1=%lx\n",
			       sym, a1);
	}

	/* 1. Name search: Existing symbols (Hits) */
	t0 = ktime_get_ns();
	for (i = 0; i < num_iters; i++) {
		const char *sym = hit_symbols[i % nr_hits];

		kallsyms_on_each_match_symbol(match_cb, sym, &addr);
		OPTIMIZER_HIDE_VAR(addr);
	}
	t1 = ktime_get_ns();
	dt_hit = t1 - t0;

	/* 2. Name search: Non-existent symbols (Misses - 17 bsearch probes) */
	t0 = ktime_get_ns();
	for (i = 0; i < num_iters; i++) {
		const char *sym = miss_symbols[i % nr_misses];

		kallsyms_on_each_match_symbol(match_cb, sym, &addr);
		OPTIMIZER_HIDE_VAR(addr);
	}
	t1 = ktime_get_ns();
	dt_miss = t1 - t0;

	pr_info("Name Search Hit:  %llu ns/lookup (%llu ms total, %u iters)\n",
		dt_hit / num_iters, dt_hit / 1000000, num_iters);
	pr_info("Name Search Miss: %llu ns/lookup (%llu ms total, %u iters)\n",
		dt_miss / num_iters, dt_miss / 1000000, num_iters);
}

static void run_address_lookup_bench(void)
{
	u64 t0, t1, dt_sprint, dt_bldid;
	char symname[KSYM_SYMBOL_LEN];
	unsigned long addrs[ARRAY_SIZE(hit_symbols)];
	unsigned int i, nr_addrs = 0;

	for (i = 0; i < ARRAY_SIZE(hit_symbols); i++) {
		unsigned long addr = 0;

		kallsyms_on_each_match_symbol(match_cb, hit_symbols[i], &addr);
		if (addr)
			addrs[nr_addrs++] = addr;
	}

	if (!nr_addrs) {
		pr_warn("Address benchmark skipped: no test addresses resolved\n");
		return;
	}

	/* 1. Address-to-name resolution (sprint_symbol) */
	t0 = ktime_get_ns();
	for (i = 0; i < num_iters; i++) {
		unsigned long addr = addrs[i % nr_addrs];

		sprint_symbol(symname, addr);
		barrier_data(symname);
	}
	t1 = ktime_get_ns();
	dt_sprint = t1 - t0;

	/* 2. Address without offset (sprint_symbol_no_offset) */
	t0 = ktime_get_ns();
	for (i = 0; i < num_iters; i++) {
		unsigned long addr = addrs[i % nr_addrs];

		sprint_symbol_no_offset(symname, addr);
		barrier_data(symname);
	}
	t1 = ktime_get_ns();
	dt_bldid = t1 - t0;

	pr_info("sprint_symbol:           %llu ns/lookup (%llu ms total, %u iters)\n",
		dt_sprint / num_iters, dt_sprint / 1000000, num_iters);
	pr_info("sprint_symbol_no_offset: %llu ns/lookup (%llu ms total, %u iters)\n",
		dt_bldid / num_iters, dt_bldid / 1000000, num_iters);
}

static void run_table_walk_bench(void)
{
	u64 t0, t1, dt_walk;
	unsigned long total_symbols = 0;
	int iter = 50;
	int i;

	t0 = ktime_get_ns();
	for (i = 0; i < iter; i++) {
		total_symbols = 0;
		kallsyms_on_each_symbol(count_cb, &total_symbols);
	}
	t1 = ktime_get_ns();
	dt_walk = t1 - t0;

	pr_info("Table Full Walk:  %llu us/pass (%lu symbols scanned, %d passes)\n",
		(dt_walk / iter) / 1000, total_symbols, iter);
}

static int run_kallsyms_benchmark(void)
{
	pr_info("==================================================\n");
	pr_info("Starting kallsyms performance benchmark (iters=%u)\n", num_iters);
	pr_info("==================================================\n");

	run_name_lookup_bench();
	run_address_lookup_bench();
	run_table_walk_bench();

	pr_info("==================================================\n");
	pr_info("kallsyms benchmark complete\n");
	pr_info("==================================================\n");

	return 0;
}

static int param_set_trigger(const char *val, const struct kernel_param *kp)
{
	return run_kallsyms_benchmark();
}

static const struct kernel_param_ops param_ops_trigger = {
	.set = param_set_trigger,
};
module_param_cb(run_test, &param_ops_trigger, NULL, 0200);
MODULE_PARM_DESC(run_test, "Write 1 to trigger kallsyms benchmark run");

static int __init test_kallsyms_init(void)
{
	return run_kallsyms_benchmark();
}

static void __exit test_kallsyms_exit(void)
{
	pr_info("test_kallsyms module unloaded\n");
}

module_init(test_kallsyms_init);
module_exit(test_kallsyms_exit);

MODULE_DESCRIPTION("Microbenchmark test module for kallsyms subsystem");
MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_LICENSE("GPL");
