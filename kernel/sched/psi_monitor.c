// SPDX-License-Identifier: GPL-2.0
/*
 * PSI Automatic Monitor with Weighted Task Ranking + Tracepoints
 *
 * Periodically samples system PSI (CPU, memory, IO) and, when any
 * configured threshold is exceeded, ranks tasks using a composite
 * score based on RSS, I/O activity and CPU time, then logs the
 * top-N tasks via printk and a tracepoint.
 *
 * Sysfs interface:
 *   /sys/kernel/psi_monitor/cpu_thresh		 (percentage)
 *   /sys/kernel/psi_monitor/mem_thresh		 (percentage)
 *   /sys/kernel/psi_monitor/io_thresh		 (percentage)
 *   /sys/kernel/psi_monitor/monitor_interval_ms (milliseconds)
 *   /sys/kernel/psi_monitor/rss_weight
 *   /sys/kernel/psi_monitor/io_weight
 *   /sys/kernel/psi_monitor/cpu_weight
 *
 * Author: Pintu Kumar Agarwal
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/loadavg.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/psi_types.h>
#include <linux/kobject.h>
#include <linux/sort.h>
#include <linux/jiffies.h>
#include <linux/time64.h>
#include <linux/sched/cputime.h>

/* Create tracepoints defined in include/trace/events/psi_monitor.h */
#define CREATE_TRACE_POINTS
#include <linux/psi.h>
#include <trace/events/psi_monitor.h>


/* Sysfs tunables */
static unsigned int cpu_thresh = 80;	  /* in percent */
static unsigned int mem_thresh = 80;	  /* in percent */
static unsigned int io_thresh  = 80;	  /* in percent */
static unsigned int monitor_interval_ms = 10000;

/* scoring weights */
static unsigned int rss_weight = 2;
static unsigned int io_weight  = 1;
static unsigned int cpu_weight = 5;

static struct delayed_work psi_work;
static struct kobject *psi_kobj;

#define TOP_N 20

struct task_info {
	struct task_struct *task;
	unsigned long rss;      /* pages */
	unsigned long io_kb;    /* kB */
	unsigned long cpu_ms;   /* ms */
	u64 score;
};

/*
 * psi_avg10_percent() - derive a rough integer percentage from avg10
 * for a given PSI state (e.g. PSI_CPU_SOME, PSI_MEM_SOME, PSI_IO_SOME).
 *
 * psi_group.avg[state][0] is the avg10 window in fixed-point notation.
 * The conversion here is approximate but monotonic, which is sufficient
 * for thresholding and ranking in this internal monitor.
 */
static unsigned long psi_avg10_percent(int state)
{
	u64 avg10;

	if (state < 0 || state >= NR_PSI_STATES)
		return 0;

	avg10 = READ_ONCE(psi_system.avg[state][0]);
	if (!avg10)
		return 0;

	/* Convert back from loadavg-style fixed-point to an approximate % */
	/* Just consider the integer value and ignore fraction */
	return LOAD_INT(avg10);
}

static int compare_score_desc(const void *a, const void *b)
{
	const struct task_info *ta = a;
	const struct task_info *tb = b;

	if (tb->score > ta->score)
		return 1;
	if (tb->score < ta->score)
		return -1;
	return 0;
}

static void log_top_tasks(void)
{
	struct task_info tasks[TOP_N];
	struct task_struct *p, *t;
	int count = 0;
	int i;

	rcu_read_lock();
	for_each_process_thread(p, t) {
		struct mm_struct *mm;
		unsigned long rss = 0;
		unsigned long io_kb = 0;
		unsigned long cpu_ms = 0;
		u64 score;

		/* Ignore tasks that are not on run queue or idle */
		if (!t->on_rq && !is_idle_task(t))
			continue;

		mm = get_task_mm(t);

		/* mm could be NULL for kernel threads */
		if (mm) {
			rss = mm ? get_mm_rss(mm) : 0;
			mmput_async(mm);
		}

		/*
		 * Approximate I/O activity: sum of read + write bytes.
		 * This uses the task_io_accounting fields in task_struct.
		 * Values are best-effort and need not be perfectly accurate
		 * for our ranking purpose.
		 */
		io_kb = (t->ioac.read_bytes + t->ioac.write_bytes) >> 10;

		/*
		 * Approximate CPU usage via task_sched_runtime(), converted
		 * to milliseconds. This is cumulative since task start, but
		 * is still useful for comparing hotspots at a given point.
		 */
		cpu_ms = (unsigned long)(task_sched_runtime(t) / NSEC_PER_MSEC);

		score = (u64)rss_weight * (u64)rss +
			(u64)io_weight  * (u64)io_kb +
			(u64)cpu_weight * (u64)cpu_ms;

		if (count < TOP_N) {
			tasks[count].task   = t;
			tasks[count].rss    = rss;
			tasks[count].io_kb  = io_kb;
			tasks[count].cpu_ms = cpu_ms;
			tasks[count].score  = score;
			count++;
		} else {
			/* Maintain a simple streaming top-N: replace smallest */
			int min_idx = 0;
			int j;

			for (j = 1; j < TOP_N; j++) {
				if (tasks[j].score < tasks[min_idx].score)
					min_idx = j;
			}

			if (score > tasks[min_idx].score) {
				tasks[min_idx].task   = t;
				tasks[min_idx].rss    = rss;
				tasks[min_idx].io_kb  = io_kb;
				tasks[min_idx].cpu_ms = cpu_ms;
				tasks[min_idx].score  = score;
			}
		}
	}
	rcu_read_unlock();

	sort(tasks, count, sizeof(struct task_info), compare_score_desc, NULL);

	pr_info("psi_monitor: logging top %d tasks under pressure:\n", count);

	for (i = 0; i < count; i++) {
		struct task_struct *ts = tasks[i].task;
		unsigned long rss_kb = tasks[i].rss << (PAGE_SHIFT - 10);
		char name[128] = {0,};

		if (ts->flags & PF_WQ_WORKER)
			wq_worker_comm(name, sizeof(name), ts);
		else
			scnprintf(name, sizeof(name) - 1, ts->comm);

		trace_psi_monitor_top_task(ts->pid, name,
				tasks[i].cpu_ms,
				rss_kb,
				tasks[i].io_kb,
				tasks[i].score);

		pr_info("psi_monitor: pid=%d comm=%s psi_flag=%d oncpu=%d cputime(ms)=%lu rss(kB)=%lu io(kB)=%lu score=%llu\n",
			ts->pid, name, ts->psi_flags, task_cpu(ts),
			tasks[i].cpu_ms, rss_kb, tasks[i].io_kb,
			(unsigned long long)tasks[i].score);
		}
}

