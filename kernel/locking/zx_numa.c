// SPDX-License-Identifier: GPL-2.0
/*
 * Dynamic numa-aware osq lock
 * Crossing from numa-aware lock to osq_lock
 * Numa lock memory initialize and /proc interface
 * Author: LiYong <yongli-oc@zhaoxin.com>
 *
 */
#include <linux/cpumask.h>
#include <asm/byteorder.h>
#include <asm/kvm_para.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/osq_lock.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/reboot.h>

#include "numa.h"
#include "numa_osq.h"

int enable_zx_numa_osq_lock;
struct delayed_work zx_numa_start_work;
struct delayed_work zx_numa_cleanup_work;

atomic_t numa_count;
struct _numa_buf *zx_numa_entry;
int zx_numa_lock_total = 256;
LIST_HEAD(_zx_numa_head);
LIST_HEAD(_zx_numa_lock_head);

struct kmem_cache *zx_numa_entry_cachep;
struct kmem_cache *zx_numa_lock_cachep;
int NUMASHIFT;
int NUMACLUSTERS;
static atomic_t lockindex;
int dynamic_enable;

static const struct numa_cpu_info numa_cpu_list[] = {
	/*feature1=1, a numa node includes two clusters*/
	//{1, 23, X86_VENDOR_AMD, 0, 1},
	{0x5b, 7, X86_VENDOR_CENTAUR, 0, 1},
	{0x5b, 7, X86_VENDOR_ZHAOXIN, 0, 1}
};

inline void *get_numa_lock(int index)
{
	if (index >= 0 && index < zx_numa_lock_total)
		return zx_numa_entry[index].numa_ptr;
	else
		return NULL;
}

static int zx_get_numa_shift(int all_cpus, int clusters)
{
	int cpus = (int) all_cpus/clusters;
	int count = 0;

	while (cpus) {
		cpus >>= 1;
		count++;
	}
	return count-1;
}

void numa_lock_init_data(struct _numa_lock *s, int clusters,
			u32 lockval, u32 lockaddr)
{
	int j = 0;

	for (j = 0; j < clusters + NUMAEXPAND; j++) {
		atomic_set(&(s + j)->tail, lockval);
		atomic_set(&(s + j)->addr, lockaddr);
		(s + j)->shift = NUMASHIFT;
		(s + j)->stopping = 0;
		(s + j)->numa_nodes = clusters;
		(s + j)->accessed = 0;
		(s + j)->totalaccessed = 0;
		(s + j)->nodeswitched = 0;
		atomic_set(&(s + j)->initlock, 0);
		atomic_set(&(s + j)->pending, 0);
	}
}

int zx_numa_lock_ptr_get(void *p)
{
	int i = 0;
	int index = 0;

	if (atomic_read(&numa_count) >= zx_numa_lock_total)
		return zx_numa_lock_total;

	index = atomic_inc_return(&lockindex);

	for (i = 0; i < zx_numa_lock_total; i++) {
		if (index >= zx_numa_lock_total)
			index = 0;
		if (cmpxchg(&zx_numa_entry[index].lockaddr,
					0, ptrmask(p)) == 0) {
			while (1) {
				struct _numa_lock *node_lock =
					zx_numa_entry[index].numa_ptr;
				struct _numa_lock *numa_lock = node_lock +
						node_lock->numa_nodes;

				if (atomic_read(&numa_lock->tail) ==
								NUMA_LOCKED_VAL)
					break;
				cpu_relax();

			}
			atomic_inc(&numa_count);
			zx_numa_entry[index].highaddr = ((u64)p) >> 32;
			atomic_set(&lockindex, index);
			return index;
		}
		index++;
		if (atomic_read(&numa_count) >= zx_numa_lock_total)
			break;
	}
	return zx_numa_lock_total;
}

int zx_check_numa_dynamic_locked(u32 lockaddr,
		struct _numa_lock *_numa_lock, int t)
{
	struct _numa_lock *node_lock = NULL;
	u64 s = -1;
	int i = 0;

	if (atomic_read(&_numa_lock->pending) != 0)
		return 1;

	for (i = 0; i < _numa_lock->numa_nodes + 1; i++) {
		node_lock = _numa_lock + i;
		cpu_relax(); cpu_relax(); cpu_relax(); cpu_relax();
		s = atomic64_read((atomic64_t *) &node_lock->tail);
		if ((s >> 32) != lockaddr)
			continue;
		if ((s & LOW32MASK) == NUMA_LOCKED_VAL
				|| (s & LOW32MASK) == NUMA_UNLOCKED_VAL)
			continue;
		break;
	}

	if (i == _numa_lock->numa_nodes + 1)
		return 0;
	return i+1;
}

