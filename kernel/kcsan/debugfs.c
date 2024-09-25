// SPDX-License-Identifier: GPL-2.0
/*
 * KCSAN debugfs interface.
 *
 * Copyright (C) 2019, Google LLC.
 */

#define pr_fmt(fmt) "kcsan: " fmt

#include <linux/atomic.h>
#include <linux/bsearch.h>
#include <linux/bug.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "kcsan.h"

atomic_long_t kcsan_counters[KCSAN_COUNTER_COUNT];
static const char *const counter_names[] = {
	[KCSAN_COUNTER_USED_WATCHPOINTS]		= "used_watchpoints",
	[KCSAN_COUNTER_SETUP_WATCHPOINTS]		= "setup_watchpoints",
	[KCSAN_COUNTER_DATA_RACES]			= "data_races",
	[KCSAN_COUNTER_ASSERT_FAILURES]			= "assert_failures",
	[KCSAN_COUNTER_NO_CAPACITY]			= "no_capacity",
	[KCSAN_COUNTER_REPORT_RACES]			= "report_races",
	[KCSAN_COUNTER_RACES_UNKNOWN_ORIGIN]		= "races_unknown_origin",
	[KCSAN_COUNTER_UNENCODABLE_ACCESSES]		= "unencodable_accesses",
	[KCSAN_COUNTER_ENCODING_FALSE_POSITIVES]	= "encoding_false_positives",
};
static_assert(ARRAY_SIZE(counter_names) == KCSAN_COUNTER_COUNT);

/*
 * Addresses for filtering functions from reporting. This list can be used as a
 * whitelist or blacklist.
 */
struct report_filterlist {
	unsigned long	*addrs;		/* array of addresses */
	size_t		size;		/* current size */
	int		used;		/* number of elements used */
	bool		sorted;		/* if elements are sorted */
	bool		whitelist;	/* if list is a blacklist or whitelist */
	struct rcu_head rcu;
};

static DEFINE_MUTEX(rp_flist_mutex);
static struct report_filterlist __rcu *rp_flist;

/*
 * The microbenchmark allows benchmarking KCSAN core runtime only. To run
 * multiple threads, pipe 'microbench=<iters>' from multiple tasks into the
 * debugfs file. This will not generate any conflicts, and tests fast-path only.
 */
static noinline void microbenchmark(unsigned long iters)
{
	const struct kcsan_ctx ctx_save = current->kcsan_ctx;
	const bool was_enabled = READ_ONCE(kcsan_enabled);
	u64 cycles;

	/* We may have been called from an atomic region; reset context. */
	memset(&current->kcsan_ctx, 0, sizeof(current->kcsan_ctx));
	/*
	 * Disable to benchmark fast-path for all accesses, and (expected
	 * negligible) call into slow-path, but never set up watchpoints.
	 */
	WRITE_ONCE(kcsan_enabled, false);

	pr_info("%s begin | iters: %lu\n", __func__, iters);

	cycles = get_cycles();
	while (iters--) {
		unsigned long addr = iters & ((PAGE_SIZE << 8) - 1);
		int type = !(iters & 0x7f) ? KCSAN_ACCESS_ATOMIC :
				(!(iters & 0xf) ? KCSAN_ACCESS_WRITE : 0);
		__kcsan_check_access((void *)addr, sizeof(long), type);
	}
	cycles = get_cycles() - cycles;

	pr_info("%s end   | cycles: %llu\n", __func__, cycles);

	WRITE_ONCE(kcsan_enabled, was_enabled);
	/* restore context */
	current->kcsan_ctx = ctx_save;
}

static int cmp_filterlist_addrs(const void *rhs, const void *lhs)
{
	const unsigned long a = *(const unsigned long *)rhs;
	const unsigned long b = *(const unsigned long *)lhs;

	return a < b ? -1 : a == b ? 0 : 1;
}

bool kcsan_skip_report_debugfs(unsigned long func_addr)
{
	unsigned long symbolsize, offset;
	bool ret = false;
	struct report_filterlist *list;

	if (!kallsyms_lookup_size_offset(func_addr, &symbolsize, &offset))
		return false;
	func_addr -= offset; /* Get function start */

	rcu_read_lock();
	list = rcu_dereference(rp_flist);

	if (!list)
		goto out;

	if (list->used == 0)
		goto out;

	/* Sort array if it is unsorted, and then do a binary search. */
	if (!list->sorted) {
		sort(list->addrs, list->used,
		     sizeof(unsigned long), cmp_filterlist_addrs, NULL);
		list->sorted = true;
	}
	ret = !!bsearch(&func_addr, list->addrs,
			list->used, sizeof(unsigned long),
			cmp_filterlist_addrs);
	if (list->whitelist)
		ret = !ret;

out:
	rcu_read_unlock();
	return ret;
}

static ssize_t set_report_filterlist_whitelist(bool whitelist)
{
	struct report_filterlist *new_list, *old_list;
	ssize_t ret = 0;

	mutex_lock(&rp_flist_mutex);
	old_list = rcu_dereference_protected(rp_flist,
					   lockdep_is_held(&rp_flist_mutex));

	new_list = kzalloc(sizeof(*new_list), GFP_KERNEL);
	if (!new_list) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(new_list, old_list, sizeof(struct report_filterlist));
	new_list->whitelist = whitelist;

	rcu_assign_pointer(rp_flist, new_list);
	synchronize_rcu();
	kfree(old_list);

out:
	mutex_unlock(&rp_flist_mutex);
	return ret;
}

