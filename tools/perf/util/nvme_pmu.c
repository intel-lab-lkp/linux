// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "counts.h"
#include "debug.h"
#include "evsel.h"
#include "hashmap.h"
#include "nvme_pmu.h"
#include "pmu.h"
#include <internal/xyarray.h>
#include <internal/threadmap.h>
#include <perf/threadmap.h>
#include <sys/types.h>
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <api/fs/fs.h>
#include <api/io.h>
#include <api/io_dir.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/zalloc.h>

#ifdef HAVE_LIBNVME_SUPPORT
#include <libnvme.h>

struct nvme_event {
	const char *name;
	const char *desc;
	const char *scale_unit;
	uint64_t config;
};

static const struct nvme_event nvme_events[] = {
	{ "smart_data_units_read",
	  "Data units read (in 1000s of 512-byte units)",
	  "512000B", NVME_SMART(16, data_units_read) },
	{ "smart_data_units_written",
	  "Data units written (in 1000s of 512-byte units)",
	  "512000B", NVME_SMART(16, data_units_written) },
	{ "smart_host_read_commands", "Host read commands", NULL, NVME_SMART(16, host_reads) },
	{ "smart_host_write_commands", "Host write commands", NULL, NVME_SMART(16, host_writes) },
	{ "smart_ctrl_busy_time", "Controller busy time", "60s", NVME_SMART(16, ctrl_busy_time) },
	{ "smart_power_cycles", "Power cycles", NULL, NVME_SMART(16, power_cycles) },
	{ "smart_power_on_hours", "Power on hours", "1h", NVME_SMART(16, power_on_hours) },
	{ "smart_unsafe_shutdowns", "Unsafe shutdowns", NULL, NVME_SMART(16, unsafe_shutdowns) },
	{ "smart_media_errors", "Media errors", NULL, NVME_SMART(16, media_errors) },
	{ "smart_num_err_log_entries",
	  "Number of error log entries",
	  NULL, NVME_SMART(16, num_err_log_entries) },
	{ "smart_warning_temp_time",
	  "Warning temperature time",
	  "60s", NVME_SMART(4, warning_temp_time) },
	{ "smart_crit_comp_time",
	  "Critical composite temperature time",
	  "60s", NVME_SMART(4, critical_comp_time) },
	{ "smart_temperature", "Temperature", "0.001'C", NVME_SMART(2, temperature) },

	{ "endurance_percent_used",
	  "Endurance group percentage used",
	  NULL, NVME_ENDURANCE(1, percent_used) },
	{ "endurance_data_units_read",
	  "Endurance group data units read",
	  "512000B", NVME_ENDURANCE(16, data_units_read) },
	{ "endurance_data_units_written",
	  "Endurance group data units written",
	  "512000B", NVME_ENDURANCE(16, data_units_written) },
	{ "endurance_media_units_written",
	  "Endurance group media units written",
	  "512000B", NVME_ENDURANCE(16, media_units_written) },
	{ "endurance_host_read_cmds",
	  "Endurance group host read commands",
	  NULL, NVME_ENDURANCE(16, host_read_cmds) },
	{ "endurance_host_write_cmds",
	  "Endurance group host write commands",
	  NULL, NVME_ENDURANCE(16, host_write_cmds) },
	{ "endurance_num_err_info_log_entries",
	  "Endurance group number of error information log entries",
	  NULL, NVME_ENDURANCE(16, num_err_info_log_entries) },

	{ "fdp_hbmw", "FDP host bytes with metadata written", "1B", NVME_FDP(16, hbmw) },
	{ "fdp_mbmw", "FDP media bytes with metadata written", "1B", NVME_FDP(16, mbmw) },
	{ "fdp_mbe", "FDP media bytes erased", "1B", NVME_FDP(16, mbe) },

	{ "error_count", "Error info log error count", NULL, NVME_ERROR(8, error_count) },

	{ "zns_nrzid", "ZNS changed zone nrzid", NULL, NVME_ZNS(2, nrzid) },
};


struct nvme_pmu {
	struct perf_pmu pmu;
	char *dev_name;
	bool support_checked;
	bool log_supported[256];
};


bool perf_pmu__is_nvme(const struct perf_pmu *pmu)
{
	return pmu && pmu->type >= PERF_PMU_TYPE_NVME_START &&
		pmu->type <= PERF_PMU_TYPE_NVME_END;
}

bool evsel__is_nvme(const struct evsel *evsel)
{
	return perf_pmu__is_nvme(evsel->pmu);
}

struct perf_pmu *nvme_pmu__new(struct list_head *pmus, const char *sysfs_name, const char *name)
{
	struct nvme_pmu *nvm;
	char buf[64];
	__u32 type;