static int zx_numa_lock64_try_to_freeze(u32 lockaddr, struct _numa_lock *_numa_lock,
			int index)
{
	struct _numa_lock *node_lock = NULL;
	u64 addr = ((u64)lockaddr) << 32;
	u64 s = 0;
	u64 ff = 0;
	int i = 0;

	for (i = 0; i < _numa_lock->numa_nodes+1; i++) {
		node_lock = _numa_lock + i;
		cpu_relax();

		s = atomic64_read((atomic64_t *)&node_lock->tail);
		if ((s & HIGH32BITMASK) != addr)
			continue;

		if ((s & LOW32MASK) == NUMA_LOCKED_VAL)
			continue;

		if ((s & LOW32MASK) == NUMA_UNLOCKED_VAL) {
			ff = atomic64_cmpxchg((atomic64_t *)&node_lock->tail,
				(addr|NUMA_UNLOCKED_VAL), NUMA_LOCKED_VAL);
			if (ff == (addr|NUMA_UNLOCKED_VAL))
				continue;
		}
		break;
	}

	if (i == _numa_lock->numa_nodes + 1) {
		zx_numa_entry[index].idle = 0;
		zx_numa_entry[index].type = 0;
		zx_numa_entry[index].highaddr = 0;
		xchg(&zx_numa_entry[index].lockaddr, 0);
	}

	return i;
}

static void zx_numa_lock_stopping(struct _numa_lock *_numa_lock)
{
	struct _numa_lock *node_lock = NULL;
	int i = 0;

	for (i = 0; i < _numa_lock->numa_nodes+1; i++) {
		node_lock = _numa_lock + i;
		WRITE_ONCE(node_lock->stopping, 1);
	}
}

static void zx_numa_cleanup(struct work_struct *work)
{
	int i = 0;
	int checktimes = 2;

	//reboot or power off state
	if (READ_ONCE(enable_zx_numa_osq_lock) == 0xf)
		return;

	if (atomic_read(&numa_count) == 0) {
		if (READ_ONCE(dynamic_enable) != 0)
			schedule_delayed_work(&zx_numa_cleanup_work, 60*HZ);
		return;
	}

	for (i = 0; i < zx_numa_lock_total; i++) {
		int s = 0;
		u32 lockaddr = READ_ONCE(zx_numa_entry[i].lockaddr);
		u32 type = zx_numa_entry[i].type;
		struct _numa_lock *buf =  zx_numa_entry[i].numa_ptr;
		int nodes = 0;

		if (lockaddr == 0 || type == 3 || zx_numa_entry[i].idle == 0)
			continue;
		nodes = buf->numa_nodes;
		if (zx_numa_entry[i].idle < checktimes) {

			s = zx_check_numa_dynamic_locked(lockaddr, buf, 1);
			if (s != 0) {
				zx_numa_entry[i].idle = 1;
				continue;
			}
			zx_numa_entry[i].idle++;
		}

		if (zx_numa_entry[i].idle == checktimes) {
			zx_numa_lock_stopping(buf);
			zx_numa_entry[i].idle++;

		}

		if (zx_numa_entry[i].idle == checktimes+1) {
			while (1) {
				if (zx_numa_lock64_try_to_freeze(lockaddr, buf,
						i) == nodes + 1) {
					//all node has been locked
					u32 left = 0;

					left = atomic_dec_return(&numa_count);
					break;
				}
				cpu_relax(); cpu_relax();
				cpu_relax(); cpu_relax();
			}
		}
	}
	schedule_delayed_work(&zx_numa_cleanup_work, 60*HZ);
}

static int create_numa_buffer_list(int clusters, int len)
{
	int i = 0;

	for (i = 0; i < zx_numa_lock_total; i++) {
		struct _numa_lock *s = (struct _numa_lock *)kmem_cache_alloc(
				zx_numa_lock_cachep, GFP_KERNEL);
		if (!s) {
			while (i > 0) {
				kmem_cache_free(zx_numa_lock_cachep,
						zx_numa_entry[i-1].numa_ptr);
				i--;
			}
			return 0;
		}
		memset((char *)s, 0,
			len * L1_CACHE_BYTES * (clusters + NUMAEXPAND));
		numa_lock_init_data(s, clusters, NUMA_LOCKED_VAL, 0);
		zx_numa_entry[i].numa_ptr = s;
		zx_numa_entry[i].lockaddr = 0;
		zx_numa_entry[i].highaddr = 0;
		zx_numa_entry[i].idle = 0;
		zx_numa_entry[i].type = 0;
	}

	for (i = 0; i < zx_numa_lock_total; i++) {
		zx_numa_entry[i].index = i;
		list_add_tail(&(zx_numa_entry[i].list), &_zx_numa_lock_head);
	}
	return 1;
}

