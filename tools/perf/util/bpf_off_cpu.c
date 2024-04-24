// SPDX-License-Identifier: GPL-2.0
#include "util/bpf_counter.h"
#include "util/debug.h"
#include "util/evsel.h"
#include "util/evlist.h"
#include "util/off_cpu.h"
#include "util/perf-hooks.h"
#include "util/record.h"
#include "util/session.h"
#include "util/target.h"
#include "util/cpumap.h"
#include "util/thread_map.h"
#include "util/cgroup.h"
#include "util/strlist.h"
#include "util/mmap.h"
#include "util/sample.h"
#include <perf/mmap.h>
#include <bpf/bpf.h>

#include "bpf_skel/off_cpu.skel.h"

#define MAX_STACKS  32
#define MAX_PROC  4096
/* we don't need actual timestamp, just want to put the samples at last */
#define OFF_CPU_TIMESTAMP  (~0ull << 32)

static struct off_cpu_bpf *skel;

static void off_cpu_start(void *arg)
{
	struct evlist *evlist = arg;

	/* update task filter for the given workload */
	if (!skel->bss->has_cpu && !skel->bss->has_task &&
	    perf_thread_map__pid(evlist->core.threads, 0) != -1) {
		int fd;
		u32 pid;
		u8 val = 1;

		skel->bss->has_task = 1;
		skel->bss->uses_tgid = 1;
		fd = bpf_map__fd(skel->maps.task_filter);
		pid = perf_thread_map__pid(evlist->core.threads, 0);
		bpf_map_update_elem(fd, &pid, &val, BPF_ANY);
	}

	skel->bss->enabled = 1;
}

static void off_cpu_finish(void *arg __maybe_unused)
{
	skel->bss->enabled = 0;
	off_cpu_bpf__destroy(skel);
}

/* v5.18 kernel added prev_state arg, so it needs to check the signature */
static void check_sched_switch_args(void)
{
	struct btf *btf = btf__load_vmlinux_btf();
	const struct btf_type *t1, *t2, *t3;
	u32 type_id;

	type_id = btf__find_by_name_kind(btf, "btf_trace_sched_switch",
					 BTF_KIND_TYPEDEF);
	if ((s32)type_id < 0)
		goto cleanup;

	t1 = btf__type_by_id(btf, type_id);
	if (t1 == NULL)
		goto cleanup;

	t2 = btf__type_by_id(btf, t1->type);
	if (t2 == NULL || !btf_is_ptr(t2))
		goto cleanup;

	t3 = btf__type_by_id(btf, t2->type);
	/* btf_trace func proto has one more argument for the context */
	if (t3 && btf_is_func_proto(t3) && btf_vlen(t3) == 5) {
		/* new format: pass prev_state as 4th arg */
		skel->rodata->has_prev_state = true;
	}
cleanup:
	btf__free(btf);
}

int off_cpu_prepare_parse(struct evlist *evlist)
{
	struct evsel *evsel;

	evsel = evlist__find_evsel_by_str(evlist, OFFCPU_EVENT);
	if (evsel == NULL)
		return -1;

	evsel->core.attr.sample_type = OFFCPU_SAMPLE_TYPES;

	return 0;
}

int off_cpu_prepare(struct evlist *evlist, struct target *target,
		    struct record_opts *opts)
{
	int err, fd, i;
	int ncpus = 1, ntasks = 1, ncgrps = 1;
	u64 sid = 0;
	struct strlist *pid_slist = NULL;
	struct str_node *pos;
	struct evsel *evsel;
	struct perf_cpu pcpu;

	skel = off_cpu_bpf__open();
	if (!skel) {
		pr_err("Failed to open off-cpu BPF skeleton\n");
		return -1;
	}

	/* don't need to set cpu filter for system-wide mode */
	if (target->cpu_list) {
		ncpus = perf_cpu_map__nr(evlist->core.user_requested_cpus);
		bpf_map__set_max_entries(skel->maps.cpu_filter, ncpus);
	}