	/*
	 * Usually sysfs_name is something like "nvme0".
	 * We try to extract the number. If parsing fails, we use 0.
	 */
	type = PERF_PMU_TYPE_NVME_START + strtoul(sysfs_name + 4, NULL, 10);

	if (type > PERF_PMU_TYPE_NVME_END) {
		pr_err("Unable to encode NVMe type from %s in valid PMU type\n", sysfs_name);
		return NULL;
	}

	snprintf(buf, sizeof(buf), "nvme_%s", name);

	nvm = zalloc(sizeof(*nvm));
	if (!nvm)
		return NULL;

	if (perf_pmu__init(&nvm->pmu, type, buf) != 0) {
		free(nvm);
		return NULL;
	}

	nvm->dev_name = strdup(sysfs_name);
	if (!nvm->dev_name) {
		perf_pmu__delete(&nvm->pmu);
		return NULL;
	}
	nvm->pmu.alias_name = strdup(sysfs_name);
	if (!nvm->pmu.alias_name) {
		perf_pmu__delete(&nvm->pmu);
		return NULL;
	}
	nvm->pmu.cpus = perf_cpu_map__new_int(0);
	if (!nvm->pmu.cpus) {
		perf_pmu__delete(&nvm->pmu);
		return NULL;
	}
	INIT_LIST_HEAD(&nvm->pmu.format);
	INIT_LIST_HEAD(&nvm->pmu.caps);

	list_add_tail(&nvm->pmu.list, pmus);
	return &nvm->pmu;
}

void nvme_pmu__exit(struct perf_pmu *pmu)
{
	struct nvme_pmu *nvm = container_of(pmu, struct nvme_pmu, pmu);

	zfree(&nvm->dev_name);
}



static void nvme_pmu__check_support(struct nvme_pmu *nvm)
{
	int fd;
	char path[PATH_MAX];
	struct nvme_smart_log smart_log;
	struct nvme_endurance_group_log endurance_log;
	struct nvme_fdp_stats_log fdp_log;
	struct nvme_error_log_page error_log;
	struct nvme_zns_changed_zone_log zns_log;

	if (nvm->support_checked)
		return;

	nvm->support_checked = true;

	/* Assume all supported if we can't test. */
	memset(nvm->log_supported, 1, sizeof(nvm->log_supported));

	snprintf(path, sizeof(path), "/dev/%s", nvm->dev_name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;

	if (nvme_get_log_smart(fd, NVME_NSID_ALL, true, &smart_log) != 0)
		nvm->log_supported[NVME_LOG_SMART] = false;

	if (nvme_get_log_endurance_group(fd, 0, &endurance_log) != 0)
		nvm->log_supported[NVME_LOG_ENDURANCE] = false;

	if (nvme_get_log_fdp_stats(fd, 0, 0, sizeof(fdp_log), &fdp_log) != 0)
		nvm->log_supported[NVME_LOG_FDP] = false;

	if (nvme_get_log_error(fd, 1, true, &error_log) != 0)
		nvm->log_supported[NVME_LOG_ERROR] = false;

	if (nvme_get_log_zns_changed_zones(fd, NVME_NSID_ALL, true, &zns_log) != 0)
		nvm->log_supported[NVME_LOG_ZNS] = false;

	close(fd);
}

int nvme_pmu__for_each_event(struct perf_pmu *pmu, void *state, pmu_event_callback cb)
{
	struct nvme_pmu *nvm = container_of(pmu, struct nvme_pmu, pmu);
	size_t i;

	nvme_pmu__check_support(nvm);
	for (i = 0; i < ARRAY_SIZE(nvme_events); i++) {
		const struct nvme_event *e = &nvme_events[i];
		char alias_buf[64];
		char desc_buf[256];
		char encoding_buf[128];
		struct pmu_event_info info = {
			.pmu = pmu,
			.name = e->name,
			.alias = alias_buf,
			.scale_unit = e->scale_unit,
			.desc = desc_buf,
			.long_desc = NULL,
			.encoding_desc = encoding_buf,

			.topic = "nvme",
			.pmu_name = pmu->name,
			.event_type_desc = "NVMe event",
			.deprecated = !nvm->log_supported[(e->config >> 24) & 0xFF],
		};

		int ret;

		snprintf(alias_buf, sizeof(alias_buf), "%s", e->name);
		snprintf(desc_buf, sizeof(desc_buf), "%s", e->desc);
		snprintf(encoding_buf, sizeof(encoding_buf),
			 "%s/config=0x%lx/", pmu->name, e->config);

		ret = cb(state, &info);
		if (ret)
			return ret;
	}
	return 0;
}

size_t nvme_pmu__num_events(struct perf_pmu *pmu __maybe_unused)
{
	return ARRAY_SIZE(nvme_events);
}

bool nvme_pmu__have_event(struct perf_pmu *pmu __maybe_unused, const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(nvme_events); i++) {
		if (!strcasecmp(name, nvme_events[i].name))
			return true;
	}
	return false;
}