static int zx_numa_lock_init(int numa)
{
	int align = max_t(int, L1_CACHE_BYTES, ARCH_MIN_TASKALIGN);
	int d = 0;
	int status = 0;

	atomic_set(&lockindex, 0);
	atomic_set(&numa_count, 0);

	if (sizeof(struct _numa_lock) & 0x3f)
		d = (int)((sizeof(struct _numa_lock) + L1_CACHE_BYTES) /
			  L1_CACHE_BYTES);
	else
		d = (int)(sizeof(struct _numa_lock) / L1_CACHE_BYTES);

	zx_numa_entry_cachep = kmem_cache_create(
		"zx_numa_entry",
		sizeof(struct _numa_buf) * zx_numa_lock_total, align,
		SLAB_PANIC | SLAB_ACCOUNT, NULL);

	zx_numa_lock_cachep = kmem_cache_create(
		"zx_numa_lock",
		d * L1_CACHE_BYTES * (numa + NUMAEXPAND), align,
		SLAB_PANIC | SLAB_ACCOUNT, NULL);


	if (zx_numa_entry_cachep && zx_numa_lock_cachep) {
		zx_numa_entry = (struct _numa_buf *)kmem_cache_alloc(
				zx_numa_entry_cachep, GFP_KERNEL);
		if (zx_numa_entry) {
			memset((char *)zx_numa_entry, 0,
				sizeof(struct _numa_buf) * zx_numa_lock_total);
			create_numa_buffer_list(numa, d);
			status = 1;
		}
	}

	pr_info("enable dynamic numa-aware osq_lock, clusters %d\n",
		numa);
	return status;
}


#define numa_lock_proc_dir "zx_numa_lock"
#define zx_numa_enable_dir "dynamic_enable"
#define numa_entry_total 8
struct proc_dir_entry *numa_lock_proc;
struct proc_dir_entry *numa_lock_enable;
struct proc_dir_entry *numa_proc_entry[numa_entry_total];

static ssize_t numa_lock_proc_read(struct file *file,
		char __user *usrbuf, size_t len, loff_t *off)
{
	int id = (long) pde_data(file_inode(file));
	char kbuffer[128];
	ssize_t retval = 0;
	size_t n = 0;

	memset(kbuffer, 0, sizeof(kbuffer));
	if (id == 0)
		n = sprintf(kbuffer, "%d\n", READ_ONCE(dynamic_enable));
	else if (id == 1)
		n = sprintf(kbuffer, "%d\n", READ_ONCE(osq_lock_depth));
	else if (id == 2)
		n = sprintf(kbuffer, "%d\n", READ_ONCE(osq_keep_times));
	else if (id == 3)
		n = sprintf(kbuffer, "%d\n", READ_ONCE(osq_node_max));
	else if (id == 4)
		n = sprintf(kbuffer, "%d\n", atomic_read(&numa_count));
	retval = simple_read_from_buffer(usrbuf, len, off, kbuffer, n);

	return retval;
}

static ssize_t numa_lock_proc_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *f_pos)
{
	int id = (long) pde_data(file_inode(file));
	char kbuffer[128];
	unsigned long new = 0;
	int err = 0;

	memset(kbuffer, 0, sizeof(kbuffer));
	if (copy_from_user(kbuffer, buffer, count))
		return count;
	kbuffer[count] = '\0';
	err = kstrtoul(kbuffer, 10, &new);

	if (id == 0) {
		int last = READ_ONCE(dynamic_enable);

		if (new < 0 || new >= 2 || last == new)
			return count;

		if (last == 0) {
			prefetchw(&enable_zx_numa_osq_lock);
			//enable to the 2-bytes-tail osq-lock
			prefetchw(&enable_zx_numa_osq_lock);
			WRITE_ONCE(enable_zx_numa_osq_lock, 2);
			schedule_delayed_work(&zx_numa_cleanup_work, 60*HZ);
		}
		prefetchw(&dynamic_enable);
		WRITE_ONCE(dynamic_enable, new);
		return count;
	}

	if (READ_ONCE(dynamic_enable) != 0) {
		pr_info("dynamic %d: change setting should disable dynamic\n",
			dynamic_enable);
		return count;
	}
	if (id == 1 && new > 4 && new <= 32)
		WRITE_ONCE(osq_lock_depth, new);
	else if (id == 2 && new >= 16 && new <= 2048)
		WRITE_ONCE(osq_keep_times, new);
	else if (id == 3 && new > 4 && new <= 2048)
		WRITE_ONCE(osq_node_max, new);
	return count;
}
static int numa_lock_proc_show(struct seq_file *m, void *v)
{
	return 0;
}

