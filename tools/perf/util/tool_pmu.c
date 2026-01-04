// SPDX-License-Identifier: GPL-2.0-only
#include "cgroup.h"
#include "counts.h"
#include "cputopo.h"
#include "debug.h"
#include "evsel.h"
#include "pmu.h"
#include "print-events.h"
#include "smt.h"
#include "stat.h"
#include "time-utils.h"
#include "tool_pmu.h"
#include "tsc.h"
#include <api/fs/fs.h>
#include <api/io.h>
#include <internal/lib.h> // page_size
#include <internal/threadmap.h>
#include <perf/cpumap.h>
#include <perf/threadmap.h>
#include <fcntl.h>
#include <strings.h>
#include <api/io_dir.h>
#include <ctype.h>

static const char *const tool_pmu__event_names[TOOL_PMU__EVENT_MAX] = {
	NULL,
	"duration_time",
	"user_time",
	"system_time",
	"has_pmem",
	"num_cores",
	"num_cpus",
	"num_cpus_online",
	"num_dies",
	"num_packages",
	"slots",
	"smt_on",
	"system_tsc_freq",
	"core_wide",
	"target_cpu",
	"memory_anon_huge_pages",
	"memory_anonymous",
	"memory_data",
	"memory_file_pmd_mapped",
	"memory_ksm",
	"memory_lazyfree",
	"memory_locked",
	"memory_private_clean",
	"memory_private_dirty",
	"memory_private_hugetlb",
	"memory_pss",
	"memory_pss_anon",
	"memory_pss_dirty",
	"memory_pss_file",
	"memory_pss_shmem",
	"memory_referenced",
	"memory_resident",
	"memory_rss",
	"memory_shared",
	"memory_shared_clean",
	"memory_shared_dirty",
	"memory_shared_hugetlb",
	"memory_shmem_pmd_mapped",
	"memory_size",
	"memory_swap",
	"memory_swap_pss",
	"memory_text",
	"memory_uss",
	"net_rx_bytes",
	"net_rx_packets",
	"net_rx_errors",
	"net_rx_drop",
	"net_rx_fifo",
	"net_rx_frame",
	"net_rx_compressed",
	"net_rx_multicast",
	"net_tx_bytes",
	"net_tx_packets",
	"net_tx_errors",
	"net_tx_drop",
	"net_tx_fifo",
	"net_tx_colls",
	"net_tx_carrier",
	"net_tx_compressed",
};

bool tool_pmu__skip_event(const char *name __maybe_unused)
{
#if !defined(__aarch64__)
	/* The slots event should only appear on arm64. */
	if (strcasecmp(name, "slots") == 0)
		return true;
#endif
#if !defined(__i386__) && !defined(__x86_64__)
	/* The system_tsc_freq event should only appear on x86. */
	if (strcasecmp(name, "system_tsc_freq") == 0)
		return true;
#endif
	return false;
}

int tool_pmu__num_skip_events(void)
{
	int num = 0;

#if !defined(__aarch64__)
	num++;
#endif
#if !defined(__i386__) && !defined(__x86_64__)
	num++;
#endif
	return num;
}

const char *tool_pmu__event_to_str(enum tool_pmu_event ev)
{
	if ((ev > TOOL_PMU__EVENT_NONE && ev < TOOL_PMU__EVENT_MAX) &&
	    !tool_pmu__skip_event(tool_pmu__event_names[ev]))
		return tool_pmu__event_names[ev];

	return NULL;
}

enum tool_pmu_event tool_pmu__str_to_event(const char *str)
{
	int i;

	if (tool_pmu__skip_event(str))
		return TOOL_PMU__EVENT_NONE;

	tool_pmu__for_each_event(i) {
		if (!strcasecmp(str, tool_pmu__event_names[i]))
			return i;
	}
	return TOOL_PMU__EVENT_NONE;
}

bool perf_pmu__is_tool(const struct perf_pmu *pmu)
{
	return pmu && pmu->type == PERF_PMU_TYPE_TOOL;
}

bool evsel__is_tool(const struct evsel *evsel)
{
	return perf_pmu__is_tool(evsel->pmu);
}

enum tool_pmu_event evsel__tool_event(const struct evsel *evsel)
{
	if (!evsel__is_tool(evsel))
		return TOOL_PMU__EVENT_NONE;

	return (enum tool_pmu_event)evsel->core.attr.config;
}

const char *evsel__tool_pmu_event_name(const struct evsel *evsel)
{
	return tool_pmu__event_to_str(evsel->core.attr.config);
}

struct perf_cpu_map *tool_pmu__cpus(struct perf_event_attr *attr)
{
	static struct perf_cpu_map *cpu0_map;
	enum tool_pmu_event event = (enum tool_pmu_event)attr->config;

	if (event <= TOOL_PMU__EVENT_NONE || event >= TOOL_PMU__EVENT_MAX) {
		pr_err("Invalid tool PMU event config %llx\n", attr->config);
		return NULL;
	}
	if (event == TOOL_PMU__EVENT_USER_TIME || event == TOOL_PMU__EVENT_SYSTEM_TIME)
		return cpu_map__online();