	if (target->pid) {
		pid_slist = strlist__new(target->pid, NULL);
		if (!pid_slist) {
			pr_err("Failed to create a strlist for pid\n");
			return -1;
		}

		ntasks = 0;
		strlist__for_each_entry(pos, pid_slist) {
			char *end_ptr;
			int pid = strtol(pos->s, &end_ptr, 10);

			if (pid == INT_MIN || pid == INT_MAX ||
			    (*end_ptr != '\0' && *end_ptr != ','))
				continue;

			ntasks++;
		}

		if (ntasks < MAX_PROC)
			ntasks = MAX_PROC;

		bpf_map__set_max_entries(skel->maps.task_filter, ntasks);
	} else if (target__has_task(target)) {
		ntasks = perf_thread_map__nr(evlist->core.threads);
		bpf_map__set_max_entries(skel->maps.task_filter, ntasks);
	} else if (target__none(target)) {
		bpf_map__set_max_entries(skel->maps.task_filter, MAX_PROC);
	}

	if (evlist__first(evlist)->cgrp) {
		ncgrps = evlist->core.nr_entries - 1; /* excluding a dummy */
		bpf_map__set_max_entries(skel->maps.cgroup_filter, ncgrps);

		if (!cgroup_is_v2("perf_event"))
			skel->rodata->uses_cgroup_v1 = true;
	}

	if (opts->record_cgroup) {
		skel->rodata->needs_cgroup = true;

		if (!cgroup_is_v2("perf_event"))
			skel->rodata->uses_cgroup_v1 = true;
	}

	set_max_rlimit();
	check_sched_switch_args();

	err = off_cpu_bpf__load(skel);
	if (err) {
		pr_err("Failed to load off-cpu skeleton\n");
		goto out;
	}

	if (target->cpu_list) {
		u32 cpu;
		u8 val = 1;

		skel->bss->has_cpu = 1;
		fd = bpf_map__fd(skel->maps.cpu_filter);

		for (i = 0; i < ncpus; i++) {
			cpu = perf_cpu_map__cpu(evlist->core.user_requested_cpus, i).cpu;
			bpf_map_update_elem(fd, &cpu, &val, BPF_ANY);
		}
	}

	if (target->pid) {
		u8 val = 1;

		skel->bss->has_task = 1;
		skel->bss->uses_tgid = 1;
		fd = bpf_map__fd(skel->maps.task_filter);

		strlist__for_each_entry(pos, pid_slist) {
			char *end_ptr;
			u32 tgid;
			int pid = strtol(pos->s, &end_ptr, 10);

			if (pid == INT_MIN || pid == INT_MAX ||
			    (*end_ptr != '\0' && *end_ptr != ','))
				continue;

			tgid = pid;
			bpf_map_update_elem(fd, &tgid, &val, BPF_ANY);
		}
	} else if (target__has_task(target)) {
		u32 pid;
		u8 val = 1;

		skel->bss->has_task = 1;
		fd = bpf_map__fd(skel->maps.task_filter);

		for (i = 0; i < ntasks; i++) {
			pid = perf_thread_map__pid(evlist->core.threads, i);
			bpf_map_update_elem(fd, &pid, &val, BPF_ANY);
		}
	}

	if (evlist__first(evlist)->cgrp) {
		u8 val = 1;

		skel->bss->has_cgroup = 1;
		fd = bpf_map__fd(skel->maps.cgroup_filter);

		evlist__for_each_entry(evlist, evsel) {
			struct cgroup *cgrp = evsel->cgrp;

			if (cgrp == NULL)
				continue;

			if (!cgrp->id && read_cgroup_id(cgrp) < 0) {
				pr_err("Failed to read cgroup id of %s\n",
				       cgrp->name);
				goto out;
			}

			bpf_map_update_elem(fd, &cgrp->id, &val, BPF_ANY);
		}
	}

