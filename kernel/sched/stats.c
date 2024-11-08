// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/schedstat implementation
 */

void __update_stats_wait_start(struct rq *rq, struct task_struct *p,
			       struct sched_statistics *stats)
{
	u64 wait_start, prev_wait_start;

	wait_start = rq_clock(rq);
	prev_wait_start = schedstat_val(stats->wait_start);

	if (p && likely(wait_start > prev_wait_start))
		wait_start -= prev_wait_start;

	__schedstat_set(stats->wait_start, wait_start);
}

void __update_stats_wait_end(struct rq *rq, struct task_struct *p,
			     struct sched_statistics *stats)
{
	u64 delta = rq_clock(rq) - schedstat_val(stats->wait_start);

	if (p) {
		if (task_on_rq_migrating(p)) {
			/*
			 * Preserve migrating task's wait time so wait_start
			 * time stamp can be adjusted to accumulate wait time
			 * prior to migration.
			 */
			__schedstat_set(stats->wait_start, delta);

			return;
		}

		trace_sched_stat_wait(p, delta);
	}

	__schedstat_set(stats->wait_max,
			max(schedstat_val(stats->wait_max), delta));
	__schedstat_inc(stats->wait_count);
	__schedstat_add(stats->wait_sum, delta);
	__schedstat_set(stats->wait_start, 0);
}

void __update_stats_enqueue_sleeper(struct rq *rq, struct task_struct *p,
				    struct sched_statistics *stats)
{
	u64 sleep_start, block_start;

	sleep_start = schedstat_val(stats->sleep_start);
	block_start = schedstat_val(stats->block_start);

	if (sleep_start) {
		u64 delta = rq_clock(rq) - sleep_start;

		if ((s64)delta < 0)
			delta = 0;

		if (unlikely(delta > schedstat_val(stats->sleep_max)))
			__schedstat_set(stats->sleep_max, delta);

		__schedstat_set(stats->sleep_start, 0);
		__schedstat_add(stats->sum_sleep_runtime, delta);

		if (p) {
			account_scheduler_latency(p, delta >> 10, 1);
			trace_sched_stat_sleep(p, delta);
		}
	}

	if (block_start) {
		u64 delta = rq_clock(rq) - block_start;

		if ((s64)delta < 0)
			delta = 0;

		if (unlikely(delta > schedstat_val(stats->block_max)))
			__schedstat_set(stats->block_max, delta);

		__schedstat_set(stats->block_start, 0);
		__schedstat_add(stats->sum_sleep_runtime, delta);
		__schedstat_add(stats->sum_block_runtime, delta);

		if (p) {
			if (p->in_iowait) {
				__schedstat_add(stats->iowait_sum, delta);
				__schedstat_inc(stats->iowait_count);
				trace_sched_stat_iowait(p, delta);
			}

			trace_sched_stat_blocked(p, delta);

			account_scheduler_latency(p, delta >> 10, 0);
		}
	}
}

/*
 * Current schedstat API version.
 *
 * Bump this up when changing the output format or the meaning of an existing
 * format, so that tools can adapt (or abort)
 */
#define SCHEDSTAT_VERSION 16