	if (!cpu0_map)
		cpu0_map = perf_cpu_map__new_int(0);
	return perf_cpu_map__get(cpu0_map);
}

static bool read_until_char(struct io *io, char e)
{
	int c;

	do {
		c = io__get_char(io);
		if (c == -1)
			return false;
	} while (c != e);
	return true;
}

static int read_stat_field(int fd, struct perf_cpu cpu, int field, __u64 *val)
{
	char buf[256];
	struct io io;
	int i;

	io__init(&io, fd, buf, sizeof(buf));

	/* Skip lines to relevant CPU. */
	for (i = -1; i < cpu.cpu; i++) {
		if (!read_until_char(&io, '\n'))
			return -EINVAL;
	}
	/* Skip to "cpu". */
	if (io__get_char(&io) != 'c') return -EINVAL;
	if (io__get_char(&io) != 'p') return -EINVAL;
	if (io__get_char(&io) != 'u') return -EINVAL;

	/* Skip N of cpuN. */
	if (!read_until_char(&io, ' '))
		return -EINVAL;

	i = 1;
	while (true) {
		if (io__get_dec(&io, val) != ' ')
			break;
		if (field == i)
			return 0;
		i++;
	}
	return -EINVAL;
}

static int read_pid_stat_field(int fd, int field, __u64 *val)
{
	char buf[256];
	struct io io;
	int c, i;

	io__init(&io, fd, buf, sizeof(buf));
	if (io__get_dec(&io, val) != ' ')
		return -EINVAL;
	if (field == 1)
		return 0;

	/* Skip comm. */
	if (io__get_char(&io) != '(' || !read_until_char(&io, ')'))
		return -EINVAL;
	if (field == 2)
		return -EINVAL; /* String can't be returned. */

	/* Skip state */
	if (io__get_char(&io) != ' ' || io__get_char(&io) == -1)
		return -EINVAL;
	if (field == 3)
		return -EINVAL; /* String can't be returned. */

	/* Loop over numeric fields*/
	if (io__get_char(&io) != ' ')
		return -EINVAL;

	i = 4;
	while (true) {
		c = io__get_dec(&io, val);
		if (c == -1)
			return -EINVAL;
		if (c == -2) {
			/* Assume a -ve was read */
			c = io__get_dec(&io, val);
			*val *= -1;
		}
		if (c != ' ')
			return -EINVAL;
		if (field == i)
			return 0;
		i++;
	}
	return -EINVAL;
}

static bool tool_pmu__is_memory_event(enum tool_pmu_event ev)
{
	return ev >= TOOL_PMU__EVENT_MEMORY_ANON_HUGE_PAGES &&
	       ev <= TOOL_PMU__EVENT_MEMORY_USS;
}

static bool tool_pmu__is_memory_statm_event(enum tool_pmu_event ev)
{
	return ev == TOOL_PMU__EVENT_MEMORY_SIZE ||
	       ev == TOOL_PMU__EVENT_MEMORY_RESIDENT ||
	       ev == TOOL_PMU__EVENT_MEMORY_SHARED ||
	       ev == TOOL_PMU__EVENT_MEMORY_TEXT ||
	       ev == TOOL_PMU__EVENT_MEMORY_DATA;
}