/* Returns 0 on success, error-code otherwise. */
static ssize_t insert_report_filterlist(const char *func)
{
	unsigned long addr = kallsyms_lookup_name(func);
	ssize_t ret = 0;
	struct report_filterlist *new_list, *old_list;
	unsigned long *new_addrs;

	if (!addr) {
		pr_err("could not find function: '%s'\n", func);
		return -ENOENT;
	}

	new_list = kzalloc(sizeof(*new_list), GFP_KERNEL);
	if (!new_list)
		return -ENOMEM;

	mutex_lock(&rp_flist_mutex);
	old_list = rcu_dereference_protected(rp_flist,
					   lockdep_is_held(&rp_flist_mutex));
	memcpy(new_list, old_list, sizeof(struct report_filterlist));

	if (new_list->addrs == NULL) {
		/* initial allocation */
		new_list->addrs =
			kmalloc_array(new_list->size,
					  sizeof(unsigned long), GFP_KERNEL);
		if (new_list->addrs == NULL)
			goto out_free;
	} else if (new_list->used == new_list->size) {
		/* resize filterlist */
		size_t new_size = new_list->size * 2;

		new_addrs = kmalloc_array(new_size,
					  sizeof(unsigned long), GFP_KERNEL);
		if (new_addrs == NULL)
			goto out_free;

		memcpy(new_addrs, old_list->addrs,
				old_list->size * sizeof(unsigned long));
		new_list->size = new_size;
		new_list->addrs = new_addrs;
	} else {
		new_addrs = kmalloc_array(new_list->size,
					  sizeof(unsigned long), GFP_KERNEL);
		if (new_addrs == NULL)
			goto out_free;

		memcpy(new_addrs, old_list->addrs,
				old_list->size * sizeof(unsigned long));
		new_list->addrs = new_addrs;
	}

	/* Note: deduplicating should be done in userspace. */
	new_list->addrs[new_list->used++] = addr;
	new_list->sorted = false;

	rcu_assign_pointer(rp_flist, new_list);
	synchronize_rcu();
	kfree(old_list->addrs);
	kfree(old_list);
	goto out;

out_free:
	ret = -ENOMEM;
	kfree(new_list);
out:
	mutex_unlock(&rp_flist_mutex);
	return ret;
}

static int show_info(struct seq_file *file, void *v)
{
	int i;
	struct report_filterlist *list;

	/* show stats */
	seq_printf(file, "enabled: %i\n", READ_ONCE(kcsan_enabled));
	for (i = 0; i < KCSAN_COUNTER_COUNT; ++i) {
		seq_printf(file, "%s: %ld\n", counter_names[i],
			   atomic_long_read(&kcsan_counters[i]));
	}

	rcu_read_lock();
	list = rcu_dereference(rp_flist);
	/* show filter functions, and filter type */
	seq_printf(file, "\n%s functions: %s\n",
		   list->whitelist ? "whitelisted" : "blacklisted",
		   list->used == 0 ? "none" : "");
	for (i = 0; i < list->used; ++i)
		seq_printf(file, " %ps\n", (void *)list->addrs[i]);
	rcu_read_unlock();

	return 0;
}

static int debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, show_info, NULL);
}

static ssize_t
debugfs_write(struct file *file, const char __user *buf, size_t count, loff_t *off)
{
	char kbuf[KSYM_NAME_LEN];
	char *arg;
	const size_t read_len = min(count, sizeof(kbuf) - 1);
	ssize_t ret;

	if (copy_from_user(kbuf, buf, read_len))
		return -EFAULT;
	kbuf[read_len] = '\0';
	arg = strstrip(kbuf);

	if (!strcmp(arg, "on")) {
		WRITE_ONCE(kcsan_enabled, true);
	} else if (!strcmp(arg, "off")) {
		WRITE_ONCE(kcsan_enabled, false);
	} else if (str_has_prefix(arg, "microbench=")) {
		unsigned long iters;

		if (kstrtoul(&arg[strlen("microbench=")], 0, &iters))
			return -EINVAL;
		microbenchmark(iters);
	} else if (!strcmp(arg, "whitelist")) {
		ret = set_report_filterlist_whitelist(true);
	} else if (!strcmp(arg, "blacklist")) {
		ret = set_report_filterlist_whitelist(false);
	} else if (arg[0] == '!') {
		ret = insert_report_filterlist(&arg[1]);
	} else {
		return -EINVAL;
	}

	if (ret < 0)
		return ret;
	else
		return count;
}

static const struct file_operations debugfs_ops =
{
	.read	 = seq_read,
	.open	 = debugfs_open,
	.write	 = debugfs_write,
	.release = single_release
};

static int __init kcsan_debugfs_init(void)
{
	struct report_filterlist *list;

	list = kzalloc(sizeof(*list), GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	list->addrs		= NULL;
	list->size		= 8;		/* small initial size */
	list->used		= 0;
	list->sorted	= false;
	list->whitelist	= false;	/* default is blacklist */
	rcu_assign_pointer(rp_flist, list);

	debugfs_create_file("kcsan", 0644, NULL, NULL, &debugfs_ops);
	return 0;
}

late_initcall(kcsan_debugfs_init);