static int show_schedstat(struct seq_file *seq, void *v)
{
	int cpu;

	if (v == (void *)1) {
		seq_printf(seq, "version %d\n", SCHEDSTAT_VERSION);
		seq_printf(seq, "timestamp %lu\n", jiffies);
	} else {
		struct rq *rq;
#ifdef CONFIG_SMP
		struct sched_domain *sd;
		int dcount = 0;
#endif
		cpu = (unsigned long)(v - 2);
		rq = cpu_rq(cpu);

		/* runqueue-specific stats */
		seq_puts(seq, "cpu");
		seq_put_decimal_ull(seq, "", cpu);
		seq_put_decimal_ull(seq, " ", rq->yld_count);
		seq_put_decimal_ull(seq, " ", 0);
		seq_put_decimal_ull(seq, " ", rq->sched_count);
		seq_put_decimal_ull(seq, " ", rq->sched_goidle);
		seq_put_decimal_ull(seq, " ", rq->ttwu_count);
		seq_put_decimal_ull(seq, " ", rq->ttwu_local);
		seq_put_decimal_ull(seq, " ", rq->rq_cpu_time);
		seq_put_decimal_ull(seq, " ", rq->rq_sched_info.run_delay);
		seq_put_decimal_ull(seq, " ", rq->rq_sched_info.pcount);
		seq_putc(seq, '\n');

#ifdef CONFIG_SMP
		/* domain-specific stats */
		rcu_read_lock();
		for_each_domain(cpu, sd) {
			enum cpu_idle_type itype;

			seq_puts(seq, "domain");
			seq_put_decimal_ull(seq, "", dcount++);
			seq_printf(seq, " %*pb",
				   cpumask_pr_args(sched_domain_span(sd)));
			for (itype = 0; itype < CPU_MAX_IDLE_TYPES; itype++) {
				seq_put_decimal_ull(seq, " ", sd->lb_count[itype]);
				seq_put_decimal_ull(seq, " ", sd->lb_balanced[itype]);
				seq_put_decimal_ull(seq, " ", sd->lb_failed[itype]);
				seq_put_decimal_ull(seq, " ", sd->lb_imbalance[itype]);
				seq_put_decimal_ull(seq, " ", sd->lb_gained[itype]);
				seq_put_decimal_ull(seq, " ", sd->lb_hot_gained[itype]);
				seq_put_decimal_ull(seq, " ", sd->lb_nobusyq[itype]);
				seq_put_decimal_ull(seq, " ", sd->lb_nobusyg[itype]);
			}
			seq_put_decimal_ull(seq, " ", sd->alb_count);
			seq_put_decimal_ull(seq, " ", sd->alb_failed);
			seq_put_decimal_ull(seq, " ", sd->alb_pushed);
			seq_put_decimal_ull(seq, " ", sd->sbe_count);
			seq_put_decimal_ull(seq, " ", sd->sbe_balanced);
			seq_put_decimal_ull(seq, " ", sd->sbe_pushed);
			seq_put_decimal_ull(seq, " ", sd->sbf_count);
			seq_put_decimal_ull(seq, " ", sd->sbf_balanced);
			seq_put_decimal_ull(seq, " ", sd->sbf_pushed);
			seq_put_decimal_ull(seq, " ", sd->ttwu_wake_remote);
			seq_put_decimal_ull(seq, " ", sd->ttwu_move_affine);
			seq_put_decimal_ull(seq, " ", sd->ttwu_move_balance);
			seq_putc(seq, '\n');
		}
		rcu_read_unlock();
#endif
	}
	return 0;
}

/*
 * This iterator needs some explanation.
 * It returns 1 for the header position.
 * This means 2 is cpu 0.
 * In a hotplugged system some CPUs, including cpu 0, may be missing so we have
 * to use cpumask_* to iterate over the CPUs.
 */
static void *schedstat_start(struct seq_file *file, loff_t *offset)
{
	unsigned long n = *offset;

	if (n == 0)
		return (void *) 1;

	n--;

	if (n > 0)
		n = cpumask_next(n - 1, cpu_online_mask);
	else
		n = cpumask_first(cpu_online_mask);

	*offset = n + 1;

	if (n < nr_cpu_ids)
		return (void *)(unsigned long)(n + 2);

	return NULL;
}

static void *schedstat_next(struct seq_file *file, void *data, loff_t *offset)
{
	(*offset)++;

	return schedstat_start(file, offset);
}

static void schedstat_stop(struct seq_file *file, void *data)
{
}

static const struct seq_operations schedstat_sops = {
	.start = schedstat_start,
	.next  = schedstat_next,
	.stop  = schedstat_stop,
	.show  = show_schedstat,
};

static int __init proc_schedstat_init(void)
{
	proc_create_seq("schedstat", 0, NULL, &schedstat_sops);
	return 0;
}
subsys_initcall(proc_schedstat_init);