static const char *tool_pmu__memory_event_to_key(enum tool_pmu_event ev)
{
	switch (ev) {
	case TOOL_PMU__EVENT_MEMORY_ANON_HUGE_PAGES: return "AnonHugePages:";
	case TOOL_PMU__EVENT_MEMORY_ANONYMOUS: return "Anonymous:";
	case TOOL_PMU__EVENT_MEMORY_FILE_PMD_MAPPED: return "FilePmdMapped:";
	case TOOL_PMU__EVENT_MEMORY_KSM: return "KSM:";
	case TOOL_PMU__EVENT_MEMORY_LAZYFREE: return "LazyFree:";
	case TOOL_PMU__EVENT_MEMORY_LOCKED: return "Locked:";
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_CLEAN: return "Private_Clean:";
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_DIRTY: return "Private_Dirty:";
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_HUGETLB: return "Private_Hugetlb:";
	case TOOL_PMU__EVENT_MEMORY_PSS: return "Pss:";
	case TOOL_PMU__EVENT_MEMORY_PSS_ANON: return "Pss_Anon:";
	case TOOL_PMU__EVENT_MEMORY_PSS_DIRTY: return "Pss_Dirty:";
	case TOOL_PMU__EVENT_MEMORY_PSS_FILE: return "Pss_File:";
	case TOOL_PMU__EVENT_MEMORY_PSS_SHMEM: return "Pss_Shmem:";
	case TOOL_PMU__EVENT_MEMORY_REFERENCED: return "Referenced:";
	case TOOL_PMU__EVENT_MEMORY_RSS: return "Rss:";
	case TOOL_PMU__EVENT_MEMORY_SHARED_CLEAN: return "Shared_Clean:";
	case TOOL_PMU__EVENT_MEMORY_SHARED_DIRTY: return "Shared_Dirty:";
	case TOOL_PMU__EVENT_MEMORY_SHARED_HUGETLB: return "Shared_Hugetlb:";
	case TOOL_PMU__EVENT_MEMORY_SHMEM_PMD_MAPPED: return "ShmemPmdMapped:";
	case TOOL_PMU__EVENT_MEMORY_SWAP: return "Swap:";
	case TOOL_PMU__EVENT_MEMORY_SWAP_PSS: return "SwapPss:";
	case TOOL_PMU__EVENT_MEMORY_DATA:
	case TOOL_PMU__EVENT_MEMORY_RESIDENT:
	case TOOL_PMU__EVENT_MEMORY_SHARED:
	case TOOL_PMU__EVENT_MEMORY_SIZE:
	case TOOL_PMU__EVENT_MEMORY_TEXT:
	case TOOL_PMU__EVENT_MEMORY_USS:
	case TOOL_PMU__EVENT_NET_RX_BYTES:
	case TOOL_PMU__EVENT_NET_RX_PACKETS:
	case TOOL_PMU__EVENT_NET_RX_ERRORS:
	case TOOL_PMU__EVENT_NET_RX_DROP:
	case TOOL_PMU__EVENT_NET_RX_FIFO:
	case TOOL_PMU__EVENT_NET_RX_FRAME:
	case TOOL_PMU__EVENT_NET_RX_COMPRESSED:
	case TOOL_PMU__EVENT_NET_RX_MULTICAST:
	case TOOL_PMU__EVENT_NET_TX_BYTES:
	case TOOL_PMU__EVENT_NET_TX_PACKETS:
	case TOOL_PMU__EVENT_NET_TX_ERRORS:
	case TOOL_PMU__EVENT_NET_TX_DROP:
	case TOOL_PMU__EVENT_NET_TX_FIFO:
	case TOOL_PMU__EVENT_NET_TX_COLLS:
	case TOOL_PMU__EVENT_NET_TX_CARRIER:
	case TOOL_PMU__EVENT_NET_TX_COMPRESSED:
	case TOOL_PMU__EVENT_DURATION_TIME:
	case TOOL_PMU__EVENT_USER_TIME:
	case TOOL_PMU__EVENT_SYSTEM_TIME:
	case TOOL_PMU__EVENT_HAS_PMEM:
	case TOOL_PMU__EVENT_NUM_CORES:
	case TOOL_PMU__EVENT_NUM_CPUS:
	case TOOL_PMU__EVENT_NUM_CPUS_ONLINE:
	case TOOL_PMU__EVENT_NUM_DIES:
	case TOOL_PMU__EVENT_NUM_PACKAGES:
	case TOOL_PMU__EVENT_SLOTS:
	case TOOL_PMU__EVENT_SMT_ON:
	case TOOL_PMU__EVENT_SYSTEM_TSC_FREQ:
	case TOOL_PMU__EVENT_CORE_WIDE:
	case TOOL_PMU__EVENT_TARGET_CPU:
	case TOOL_PMU__EVENT_NONE:
	case TOOL_PMU__EVENT_MAX:
	default: return NULL;
	}
}

static int read_smaps_rollup_field(int fd, const char *key, u64 *val)
{
	char buf[4096];
	struct io io;
	int ch;

	io__init(&io, fd, buf, sizeof(buf));

	while ((ch = io__get_char(&io)) != -1) {
		/* Check if line starts with key */
		if (ch == key[0]) {
			const char *k = key + 1;

			while (*k && (ch = io__get_char(&io)) == *k)
				k++;

			if (!*k) {
				/* Found key, skip whitespace */
				while ((ch = io__get_char(&io)) == ' ' || ch == '\t')
					;
				/* Read value */
				if (ch >= '0' && ch <= '9') {
					*val = ch - '0';
					while ((ch = io__get_char(&io)) >= '0' && ch <= '9') {
						*val = *val * 10 + (ch - '0');
					}
					/* Convert kB to bytes */
					*val *= 1024;
					return 0;
				}
			}
		}
		/* Skip rest of line */
		if (ch != '\n')
			read_until_char(&io, '\n');
	}
	return -EINVAL;
}

static int read_smaps_rollup(int fd, enum tool_pmu_event ev, u64 *val)
{
	int ret;

	if (ev == TOOL_PMU__EVENT_MEMORY_USS) {
		u64 pc, pd;

		lseek(fd, 0, SEEK_SET);
		ret = read_smaps_rollup_field(fd, "Private_Clean:", &pc);
		if (ret)
			return ret;
		lseek(fd, 0, SEEK_SET);
		ret = read_smaps_rollup_field(fd, "Private_Dirty:", &pd);
		if (ret)
			return ret;
		*val = pc + pd;
		return 0;
	}

	lseek(fd, 0, SEEK_SET);
	return read_smaps_rollup_field(fd, tool_pmu__memory_event_to_key(ev), val);
}