static int nvme_pmu__config_term(const struct nvme_pmu *nvm __maybe_unused,
				 struct perf_event_attr *attr,
				 struct parse_events_term *term,
				 struct parse_events_error *err)
{
	if (term->type_term == PARSE_EVENTS__TERM_TYPE_USER) {
		size_t i;

		for (i = 0; i < ARRAY_SIZE(nvme_events); i++) {
			if (!strcasecmp(term->config, nvme_events[i].name)) {
				attr->config = nvme_events[i].config;
				return 0;
			}
		}
	}
	if (err) {
		char *err_str;

		parse_events_error__handle(err, term->err_val,
					asprintf(&err_str,
						"unexpected nvme event term (%s) %s",
						parse_events__term_type_str(term->type_term),
						term->config) < 0
					? strdup("unexpected nvme event term")
					: err_str,
					NULL);
	}
	return -EINVAL;
}

int nvme_pmu__config_terms(const struct perf_pmu *pmu,
			   struct perf_event_attr *attr,
			   struct parse_events_terms *terms,
			   struct parse_events_error *err)
{
	struct nvme_pmu *nvm = container_of(pmu, struct nvme_pmu, pmu);
	struct parse_events_term *term;

	list_for_each_entry(term, &terms->terms, list) {
		if (nvme_pmu__config_term(nvm, attr, term, err))
			return -EINVAL;
	}

	return 0;
}

int nvme_pmu__check_alias(struct parse_events_terms *terms, struct perf_pmu_info *info,
			  struct parse_events_error *err)
{
	struct parse_events_term *term =
		list_first_entry(&terms->terms, struct parse_events_term, list);

	if (term->type_term == PARSE_EVENTS__TERM_TYPE_USER) {
		size_t i;

		for (i = 0; i < ARRAY_SIZE(nvme_events); i++) {
			if (!strcasecmp(term->config, nvme_events[i].name)) {
				if (nvme_events[i].scale_unit) {
					char *unit;

					perf_pmu__convert_scale(nvme_events[i].scale_unit,
								&unit, &info->scale);
					info->unit = unit;
				}
				return 0;
			}
		}
	}
	if (err) {
		char *err_str;

		parse_events_error__handle(err, term->err_val,
					asprintf(&err_str,
						"unexpected nvme event term (%s) %s",
						parse_events__term_type_str(term->type_term),
						term->config) < 0
					? strdup("unexpected nvme event term")
					: err_str,
					NULL);
	}
	return -EINVAL;
}

int perf_pmus__read_nvme_pmus(struct list_head *pmus)
{
	nvme_root_t r = nvme_scan(NULL);
	nvme_host_t h;
	nvme_subsystem_t s;
	nvme_ctrl_t c;

	if (!r)
		return 0;

	nvme_for_each_host(r, h) {
		nvme_for_each_subsystem(h, s) {
			nvme_subsystem_for_each_ctrl(s, c) {
				nvme_pmu__new(pmus, nvme_ctrl_get_name(c), nvme_ctrl_get_name(c));
			}
		}
	}
	nvme_free_tree(r);
	return 0;
}


