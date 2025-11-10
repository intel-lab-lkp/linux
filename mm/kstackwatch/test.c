// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/kstackwatch.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/prandom.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

static struct dentry *test_file;

#define BUFFER_SIZE 32
#define MAX_DEPTH 6

struct work_node {
	ulong *ptr;
	u64 start_ns;
	struct completion done;
	struct list_head list;
};

static DECLARE_COMPLETION(work_res);
static DEFINE_MUTEX(work_mutex);
static LIST_HEAD(work_list);

static int global_fence_size;
static int global_loop_count;

static void test_watch_fire(void)
{
	u64 buffer[BUFFER_SIZE] = { 0 };

	pr_info("entry of %s\n", __func__);
	ksw_watch_show();
	pr_info("buf: 0x%px\n", buffer);

	ksw_watch_fire();

	barrier_data(buffer);
	pr_info("exit of %s\n", __func__);
}

static void test_canary_overflow(void)
{
	u64 buffer[BUFFER_SIZE];

	pr_info("entry of %s\n", __func__);
	ksw_watch_show();
	pr_info("buf: 0x%px\n", buffer);

	/* intentionally overflow */
	for (int i = BUFFER_SIZE; i < BUFFER_SIZE + 10; i++)
		buffer[i] = 0xdeadbeefdeadbeef;
	barrier_data(buffer);

	pr_info("exit of %s\n", __func__);
}

static void test_recursive_depth(int depth)
{
	u64 buffer[BUFFER_SIZE];

	pr_info("entry of %s depth:%d\n", __func__, depth);

	if (depth < MAX_DEPTH)
		test_recursive_depth(depth + 1);

	buffer[0] = depth;
	barrier_data(buffer);

	pr_info("exit of %s depth:%d\n", __func__, depth);
}

static struct work_node *test_mthread_buggy(int thread_id, int seq_id)
{
	ulong buf[BUFFER_SIZE];
	struct work_node *node;
	bool trigger;

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return NULL;

	init_completion(&node->done);
	node->ptr = buf;
	node->start_ns = ktime_get_ns();
	mutex_lock(&work_mutex);
	list_add(&node->list, &work_list);
	mutex_unlock(&work_mutex);
	complete(&work_res);

	trigger = (get_random_u32() % 100) < 10;
	if (trigger)
		return node; /* let the caller handle cleanup */

	wait_for_completion(&node->done);
	kfree(node);
	return NULL;
}

#define CORRUPTING_MINIOR_WAIT_NS (100000)
#define VICTIM_MINIOR_WAIT_NS (300000)

static inline void silent_wait_us(u64 start_ns, u64 min_wait_us)
{
	u64 diff_ns, remain_us;

	diff_ns = ktime_get_ns() - start_ns;
	if (diff_ns < min_wait_us * 1000ULL) {
		remain_us = min_wait_us - (diff_ns >> 10);
		usleep_range(remain_us, remain_us + 200);
	}
}

static void test_mthread_victim(int thread_id, int seq_id, u64 start_ns)
{
	ulong buf[BUFFER_SIZE];

	for (int j = 0; j < BUFFER_SIZE; j++)
		buf[j] = 0xdeadbeef + seq_id;
	if (start_ns)
		silent_wait_us(start_ns, VICTIM_MINIOR_WAIT_NS);

	for (int j = 0; j < BUFFER_SIZE; j++) {
		if (buf[j] != (0xdeadbeef + seq_id)) {
			pr_warn("victim[%d][%d]: unhappy buf[%d]=0x%lx\n",
				thread_id, seq_id, j, buf[j]);
			return;
		}
	}

	pr_info("victim[%d][%d]: happy\n", thread_id, seq_id);
}

static int test_mthread_corrupting(void *data)
{
	struct work_node *node;
	int fence_size;

	while (!kthread_should_stop()) {
		if (!wait_for_completion_timeout(&work_res, HZ))
			continue;
		while (true) {
			mutex_lock(&work_mutex);
			node = list_first_entry_or_null(&work_list,
							struct work_node, list);
			if (node)
				list_del(&node->list);
			mutex_unlock(&work_mutex);

			if (!node)
				break; /* no more nodes, exit inner loop */
			silent_wait_us(node->start_ns,
				       CORRUPTING_MINIOR_WAIT_NS);

			fence_size = READ_ONCE(global_fence_size);
			for (int i = fence_size; i < BUFFER_SIZE - fence_size;
			     i++)
				node->ptr[i] = 0xabcdabcd;

			complete(&node->done);
		}
	}

	return 0;
}