static int read_statm(int fd, enum tool_pmu_event ev, u64 *val)
{
	char buf[128];
	struct io io;
	u64 v;

	io__init(&io, fd, buf, sizeof(buf));
	lseek(fd, 0, SEEK_SET);

	/* Size */
	if (io__get_dec(&io, (__u64 *)&v) == -1)
		return -EINVAL;
	if (ev == TOOL_PMU__EVENT_MEMORY_SIZE) {
		*val = v * page_size;
		return 0;
	}

	/* Resident */
	if (io__get_dec(&io, (__u64 *)&v) == -1) /* Skip */
		return -EINVAL;
	if (ev == TOOL_PMU__EVENT_MEMORY_RESIDENT) {
		*val = v * page_size;
		return 0;
	}

	/* Shared */
	if (io__get_dec(&io, (__u64 *)&v) == -1) /* Skip */
		return -EINVAL;
	if (ev == TOOL_PMU__EVENT_MEMORY_SHARED) {
		*val = v * page_size;
		return 0;
	}

	/* Text */
	if (io__get_dec(&io, (__u64 *)&v) == -1)
		return -EINVAL;
	if (ev == TOOL_PMU__EVENT_MEMORY_TEXT) {
		*val = v * page_size;
		return 0;
	}

	/* Lib */
	if (io__get_dec(&io, (__u64 *)&v) == -1) /* Skip */
		return -EINVAL;

	/* Data */
	if (io__get_dec(&io, (__u64 *)&v) == -1)
		return -EINVAL;
	if (ev == TOOL_PMU__EVENT_MEMORY_DATA) {
		*val = v * page_size;
		return 0;
	}

	return -EINVAL;
}

static bool tool_pmu__is_net_event(enum tool_pmu_event ev)
{
	return ev >= TOOL_PMU__EVENT_NET_RX_BYTES &&
	       ev <= TOOL_PMU__EVENT_NET_TX_COMPRESSED;
}

static int read_net_dev(int fd, enum tool_pmu_event ev, u64 *val)
{
	struct io io;
	char buf[4096];
	int i;
	int index = ev - TOOL_PMU__EVENT_NET_RX_BYTES;

	io__init(&io, fd, buf, sizeof(buf));
	lseek(fd, 0, SEEK_SET);

	/*
	 * Drop first two lines of:
	 * Inter-|   Receive                                                |  Transmit
	 *  face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
	 */
	if (!read_until_char(&io, '\n'))
		return -EINVAL;
	if (!read_until_char(&io, '\n'))
		return -EINVAL;

	*val = 0;
	while (true) {
		int ch = io__get_char(&io);
		__u64 read_val;

		/* First read interface name, such as "    lo:" */
		if (ch == -1)
			break;
		while (ch == ' ')
			ch = io__get_char(&io);
		if (ch == -1)
			break;
		while (ch != ':' && ch != -1 && ch != '\n')
			ch = io__get_char(&io);
		if (ch != ':') {
			if (ch == '\n')
				continue;
			if (ch == -1)
				return 0; /* Assume EOF. */
			read_until_char(&io, '\n');
			continue;
		}
		/* Ignore columns before one being read. */
		for (i = 0; i < index; i++) {
			if (io__get_dec(&io, &read_val) == -1)
				return 0; /* Assume EOF. */
		}
		/* Read actually value. */
		if (io__get_dec(&io, &read_val) != -1)
			*val += read_val;
		/* Move to the next line. */
		read_until_char(&io, '\n');
	}
	return 0;
}

int evsel__tool_pmu_prepare_open(struct evsel *evsel,
				 struct perf_cpu_map *cpus,
				 int nthreads)
{
	if ((evsel__tool_event(evsel) == TOOL_PMU__EVENT_SYSTEM_TIME ||
	     evsel__tool_event(evsel) == TOOL_PMU__EVENT_USER_TIME) &&
	    !evsel->start_times) {
		evsel->start_times = xyarray__new(perf_cpu_map__nr(cpus),
						  nthreads,
						  sizeof(__u64));
		if (!evsel->start_times)
			return -ENOMEM;
	}
	return 0;
}

#define FD(e, x, y) (*(int *)xyarray__entry(e->core.fd, x, y))

