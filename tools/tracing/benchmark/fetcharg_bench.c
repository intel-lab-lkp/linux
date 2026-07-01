// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>

#define CREATE_TRACE_POINTS
#include "fetcharg_bench_trace.h"

static noinline int fetcharg_bench_target(int a, int **b, char *c)
{
	/* Prevent compiler from optimizing the loop out entirely */
	asm volatile ("" : : "r"(a), "r"(b), "r"(c) : "memory");
	trace_fetcharg_bench_event(a, b, c);
	return a + **b;
}

/* Indirect pointer to prevent inlining */
static int (*bench_func_ptr)(int, int **, char *) = fetcharg_bench_target;

#define BENCH_ITERATIONS 1000000

static ssize_t fetcharg_bench_read(struct file *file, char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	char buf[64];
	int len;
	u64 start, current_time;
	u64 elapsed;
	u64 loops_per_sec;
	int dummy = 0;
	int a = 1;
	int b_val = 2;
	int *b_ptr = &b_val;
	int **b = &b_ptr;
	char c[] = "benchmark";
	int i;

	if (*ppos > 0)
		return 0; /* EOF */

	start = ktime_get_ns();
	for (i = 0; i < BENCH_ITERATIONS; i++)
		dummy += bench_func_ptr(a, b, c);
	current_time = ktime_get_ns();

	elapsed = current_time - start;
	loops_per_sec = ((u64)BENCH_ITERATIONS * NSEC_PER_SEC) / elapsed;

	len = snprintf(buf, sizeof(buf), "%llu\n", loops_per_sec);
	if (len < 0)
		return len;

	if (copy_to_user(user_buf, buf, len))
		return -EFAULT;

	*ppos += len;

	/*
	 * Use 'dummy' to ensure the compiler doesn't optimize out
	 * the call completely, though the asm volatile helps too.
	 */
	if (dummy == 0xdeadbeef)
		pr_info("dummy=%d\n", dummy);

	return len;
}

static const struct file_operations fetcharg_bench_fops = {
	.read		= fetcharg_bench_read,
	.open		= simple_open,
	.llseek		= default_llseek,
};

static struct dentry *bench_dir;

static int __init fetcharg_bench_init(void)
{
	bench_dir = debugfs_create_dir("fetcharg_benchmark", NULL);
	if (!bench_dir)
		return -ENOMEM;

	debugfs_create_file("trigger", 0444, bench_dir, NULL, &fetcharg_bench_fops);

	return 0;
}

static void __exit fetcharg_bench_exit(void)
{
	debugfs_remove_recursive(bench_dir);
}

module_init(fetcharg_bench_init);
module_exit(fetcharg_bench_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity");
MODULE_DESCRIPTION("Fetcharg performance benchmark test module");