	evsel = evlist__find_evsel_by_str(evlist, OFFCPU_EVENT);
	if (evsel == NULL) {
		pr_err("%s evsel not found\n", OFFCPU_EVENT);
		goto out;
	}

	if (evsel->core.id)
		sid = evsel->core.id[0];

	skel->bss->sample_id = sid;
	skel->bss->sample_type = OFFCPU_SAMPLE_TYPES;

	perf_cpu_map__for_each_cpu(pcpu, i, evsel->core.cpus) {
		bpf_map__update_elem(skel->maps.offcpu_output,
				     &pcpu.cpu, sizeof(int),
				     xyarray__entry(evsel->core.fd, pcpu.cpu, 0),
				     sizeof(__u32), BPF_ANY);
	}

	err = off_cpu_bpf__attach(skel);
	if (err) {
		pr_err("Failed to attach off-cpu BPF skeleton\n");
		goto out;
	}

	if (perf_hooks__set_hook("record_start", off_cpu_start, evlist) ||
	    perf_hooks__set_hook("record_done", off_cpu_finish, evlist)) {
		pr_err("Failed to attach off-cpu skeleton\n");
		goto out;
	}

	return 0;

out:
	off_cpu_bpf__destroy(skel);
	return -1;
}

ssize_t off_cpu_strip(struct evlist *evlist, struct mmap *mp, char *dst, size_t size)
{
	/*
	 * In this function, we read events one by one,
	 * stripping actual samples from raw data.
	 */

	union perf_event *event, tmp;
	u64 sample_type = OFFCPU_SAMPLE_TYPES;
	size_t written = 0, event_sz, write_sz, raw_sz_aligned, offset = 0;
	void *src;
	int err = 0, n = 0;
	struct perf_sample sample;
	struct evsel *evsel;

	evsel = evlist__find_evsel_by_str(evlist, OFFCPU_EVENT);
	if (evsel == NULL) {
		pr_err("%s evsel not found\n", OFFCPU_EVENT);
		return -1;
	}

	/* for writing sample time*/
	if (sample_type & PERF_SAMPLE_IDENTIFIER)
		++n;
	if (sample_type & PERF_SAMPLE_IP)
		++n;
	if (sample_type & PERF_SAMPLE_TID)
		++n;

	/* no need for perf_mmap__consume(), it will be handled by perf_mmap__push() */
	while ((event = perf_mmap__read_event(&mp->core)) != NULL) {
		event_sz = event->header.size;
		write_sz = event_sz;
		src = event;

		if (event->header.type == PERF_RECORD_SAMPLE) {
			err = evlist__parse_sample(evlist, event, &sample);
			if (err) {
				pr_err("Failed to parse off-cpu sample\n");
				return -1;
			}

			if (sample.raw_data && evsel->core.id) {
				bool flag = false;

				for (u32 i = 0; i < evsel->core.ids; i++) {
					if (sample.id == evsel->core.id[i]) {
						flag = true;
						break;
					}
				}
				if (flag) {
					memcpy(&tmp, event, event_sz);

					/* raw data has extra bits for alignment, discard them */
					raw_sz_aligned = sample.raw_size - sizeof(u32);
					memcpy(tmp.sample.array, sample.raw_data, raw_sz_aligned);

					write_sz = sizeof(struct perf_event_header) +
							  raw_sz_aligned;

					/* without this we'll have out of order events */
					if (sample_type & PERF_SAMPLE_TIME)
						tmp.sample.array[n] = sample.time;

					tmp.header.size = write_sz;
					tmp.header.type = PERF_RECORD_SAMPLE;
					tmp.header.misc = PERF_RECORD_MISC_USER;

					src = &tmp;
				}
			}
		}
		if (offset + event_sz > size || written + write_sz > size)
			break;

		memcpy(dst, src, write_sz);

		dst += write_sz;
		written += write_sz;
		offset += event_sz;
	}

	return written;
}