int evsel__tool_pmu_open(struct evsel *evsel,
			 struct perf_thread_map *threads,
			 int start_cpu_map_idx, int end_cpu_map_idx)
{
	enum tool_pmu_event ev = evsel__tool_event(evsel);
	int pid = -1, idx = 0, thread = 0, nthreads, err = 0, old_errno;

	if (ev == TOOL_PMU__EVENT_NUM_CPUS)
		return 0;

	if (ev == TOOL_PMU__EVENT_DURATION_TIME) {
		if (evsel->core.attr.sample_period) /* no sampling */
			return -EINVAL;
		evsel->start_time = rdclock();
		return 0;
	}

	if (evsel->cgrp)
		pid = evsel->cgrp->fd;

	nthreads = perf_thread_map__nr(threads);
	for (idx = start_cpu_map_idx; idx < end_cpu_map_idx; idx++) {
		for (thread = 0; thread < nthreads; thread++) {
			if (!evsel->cgrp && !evsel->core.system_wide)
				pid = perf_thread_map__pid(threads, thread);

			if (ev == TOOL_PMU__EVENT_USER_TIME || ev == TOOL_PMU__EVENT_SYSTEM_TIME) {
				bool system = ev == TOOL_PMU__EVENT_SYSTEM_TIME;
				__u64 *start_time = NULL;
				char buf[PATH_MAX];
				int fd;

				if (evsel->core.attr.sample_period) {
					/* no sampling */
					err = -EINVAL;
					goto out_close;
				}
				if (pid > -1) {
					snprintf(buf, sizeof(buf), "%s/%d/stat",
						 procfs__mountpoint(), pid);
					evsel->pid_stat = true;
				} else {
					snprintf(buf, sizeof(buf), "%s/stat",
						 procfs__mountpoint());
				}
				fd = open(buf, O_RDONLY);
				FD(evsel, idx, thread) = fd;
				if (fd < 0) {
					err = -errno;
					goto out_close;
				}
				start_time = xyarray__entry(evsel->start_times, idx, thread);
				if (pid > -1) {
					err = read_pid_stat_field(fd, system ? 15 : 14,
								  start_time);
				} else {
					struct perf_cpu cpu;

					cpu = perf_cpu_map__cpu(evsel->core.cpus, idx);
					err = read_stat_field(fd, cpu, system ? 3 : 1,
							      start_time);
				}
				if (err)
					goto out_close;
			} else if (tool_pmu__is_memory_event(ev) ||
				   tool_pmu__is_net_event(ev)) {
				char buf[PATH_MAX];
				int fd = -1;

				if (pid > -1) {
					if (tool_pmu__is_memory_statm_event(ev)) {
						snprintf(buf, sizeof(buf), "%s/%d/statm",
							 procfs__mountpoint(), pid);
					} else if (tool_pmu__is_net_event(ev)) {
						snprintf(buf, sizeof(buf), "%s/%d/net/dev",
							 procfs__mountpoint(), pid);
					} else {
						snprintf(buf, sizeof(buf), "%s/%d/smaps_rollup",
							 procfs__mountpoint(), pid);
					}
					fd = open(buf, O_RDONLY);
				}
				if (pid == -1 && tool_pmu__is_net_event(ev)) {
					/* Read /proc/net/dev that already aggregates the counts. */
					snprintf(buf, sizeof(buf), "%s/net/dev",
						 procfs__mountpoint());
					fd = open(buf, O_RDONLY);
				}
				/*
				 * For memory event system-wide (pid == -1), we
				 * don't open a file here.  We will aggregate in
				 * read().
				 */
				if ((pid > -1 || tool_pmu__is_net_event(ev)) && fd < 0) {
					err = -errno;
					goto out_close;
				}
				FD(evsel, idx, thread) = fd;
			}

		}
	}
	return 0;
out_close:
	if (err)
		threads->err_thread = thread;

	old_errno = errno;
	do {
		while (--thread >= 0) {
			if (FD(evsel, idx, thread) >= 0)
				close(FD(evsel, idx, thread));
			FD(evsel, idx, thread) = -1;
		}
		thread = nthreads;
	} while (--idx >= 0);
	errno = old_errno;
	return err;
}

#if !defined(__i386__) && !defined(__x86_64__)
u64 arch_get_tsc_freq(void)
{
	return 0;
}
#endif

#if !defined(__aarch64__)
u64 tool_pmu__cpu_slots_per_cycle(void)
{
	return 0;
}
#endif

static bool has_pmem(void)
{
	static bool has_pmem, cached;
	const char *sysfs = sysfs__mountpoint();
	char path[PATH_MAX];

	if (!cached) {
		snprintf(path, sizeof(path), "%s/firmware/acpi/tables/NFIT", sysfs);
		has_pmem = access(path, F_OK) == 0;
		cached = true;
	}
	return has_pmem;
}