static void psi_monitor_fn(struct work_struct *work)
{
	unsigned long cpu_pct, mem_pct, io_pct;
	bool trigger = false;

	cpu_pct = psi_avg10_percent(PSI_CPU_SOME);
	mem_pct = psi_avg10_percent(PSI_MEM_SOME);
	io_pct  = psi_avg10_percent(PSI_IO_SOME);

	if (cpu_pct >= cpu_thresh || mem_pct >= mem_thresh ||
		io_pct >= io_thresh)
		trigger = true;

	if (trigger) {
		pr_info("psi_monitor: pressure high: cpu=%lu%% mem=%lu%% io=%lu%% (thresh cpu=%u mem=%u io=%u)\n",
			cpu_pct, mem_pct, io_pct,
			cpu_thresh, mem_thresh, io_thresh);
		log_top_tasks();
	}

	queue_delayed_work(system_wq, &psi_work,
		msecs_to_jiffies(monitor_interval_ms));
}

/* Sysfs helpers */
#define PSI_ATTR_RW(_name)						\
static ssize_t _name##_show(struct kobject *kobj,			\
			struct kobj_attribute *attr, char *buf)		\
{									\
	return sysfs_emit(buf, "%u\n", _name);				\
}									\
static ssize_t _name##_store(struct kobject *kobj,			\
			    struct kobj_attribute *attr,		\
			    const char *buf, size_t count)		\
{									\
	unsigned int val;						\
	if (kstrtouint(buf, 10, &val))					\
		return -EINVAL;						\
	_name = val;							\
	return count;							\
}									\
static struct kobj_attribute _name##_attr = __ATTR_RW(_name)

PSI_ATTR_RW(cpu_thresh);
PSI_ATTR_RW(mem_thresh);
PSI_ATTR_RW(io_thresh);
PSI_ATTR_RW(monitor_interval_ms);
PSI_ATTR_RW(rss_weight);
PSI_ATTR_RW(io_weight);
PSI_ATTR_RW(cpu_weight);

static struct attribute *psi_attrs[] = {
	&cpu_thresh_attr.attr,
	&mem_thresh_attr.attr,
	&io_thresh_attr.attr,
	&monitor_interval_ms_attr.attr,
	&rss_weight_attr.attr,
	&io_weight_attr.attr,
	&cpu_weight_attr.attr,
	NULL,
};

static const struct attribute_group psi_attr_group = {
	.attrs = psi_attrs,
};

static int __init psi_monitor_init(void)
{
	int ret;

	INIT_DELAYED_WORK(&psi_work, psi_monitor_fn);
	queue_delayed_work(system_wq, &psi_work,
			msecs_to_jiffies(monitor_interval_ms));

	psi_kobj = kobject_create_and_add("psi_monitor", kernel_kobj);
	if (!psi_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(psi_kobj, &psi_attr_group);
	if (ret) {
		kobject_put(psi_kobj);
		cancel_delayed_work_sync(&psi_work);
		return ret;
	}

	pr_info("psi_monitor: in-kernel PSI auto monitor (weighted + tracepoints) loaded\n");
	return 0;
}

static void __exit psi_monitor_exit(void)
{
	cancel_delayed_work_sync(&psi_work);
	if (psi_kobj)
		kobject_put(psi_kobj);
	pr_info("psi_monitor: unloaded\n");
}

module_init(psi_monitor_init);
module_exit(psi_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pintu Kumar Agarwal");
MODULE_DESCRIPTION("In-kernel PSI automatic monitor with sysfs, weighted scoring and tracepoints");
