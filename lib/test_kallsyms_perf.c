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
#include <linux/sched.h>
#include <linux/math.h>
#include <linux/mutex.h>
#include <linux/minmax.h>

#define BENCH_CHUNK_SIZE 4096

static unsigned int num_iters = 100000;

static int param_set_num_iters(const char *val, const struct kernel_param *kp)
{
	unsigned int n;
	int ret;

	ret = kstrtouint(val, 0, &n);
	if (ret)
		return ret;
	if (!n)
		return -EINVAL;

	*((unsigned int *)kp->arg) = n;
	return 0;
}

static const struct kernel_param_ops param_ops_num_iters = {
	.set = param_set_num_iters,
	.get = param_get_uint,
};
module_param_cb(num_iters, &param_ops_num_iters, &num_iters, 0644);
MODULE_PARM_DESC(num_iters, "Number of iterations per microbenchmark (must be > 0)");

static bool run_on_boot;
module_param(run_on_boot, bool, 0444);
MODULE_PARM_DESC(run_on_boot, "Run benchmark during boot (default: false)");

static DEFINE_MUTEX(bench_lock);

static const char * const hit_symbols[] = {
	"_printk",
	"schedule",
	"vfs_read",
	"do_sys_openat2",
	"kernel_clone",
	"panic",
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

static void run_name_lookup_bench(const char *mode, unsigned int iters,
				  u64 *avg_hit_ns, u64 *avg_miss_ns)
{
	u64 t0, t1, dt_hit = 0, dt_miss = 0;
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
	for (i = 0; i < iters; i += BENCH_CHUNK_SIZE) {
		unsigned int chunk = min_t(unsigned int, BENCH_CHUNK_SIZE, iters - i);
		unsigned int j;

		cond_resched();
		t0 = ktime_get_ns();
		for (j = 0; j < chunk; j++) {
			const char *sym = hit_symbols[(i + j) % nr_hits];

			kallsyms_on_each_match_symbol(match_cb, sym, &addr);
			OPTIMIZER_HIDE_VAR(addr);
		}
		t1 = ktime_get_ns();
		dt_hit += t1 - t0;
	}

	/* 2. Name search: Non-existent symbols (Misses - 17 bsearch probes) */
	for (i = 0; i < iters; i += BENCH_CHUNK_SIZE) {
		unsigned int chunk = min_t(unsigned int, BENCH_CHUNK_SIZE, iters - i);
		unsigned int j;

		cond_resched();
		t0 = ktime_get_ns();
		for (j = 0; j < chunk; j++) {
			const char *sym = miss_symbols[(i + j) % nr_misses];

			kallsyms_on_each_match_symbol(match_cb, sym, &addr);
			OPTIMIZER_HIDE_VAR(addr);
		}
		t1 = ktime_get_ns();
		dt_miss += t1 - t0;
	}

	if (avg_hit_ns)
		*avg_hit_ns = div_u64(dt_hit, iters);
	if (avg_miss_ns)
		*avg_miss_ns = div_u64(dt_miss, iters);

	pr_info("[%s] Name Search Hit:  %llu ns/lookup (%llu ms total, %u iters)\n",
		mode, div_u64(dt_hit, iters), div_u64(dt_hit, 1000000), iters);
	pr_info("[%s] Name Search Miss: %llu ns/lookup (%llu ms total, %u iters)\n",
		mode, div_u64(dt_miss, iters), div_u64(dt_miss, 1000000), iters);
}

static void run_address_lookup_bench(unsigned int iters)
{
	u64 t0, t1, dt_sprint = 0, dt_bldid = 0;
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
	for (i = 0; i < iters; i += BENCH_CHUNK_SIZE) {
		unsigned int chunk = min_t(unsigned int, BENCH_CHUNK_SIZE, iters - i);
		unsigned int j;

		cond_resched();
		t0 = ktime_get_ns();
		for (j = 0; j < chunk; j++) {
			unsigned long addr = addrs[(i + j) % nr_addrs];

			sprint_symbol(symname, addr);
			barrier_data(symname);
		}
		t1 = ktime_get_ns();
		dt_sprint += t1 - t0;
	}

	/* 2. Address without offset (sprint_symbol_no_offset) */
	for (i = 0; i < iters; i += BENCH_CHUNK_SIZE) {
		unsigned int chunk = min_t(unsigned int, BENCH_CHUNK_SIZE, iters - i);
		unsigned int j;

		cond_resched();
		t0 = ktime_get_ns();
		for (j = 0; j < chunk; j++) {
			unsigned long addr = addrs[(i + j) % nr_addrs];

			sprint_symbol_no_offset(symname, addr);
			barrier_data(symname);
		}
		t1 = ktime_get_ns();
		dt_bldid += t1 - t0;
	}

	pr_info("sprint_symbol:           %llu ns/lookup (%llu ms total, %u iters)\n",
		div_u64(dt_sprint, iters), div_u64(dt_sprint, 1000000), iters);
	pr_info("sprint_symbol_no_offset: %llu ns/lookup (%llu ms total, %u iters)\n",
		div_u64(dt_bldid, iters), div_u64(dt_bldid, 1000000), iters);
}

static void run_table_walk_bench(void)
{
	u64 t0, t1, dt_walk = 0;
	unsigned long total_symbols = 0;
	int iter = 50;
	int i;

	for (i = 0; i < iter; i++) {
		total_symbols = 0;
		cond_resched();
		t0 = ktime_get_ns();
		kallsyms_on_each_symbol(count_cb, &total_symbols);
		t1 = ktime_get_ns();
		dt_walk += t1 - t0;
	}

	pr_info("Table Full Walk:  %llu ns/sym (%llu us/pass, %lu symbols scanned, %d passes)\n",
		div_u64(div_u64(dt_walk, iter), total_symbols ? total_symbols : 1),
		div_u64(div_u64(dt_walk, iter), 1000), total_symbols, iter);
}

static int run_kallsyms_benchmark(void)
{
	unsigned int iters;
	u64 unindexed_hit = 0, unindexed_miss = 0;
	u64 dyn_hit = 0, dyn_miss = 0;
	u64 t0, t1, dt_start = 0, dt_end = 0;
	int ret = 0;

	if (!mutex_trylock(&bench_lock)) {
		pr_warn("Benchmark already running\n");
		return -EBUSY;
	}

	iters = READ_ONCE(num_iters);
	if (!iters) {
		pr_err("num_iters must be non-zero\n");
		ret = -EINVAL;
		goto out;
	}

	pr_info("==================================================\n");
	pr_info("Starting kallsyms performance benchmark (iters=%u)\n", iters);
	pr_info("==================================================\n");

	/* 1. Benchmark baseline unindexed marker scan */
	run_name_lookup_bench("Unindexed (markers)", iters, &unindexed_hit, &unindexed_miss);

	/* 2. Benchmark dynamic lookup index */
	t0 = ktime_get_ns();
	ret = kallsyms_lookup_batch_start();
	t1 = ktime_get_ns();
	dt_start = t1 - t0;

	if (ret) {
		pr_err("Failed to start kallsyms lookup batch: %d\n", ret);
	} else {
		run_name_lookup_bench("Dynamic Index", iters, &dyn_hit, &dyn_miss);

		t0 = ktime_get_ns();
		kallsyms_lookup_batch_end();
		t1 = ktime_get_ns();
		dt_end = t1 - t0;

		pr_info("[Dynamic Index] Batch setup: %llu us, teardown: %llu us\n",
			div_u64(dt_start, 1000), div_u64(dt_end, 1000));

		if (unindexed_hit > dyn_hit) {
			u64 saved = unindexed_hit - dyn_hit;

			pr_info("[Dynamic Index] Amortization break-even: %llu queries (setup), %llu queries (total)\n",
				DIV_ROUND_UP_ULL(dt_start, saved),
				DIV_ROUND_UP_ULL(dt_start + dt_end, saved));
		}
	}

	run_address_lookup_bench(iters);
	run_table_walk_bench();

	pr_info("==================================================\n");
	pr_info("kallsyms benchmark complete\n");
	pr_info("==================================================\n");

out:
	mutex_unlock(&bench_lock);
	return ret;
}

static int param_set_trigger(const char *val, const struct kernel_param *kp)
{
	if (system_state < SYSTEM_RUNNING) {
		pr_warn("Early boot run ignored; use test_kallsyms_perf.run_on_boot=1 or trigger via sysfs\n");
		return 0;
	}
	return run_kallsyms_benchmark();
}

static const struct kernel_param_ops param_ops_trigger = {
	.set = param_set_trigger,
};
module_param_cb(run_test, &param_ops_trigger, NULL, 0200);
MODULE_PARM_DESC(run_test, "Write 1 to trigger kallsyms benchmark run");

static int __init test_kallsyms_init(void)
{
	if (run_on_boot)
		return run_kallsyms_benchmark();
	return 0;
}
late_initcall(test_kallsyms_init);

MODULE_DESCRIPTION("Microbenchmark test module for kallsyms subsystem");
MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_LICENSE("GPL");