bool tool_pmu__read_event(enum tool_pmu_event ev,
			  struct evsel *evsel,
			  bool system_wide,
			  const char *user_requested_cpu_list,
			  u64 *result)
{
	const struct cpu_topology *topology;

	switch (ev) {
	case TOOL_PMU__EVENT_HAS_PMEM:
		*result = has_pmem() ? 1 : 0;
		return true;

	case TOOL_PMU__EVENT_NUM_CORES:
		topology = online_topology();
		*result = topology->core_cpus_lists;
		return true;

	case TOOL_PMU__EVENT_NUM_CPUS:
		if (!evsel || perf_cpu_map__is_empty(evsel->core.cpus)) {
			/* No evsel to be specific to. */
			*result = cpu__max_present_cpu().cpu;
		} else if (!perf_cpu_map__has_any_cpu(evsel->core.cpus)) {
			/* Evsel just has specific CPUs. */
			*result = perf_cpu_map__nr(evsel->core.cpus);
		} else {
			/*
			 * "Any CPU" event that can be scheduled on any CPU in
			 * the PMU's cpumask. The PMU cpumask should be saved in
			 * pmu_cpus. If not present fall back to max.
			 */
			if (!perf_cpu_map__is_empty(evsel->core.pmu_cpus))
				*result = perf_cpu_map__nr(evsel->core.pmu_cpus);
			else
				*result = cpu__max_present_cpu().cpu;
		}
		return true;

	case TOOL_PMU__EVENT_NUM_CPUS_ONLINE: {
		struct perf_cpu_map *online = cpu_map__online();

		if (!online)
			return false;

		if (!evsel || perf_cpu_map__is_empty(evsel->core.cpus)) {
			/* No evsel to be specific to. */
			*result = perf_cpu_map__nr(online);
		} else if (!perf_cpu_map__has_any_cpu(evsel->core.cpus)) {
			/* Evsel just has specific CPUs. */
			struct perf_cpu_map *tmp =
				perf_cpu_map__intersect(online, evsel->core.cpus);

			*result = perf_cpu_map__nr(tmp);
			perf_cpu_map__put(tmp);
		} else {
			/*
			 * "Any CPU" event that can be scheduled on any CPU in
			 * the PMU's cpumask. The PMU cpumask should be saved in
			 * pmu_cpus, if not present then just the online cpu
			 * mask.
			 */
			if (!perf_cpu_map__is_empty(evsel->core.pmu_cpus)) {
				struct perf_cpu_map *tmp =
					perf_cpu_map__intersect(online, evsel->core.pmu_cpus);

				*result = perf_cpu_map__nr(tmp);
				perf_cpu_map__put(tmp);
			} else {
				*result = perf_cpu_map__nr(online);
			}
		}
		perf_cpu_map__put(online);
		return true;
	}
	case TOOL_PMU__EVENT_NUM_DIES:
		topology = online_topology();
		*result = topology->die_cpus_lists;
		return true;

	case TOOL_PMU__EVENT_NUM_PACKAGES:
		topology = online_topology();
		*result = topology->package_cpus_lists;
		return true;

	case TOOL_PMU__EVENT_SLOTS:
		*result = tool_pmu__cpu_slots_per_cycle();
		return *result ? true : false;

	case TOOL_PMU__EVENT_SMT_ON:
		*result = smt_on() ? 1 : 0;
		return true;

	case TOOL_PMU__EVENT_SYSTEM_TSC_FREQ:
		*result = arch_get_tsc_freq();
		return true;

	case TOOL_PMU__EVENT_CORE_WIDE:
		*result = core_wide(system_wide, user_requested_cpu_list) ? 1 : 0;
		return true;

	case TOOL_PMU__EVENT_TARGET_CPU:
		*result = system_wide || (user_requested_cpu_list != NULL) ? 1 : 0;
		return true;

	case TOOL_PMU__EVENT_MEMORY_SIZE:
	case TOOL_PMU__EVENT_MEMORY_RSS:
	case TOOL_PMU__EVENT_MEMORY_PSS:
	case TOOL_PMU__EVENT_MEMORY_SHARED:
	case TOOL_PMU__EVENT_MEMORY_SHARED_CLEAN:
	case TOOL_PMU__EVENT_MEMORY_SHARED_DIRTY:
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_CLEAN:
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_DIRTY:
	case TOOL_PMU__EVENT_MEMORY_USS:
	case TOOL_PMU__EVENT_MEMORY_SWAP:
	case TOOL_PMU__EVENT_MEMORY_SWAP_PSS:
	case TOOL_PMU__EVENT_MEMORY_PSS_DIRTY:
	case TOOL_PMU__EVENT_MEMORY_PSS_ANON:
	case TOOL_PMU__EVENT_MEMORY_PSS_FILE:
	case TOOL_PMU__EVENT_MEMORY_PSS_SHMEM:
	case TOOL_PMU__EVENT_MEMORY_RESIDENT:
	case TOOL_PMU__EVENT_MEMORY_REFERENCED:
	case TOOL_PMU__EVENT_MEMORY_ANONYMOUS:
	case TOOL_PMU__EVENT_MEMORY_KSM:
	case TOOL_PMU__EVENT_MEMORY_LAZYFREE:
	case TOOL_PMU__EVENT_MEMORY_ANON_HUGE_PAGES:
	case TOOL_PMU__EVENT_MEMORY_SHMEM_PMD_MAPPED:
	case TOOL_PMU__EVENT_MEMORY_FILE_PMD_MAPPED:
	case TOOL_PMU__EVENT_MEMORY_SHARED_HUGETLB:
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_HUGETLB:
	case TOOL_PMU__EVENT_MEMORY_LOCKED:
	case TOOL_PMU__EVENT_MEMORY_DATA:
	case TOOL_PMU__EVENT_MEMORY_TEXT:
	case TOOL_PMU__EVENT_NET_RX_BYTES:
	case TOOL_PMU__EVENT_NET_RX_PACKETS:
	case TOOL_PMU__EVENT_NET_RX_ERRORS:
	case TOOL_PMU__EVENT_NET_RX_DROP:
	case TOOL_PMU__EVENT_NET_RX_FIFO:
	case TOOL_PMU__EVENT_NET_RX_FRAME:
	case TOOL_PMU__EVENT_NET_RX_COMPRESSED:
	case TOOL_PMU__EVENT_NET_RX_MULTICAST:
	case TOOL_PMU__EVENT_NET_TX_BYTES:
	case TOOL_PMU__EVENT_NET_TX_PACKETS:
	case TOOL_PMU__EVENT_NET_TX_ERRORS:
	case TOOL_PMU__EVENT_NET_TX_DROP:
	case TOOL_PMU__EVENT_NET_TX_FIFO:
	case TOOL_PMU__EVENT_NET_TX_COLLS:
	case TOOL_PMU__EVENT_NET_TX_CARRIER:
	case TOOL_PMU__EVENT_NET_TX_COMPRESSED:
	case TOOL_PMU__EVENT_NONE:
	case TOOL_PMU__EVENT_DURATION_TIME:
	case TOOL_PMU__EVENT_USER_TIME:
	case TOOL_PMU__EVENT_SYSTEM_TIME:
	case TOOL_PMU__EVENT_MAX:
	default:
		return false;
	}
}