static int numa_lock_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, numa_lock_proc_show, NULL);
}
static const struct proc_ops numa_lock_proc_fops = {
	.proc_open = numa_lock_proc_open,
	.proc_read = numa_lock_proc_read,
	.proc_write = numa_lock_proc_write
};

static int numalock_proc_init(void)
{
	int index = 0;
	int i = 0;

	numa_lock_proc = proc_mkdir(numa_lock_proc_dir, NULL);
	if (numa_lock_proc == NULL) {
		pr_info("%s proc create %s failed\n", __func__,
				numa_lock_proc_dir);
		return -EINVAL;
	}

	numa_lock_enable = proc_create_data(zx_numa_enable_dir, 0666,
		numa_lock_proc, &numa_lock_proc_fops, (void *)(long)index++);
	if (!numa_lock_enable) {
		pr_info("%s proc_create_data %s failed!\n", __func__,
				zx_numa_enable_dir);
		return -ENOMEM;
	}

	for (i = 0; i < numa_entry_total; i++)
		numa_proc_entry[i] = NULL;

	numa_proc_entry[0] =  proc_create_data("osq_lock_depth", 0664,
		numa_lock_proc, &numa_lock_proc_fops, (void *)(long)index++);
	numa_proc_entry[1] =  proc_create_data("osq_keep_times", 0664,
		numa_lock_proc, &numa_lock_proc_fops, (void *)(long)index++);
	numa_proc_entry[2] =  proc_create_data("osq_node_max", 0664,
		numa_lock_proc, &numa_lock_proc_fops, (void *)(long)index++);
	numa_proc_entry[3] =  proc_create_data("numa_osq_lock", 0444,
		numa_lock_proc, &numa_lock_proc_fops, (void *)(long)index++);
	return 0;
}

static void numalock_proc_exit(void)
{
	int i = 0;

	for (i = 0; i < numa_entry_total; i++) {
		if (numa_proc_entry[i])
			proc_remove(numa_proc_entry[i]);
	}
	if (numa_lock_enable)
		proc_remove(numa_lock_enable);
	if (numa_lock_proc)
		remove_proc_entry(numa_lock_proc_dir, NULL);

}

static int numalock_shutdown_notify(struct notifier_block *unused1,
		unsigned long unused2, void *unused3)
{
	if (READ_ONCE(enable_zx_numa_osq_lock) == 2) {
		WRITE_ONCE(dynamic_enable, 0);
		WRITE_ONCE(enable_zx_numa_osq_lock, 0xf);
	}
	return NOTIFY_DONE;
}
static struct notifier_block numalock_shutdown_nb = {
	.notifier_call = numalock_shutdown_notify,
};
static int __init zx_numa_base_init(void)
{
	int cpu = num_possible_cpus();
	int i = 0;

	WRITE_ONCE(enable_zx_numa_osq_lock, 0);
	if (kvm_para_available())
		return 0;
	if (cpu >= 65534 || cpu < 16 || (cpu & 0x7) != 0)
		return 0;

	for (i = 0; i < ARRAY_SIZE(numa_cpu_list); i++) {
		if (boot_cpu_data.x86_vendor == numa_cpu_list[i].x86_vendor &&
			boot_cpu_data.x86 == numa_cpu_list[i].x86 &&
			boot_cpu_data.x86_model == numa_cpu_list[i].x86_model) {

			if (numa_cpu_list[i].feature1 == 1)
				NUMACLUSTERS = nr_node_ids + nr_node_ids;
			NUMASHIFT = zx_get_numa_shift(num_possible_cpus(),
					NUMACLUSTERS);

			if (zx_numa_lock_init(NUMACLUSTERS) == 0)
				return -ENOMEM;
			register_reboot_notifier(&numalock_shutdown_nb);
			numalock_proc_init();
			INIT_DELAYED_WORK(&zx_numa_cleanup_work,
				zx_numa_cleanup);
			prefetchw(&enable_zx_numa_osq_lock);
			WRITE_ONCE(enable_zx_numa_osq_lock, 1);
			return 0;
		}
	}
	return 0;
}

static void __exit zx_numa_lock_exit(void)
{
	numalock_proc_exit();
	prefetchw(&dynamic_enable);
	WRITE_ONCE(dynamic_enable, 0);
}

late_initcall(zx_numa_base_init);
module_exit(zx_numa_lock_exit);
MODULE_AUTHOR("LiYong <yongli-oc@zhaoxin.com>");
MODULE_DESCRIPTION("zx dynamic numa-aware osq lock");
MODULE_LICENSE("GPL");