static int test_mthread_worker(void *data)
{
	int thread_id = (long)data;
	int loop_count;
	struct work_node *node;

	loop_count = READ_ONCE(global_loop_count);

	for (int i = 0; i < loop_count; i++) {
		node = test_mthread_buggy(thread_id, i);

		if (node)
			test_mthread_victim(thread_id, i, node->start_ns);
		else
			test_mthread_victim(thread_id, i, 0);
		if (node) {
			wait_for_completion(&node->done);
			kfree(node);
		}
	}
	return 0;
}

static void test_mthread_case(int num_workers, int loop_count, int fence_size)
{
	static struct task_struct *corrupting;
	static struct task_struct **workers;

	WRITE_ONCE(global_loop_count, loop_count);
	WRITE_ONCE(global_fence_size, fence_size);

	init_completion(&work_res);
	workers = kmalloc_array(num_workers, sizeof(void *), GFP_KERNEL);
	memset(workers, 0, sizeof(struct task_struct *) * num_workers);

	corrupting = kthread_run(test_mthread_corrupting, NULL, "corrupting");
	if (IS_ERR(corrupting)) {
		pr_err("failed to create corrupting thread\n");
		return;
	}

	for (ulong i = 0; i < num_workers; i++) {
		workers[i] = kthread_run(test_mthread_worker, (void *)i,
					 "worker_%ld", i);
		if (IS_ERR(workers[i])) {
			pr_err("failto create worker thread %ld", i);
			workers[i] = NULL;
		}
	}

	for (ulong i = 0; i < num_workers; i++) {
		if (workers[i] && workers[i]->__state != TASK_DEAD) {
			usleep_range(1000, 2000);
			i--;
		}
	}
	kfree(workers);

	if (corrupting && !IS_ERR(corrupting)) {
		kthread_stop(corrupting);
		corrupting = NULL;
	}
}

static ssize_t test_dbgfs_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *pos)
{
	char cmd[256];
	int test_num;

	if (count >= sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(cmd, buffer, count))
		return -EFAULT;

	cmd[count] = '\0';
	strim(cmd);

	pr_info("received command: %s\n", cmd);

	if (sscanf(cmd, "test%d", &test_num) == 1) {
		switch (test_num) {
		case 0:
			test_watch_fire();
			break;
		case 1:
			test_canary_overflow();
			break;
		case 2:
			test_recursive_depth(0);
			break;
		case 3:
			test_mthread_case(1, 20, BUFFER_SIZE / 4);
			break;
		case 4:
			test_mthread_case(200, 1, BUFFER_SIZE / 4);
			break;
		case 5:
			test_mthread_case(1, 1, -3);
			break;
		default:
			pr_err("Unknown test number %d\n", test_num);
			return -EINVAL;
		}
	} else {
		pr_err("invalid command format. Use 'testN'.\n");
		return -EINVAL;
	}

	return count;
}

static ssize_t test_dbgfs_read(struct file *file, char __user *buffer,
			       size_t count, loff_t *ppos)
{
	static const char usage[] =
		"KStackWatch Simplified Test Module\n"
		"============ usage ===============\n"
		"Usage:\n"
		"echo test{i} > /sys/kernel/debug/kstackwatch/test\n"
		" test0 - test watch fire\n"
		" test1 - test canary overflow\n"
		" test2 - test recursive func\n"
		" test3 - test silent corruption\n"
		" test4 - test multiple silent corruption\n"
		" test5 - test prologue corruption\n";

	return simple_read_from_buffer(buffer, count, ppos, usage,
				       strlen(usage));
}

static const struct file_operations test_dbgfs_fops = {
	.owner = THIS_MODULE,
	.read = test_dbgfs_read,
	.write = test_dbgfs_write,
	.llseek = noop_llseek,
};

static int __init kstackwatch_test_init(void)
{
	struct dentry *ksw_dir = ksw_get_dbgdir();

	if (!ksw_dir) {
		pr_err("kstackwatch must be loaded first\n");
		return -ENODEV;
	}

	test_file = debugfs_create_file("test", 0600, ksw_dir, NULL,
					&test_dbgfs_fops);
	if (!test_file) {
		pr_err("Failed to create debugfs test file\n");
		return -ENOMEM;
	}

	pr_info("module loaded\n");
	return 0;
}

static void __exit kstackwatch_test_exit(void)
{
	debugfs_remove(test_file);
	pr_info("module unloaded\n");
}

module_init(kstackwatch_test_init);
module_exit(kstackwatch_test_exit);

MODULE_AUTHOR("Jinchao Wang");
MODULE_DESCRIPTION("KStackWatch Test Module");
MODULE_LICENSE("GPL");