static void perf_counts__update(struct perf_counts_values *count,
				const struct perf_counts_values *old_count,
				bool raw, u64 val)
{
	/*
	 * The values of enabled and running must make a ratio of 100%. The
	 * exact values don't matter as long as they are non-zero to avoid
	 * issues with evsel__count_has_error.
	 */
	if (old_count) {
		count->val = raw ? val : old_count->val + val;
		count->run = old_count->run + 1;
		count->ena = old_count->ena + 1;
		count->lost = old_count->lost;
	} else {
		count->val = val;
		count->run++;
		count->ena++;
		count->lost = 0;
	}
}

static int tool_pmu__aggregate_memory_event(enum tool_pmu_event ev, u64 *val)
{
	struct io_dir iod;
	struct io_dirent64 *ent;
	int proc_fd;

	*val = 0;
	proc_fd = open(procfs__mountpoint(), O_DIRECTORY | O_RDONLY);
	if (proc_fd < 0)
		return -errno;

	io_dir__init(&iod, proc_fd);

	while ((ent = io_dir__readdir(&iod)) != NULL) {
		char buf[PATH_MAX];
		u64 pid_val;
		int fd;

		if (!io_dir__is_dir(&iod, ent))
			continue;

		if (!isdigit(ent->d_name[0]))
			continue;

		if (tool_pmu__is_memory_statm_event(ev))
			snprintf(buf, sizeof(buf), "%s/statm", ent->d_name);
		else
			snprintf(buf, sizeof(buf), "%s/smaps_rollup", ent->d_name);

		fd = openat(proc_fd, buf, O_RDONLY);
		if (fd < 0)
			continue;

		if (tool_pmu__is_memory_statm_event(ev)) {
			if (!read_statm(fd, ev, &pid_val))
				*val += pid_val;
		} else {
			if (!read_smaps_rollup(fd, ev, &pid_val))
				*val += pid_val;
		}
		close(fd);
	}
	close(proc_fd);
	return 0;
}