static int nvme_pmu__read_val(int fd, uint64_t config, uint64_t *val)
{
	int log_type = (config >> 24) & 0xFF;
	unsigned int size = (config >> 16) & 0xFF;
	unsigned int offset = config & 0xFFFF;
	uint8_t buf[4096];
	uint8_t *p;

	if (log_type == NVME_LOG_SMART) {
		if (offset + size > sizeof(struct nvme_smart_log))
			return -EINVAL;
		if (nvme_get_log_smart(fd, NVME_NSID_ALL, true, (struct nvme_smart_log *)buf) != 0)
			return -EINVAL;

		if (offset == offsetof(struct nvme_smart_log, temperature)) {
			uint64_t kelvin = ((struct nvme_smart_log *)buf)->temperature[0] |
					  (((struct nvme_smart_log *)buf)->temperature[1] << 8);
			*val = (kelvin * 1000) - 273150;
			return 0;
		}
	} else if (log_type == NVME_LOG_ENDURANCE) {
		if (offset + size > sizeof(struct nvme_endurance_group_log))
			return -EINVAL;
		if (nvme_get_log_endurance_group(fd, 0,
				(struct nvme_endurance_group_log *)buf) != 0)
			return -EINVAL;
	} else if (log_type == NVME_LOG_FDP) {
		if (offset + size > sizeof(struct nvme_fdp_stats_log))
			return -EINVAL;
		if (nvme_get_log_fdp_stats(fd, 0, 0, sizeof(struct nvme_fdp_stats_log), buf) != 0)
			return -EINVAL;
	} else if (log_type == NVME_LOG_ERROR) {
		if (offset + size > sizeof(struct nvme_error_log_page))
			return -EINVAL;
		if (nvme_get_log_error(fd, 1, true, (struct nvme_error_log_page *)buf) != 0)
			return -EINVAL;
	} else if (log_type == NVME_LOG_ZNS) {
		if (offset + size > sizeof(struct nvme_zns_changed_zone_log))
			return -EINVAL;
		if (nvme_get_log_zns_changed_zones(fd, NVME_NSID_ALL, true,
				(struct nvme_zns_changed_zone_log *)buf) != 0)
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	p = buf + offset;
	if (size == 16 || size == 8)
		*val = le64_to_cpu(*(uint64_t *)p);
	else if (size == 4)
		*val = le32_to_cpu(*(uint32_t *)p);
	else if (size == 2)
		*val = le16_to_cpu(*(uint16_t *)p);
	else if (size == 1)
		*val = *(uint8_t *)p;
	else
		return -EINVAL;

	return 0;
}

static bool nvme_pmu__is_gauge(uint64_t config)
{
	if (config == NVME_SMART(2, temperature) ||
	    config == NVME_ENDURANCE(1, percent_used) ||
	    config == NVME_ZNS(2, nrzid))
		return true;
	return false;
}

#define FD(e, x, y) (*(int *)xyarray__entry(e->core.fd, x, y))

int evsel__nvme_pmu_open(struct evsel *evsel,
			 struct perf_thread_map *threads,
			 int start_cpu_map_idx, int end_cpu_map_idx)
{
	struct nvme_pmu *nvm = container_of(evsel->pmu, struct nvme_pmu, pmu);
	int idx = 0, thread = 0, nthreads, err = 0;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/dev/%s", nvm->dev_name);

	nthreads = perf_thread_map__nr(threads);

	if (!evsel->priv) {
		int max_cpus = evsel->core.cpus ? perf_cpu_map__nr(evsel->core.cpus) : 1;

		evsel->priv = xyarray__new(max_cpus, nthreads, sizeof(uint64_t));
	}

	for (idx = start_cpu_map_idx; idx < end_cpu_map_idx; idx++) {
		for (thread = 0; thread < nthreads; thread++) {
			int fd = open(path, O_RDONLY);

			FD(evsel, idx, thread) = fd;
			if (fd < 0) {
				err = -errno;
				goto out_close;
			}
			if (evsel->priv) {
				uint64_t *initial_val = xyarray__entry(evsel->priv, idx, thread);

				if (nvme_pmu__read_val(fd, evsel->core.attr.config, initial_val))
					*initial_val = 0;
			}
		}
	}
	return 0;
out_close:
	if (err)
		threads->err_thread = thread;

	do {
		while (--thread >= 0) {
			if (FD(evsel, idx, thread) >= 0)
				close(FD(evsel, idx, thread));
			FD(evsel, idx, thread) = -1;
		}
		thread = nthreads;
	} while (--idx >= 0);
	return err;
}

int evsel__nvme_pmu_read(struct evsel *evsel, int cpu_map_idx, int thread)
{
	int fd;
	struct perf_counts_values *count, *old_count = NULL;
	uint64_t val = 0;
	uint64_t *initial_val = NULL;

	if (evsel->prev_raw_counts)
		old_count = perf_counts(evsel->prev_raw_counts, cpu_map_idx, thread);

	count = perf_counts(evsel->counts, cpu_map_idx, thread);
	fd = FD(evsel, cpu_map_idx, thread);

	if (fd < 0 || nvme_pmu__read_val(fd, evsel->core.attr.config, &val)) {
		count->lost++;
		return -EINVAL;
	}

	if (evsel->priv)
		initial_val = xyarray__entry(evsel->priv, cpu_map_idx, thread);

	if (old_count) {
		if (nvme_pmu__is_gauge(evsel->core.attr.config))
			count->val = old_count->val + val;
		else
			count->val = val - (initial_val ? *initial_val : 0);
		count->run = old_count->run + 1;
		count->ena = old_count->ena + 1;
	} else {
		if (nvme_pmu__is_gauge(evsel->core.attr.config))
			count->val = val;
		else
			count->val = val - (initial_val ? *initial_val : 0);
		count->run++;
		count->ena++;
	}
	return 0;
}


#endif