int evsel__tool_pmu_read(struct evsel *evsel, int cpu_map_idx, int thread)
{
	__u64 *start_time, cur_time, delta_start;
	int err = 0;
	struct perf_counts_values *count, *old_count = NULL;
	bool adjust = false;
	enum tool_pmu_event ev = evsel__tool_event(evsel);

	count = perf_counts(evsel->counts, cpu_map_idx, thread);
	if (evsel->prev_raw_counts)
		old_count = perf_counts(evsel->prev_raw_counts, cpu_map_idx, thread);

	switch (ev) {
	case TOOL_PMU__EVENT_HAS_PMEM:
	case TOOL_PMU__EVENT_NUM_CORES:
	case TOOL_PMU__EVENT_NUM_CPUS:
	case TOOL_PMU__EVENT_NUM_CPUS_ONLINE:
	case TOOL_PMU__EVENT_NUM_DIES:
	case TOOL_PMU__EVENT_NUM_PACKAGES:
	case TOOL_PMU__EVENT_SLOTS:
	case TOOL_PMU__EVENT_SMT_ON:
	case TOOL_PMU__EVENT_CORE_WIDE:
	case TOOL_PMU__EVENT_TARGET_CPU:
	case TOOL_PMU__EVENT_SYSTEM_TSC_FREQ: {
		u64 val = 0;

		if (cpu_map_idx == 0 && thread == 0) {
			if (!tool_pmu__read_event(ev, evsel,
						  stat_config.system_wide,
						  stat_config.user_requested_cpu_list,
						  &val)) {
				count->lost++;
				val = 0;
			}
		}
		perf_counts__update(count, old_count, /*raw=*/false, val);
		return 0;
	}
	case TOOL_PMU__EVENT_DURATION_TIME:
		/*
		 * Pretend duration_time is only on the first CPU and thread, or
		 * else aggregation will scale duration_time by the number of
		 * CPUs/threads.
		 */
		start_time = &evsel->start_time;
		if (cpu_map_idx == 0 && thread == 0)
			cur_time = rdclock();
		else
			cur_time = *start_time;
		break;
	case TOOL_PMU__EVENT_USER_TIME:
	case TOOL_PMU__EVENT_SYSTEM_TIME: {
		bool system = evsel__tool_event(evsel) == TOOL_PMU__EVENT_SYSTEM_TIME;
		int fd = FD(evsel, cpu_map_idx, thread);

		start_time = xyarray__entry(evsel->start_times, cpu_map_idx, thread);
		lseek(fd, SEEK_SET, 0);
		if (evsel->pid_stat) {
			/* The event exists solely on 1 CPU. */
			if (cpu_map_idx == 0)
				err = read_pid_stat_field(fd, system ? 15 : 14, &cur_time);
			else
				cur_time = 0;
		} else {
			/* The event is for all threads. */
			if (thread == 0) {
				struct perf_cpu cpu = perf_cpu_map__cpu(evsel->core.cpus,
									cpu_map_idx);

				err = read_stat_field(fd, cpu, system ? 3 : 1, &cur_time);
			} else {
				cur_time = 0;
			}
		}
		adjust = true;
		break;
	}
	case TOOL_PMU__EVENT_MEMORY_SIZE:
	case TOOL_PMU__EVENT_MEMORY_RSS:
	case TOOL_PMU__EVENT_MEMORY_PSS:
	case TOOL_PMU__EVENT_MEMORY_SHARED:
	case TOOL_PMU__EVENT_MEMORY_SHARED_CLEAN:
	case TOOL_PMU__EVENT_MEMORY_SHARED_DIRTY:
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_CLEAN:
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_DIRTY:
	case TOOL_PMU__EVENT_MEMORY_USS:
	case TOOL_PMU__EVENT_MEMORY_SWAP:
	case TOOL_PMU__EVENT_MEMORY_SWAP_PSS:
	case TOOL_PMU__EVENT_MEMORY_PSS_DIRTY:
	case TOOL_PMU__EVENT_MEMORY_PSS_ANON:
	case TOOL_PMU__EVENT_MEMORY_PSS_FILE:
	case TOOL_PMU__EVENT_MEMORY_PSS_SHMEM:
	case TOOL_PMU__EVENT_MEMORY_REFERENCED:
	case TOOL_PMU__EVENT_MEMORY_RESIDENT:
	case TOOL_PMU__EVENT_MEMORY_ANONYMOUS:
	case TOOL_PMU__EVENT_MEMORY_KSM:
	case TOOL_PMU__EVENT_MEMORY_LAZYFREE:
	case TOOL_PMU__EVENT_MEMORY_ANON_HUGE_PAGES:
	case TOOL_PMU__EVENT_MEMORY_SHMEM_PMD_MAPPED:
	case TOOL_PMU__EVENT_MEMORY_FILE_PMD_MAPPED:
	case TOOL_PMU__EVENT_MEMORY_SHARED_HUGETLB:
	case TOOL_PMU__EVENT_MEMORY_PRIVATE_HUGETLB:
	case TOOL_PMU__EVENT_MEMORY_LOCKED:
	case TOOL_PMU__EVENT_MEMORY_DATA:
	case TOOL_PMU__EVENT_MEMORY_TEXT:
	case TOOL_PMU__EVENT_NET_RX_BYTES:
	case TOOL_PMU__EVENT_NET_RX_PACKETS:
	case TOOL_PMU__EVENT_NET_RX_ERRORS:
	case TOOL_PMU__EVENT_NET_RX_DROP:
	case TOOL_PMU__EVENT_NET_RX_FIFO:
	case TOOL_PMU__EVENT_NET_RX_FRAME:
	case TOOL_PMU__EVENT_NET_RX_COMPRESSED:
	case TOOL_PMU__EVENT_NET_RX_MULTICAST:
	case TOOL_PMU__EVENT_NET_TX_BYTES:
	case TOOL_PMU__EVENT_NET_TX_PACKETS:
	case TOOL_PMU__EVENT_NET_TX_ERRORS:
	case TOOL_PMU__EVENT_NET_TX_DROP:
	case TOOL_PMU__EVENT_NET_TX_FIFO:
	case TOOL_PMU__EVENT_NET_TX_COLLS:
	case TOOL_PMU__EVENT_NET_TX_CARRIER:
	case TOOL_PMU__EVENT_NET_TX_COMPRESSED: {
		int fd = FD(evsel, cpu_map_idx, thread);
		u64 val = 0;

		if (fd >= 0) {
			/* Per-process or system-wide net. */
			int ret;

			if (tool_pmu__is_memory_statm_event(ev))
				ret = read_statm(fd, ev, &val);
			else if (tool_pmu__is_net_event(ev))
				ret = read_net_dev(fd, ev, &val);
			else
				ret = read_smaps_rollup(fd, ev, &val);

			if (ret)
				return ret;
		} else {
			/* System-wide aggregation */
			if (cpu_map_idx == 0 && thread == 0) {
				assert(tool_pmu__is_memory_event(ev));
				tool_pmu__aggregate_memory_event(ev, &val);
			}
		}
		perf_counts__update(count, old_count, /*raw=*/false, val);
		return 0;
	}
	case TOOL_PMU__EVENT_NONE:
	case TOOL_PMU__EVENT_MAX:
	default:
		err = -EINVAL;
	}
	if (err)
		return err;

	delta_start = cur_time - *start_time;
	if (adjust) {
		__u64 ticks_per_sec = sysconf(_SC_CLK_TCK);

		delta_start *= 1e9 / ticks_per_sec;
	}
	perf_counts__update(count, old_count, /*raw=*/true, delta_start);
	return 0;
}

struct perf_pmu *tool_pmu__new(void)
{
	struct perf_pmu *tool = zalloc(sizeof(struct perf_pmu));

	if (!tool)
		return NULL;

	if (perf_pmu__init(tool, PERF_PMU_TYPE_TOOL, "tool") != 0) {
		perf_pmu__delete(tool);
		return NULL;
	}
	tool->events_table = find_core_events_table("common", "common");
	return tool;
}
