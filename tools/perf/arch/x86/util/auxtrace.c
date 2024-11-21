// SPDX-License-Identifier: GPL-2.0-only
/*
 * auxtrace.c: AUX area tracing support
 * Copyright (c) 2013-2014, Intel Corporation.
 */

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>

#include "../../../util/header.h"
#include "../../../util/debug.h"
#include "../../../util/pmu.h"
#include "../../../util/pmus.h"
#include "../../../util/auxtrace.h"
#include "../../../util/intel-pt.h"
#include "../../../util/intel-bts.h"
#include "../../../util/cxl-hmu.h"
#include "../../../util/evlist.h"

static
struct auxtrace_record *auxtrace_record__init_intel(struct evlist *evlist,
						    int *err)
{
	struct perf_pmu *intel_pt_pmu;
	struct perf_pmu *intel_bts_pmu;
	struct evsel *evsel;
	bool found_pt = false;
	bool found_bts = false;

	intel_pt_pmu = perf_pmus__find(INTEL_PT_PMU_NAME);
	intel_bts_pmu = perf_pmus__find(INTEL_BTS_PMU_NAME);

	evlist__for_each_entry(evlist, evsel) {
		if (intel_pt_pmu && evsel->core.attr.type == intel_pt_pmu->type)
			found_pt = true;
		if (intel_bts_pmu && evsel->core.attr.type == intel_bts_pmu->type)
			found_bts = true;
	}

	if (found_pt && found_bts) {
		pr_err("intel_pt and intel_bts may not be used together\n");
		*err = -EINVAL;
		return NULL;
	}

	if (found_pt)
		return intel_pt_recording_init(err);

	if (found_bts)
		return intel_bts_recording_init(err);

	return NULL;
}

static struct perf_pmu **find_all_cxl_hmu_pmus(int *nr_hmus, int *err)
{
	struct perf_pmu **cxl_hmu_pmus = NULL;
	struct dirent *dent;
	char path[PATH_MAX];
	DIR *dir = NULL;
	int idx = 0;

	perf_pmu__event_source_devices_scnprintf(path, sizeof(path));
	dir = opendir(path);
	if (!dir) {
		*err = -EINVAL;
		return NULL;
	}

	while ((dent = readdir(dir))) {
		if (strstr(dent->d_name, "cxl_hmu"))
			(*nr_hmus)++;
	}

	if (!(*nr_hmus))
		goto out;

	cxl_hmu_pmus = zalloc(sizeof(struct perf_pmu *) * (*nr_hmus));
	if (!cxl_hmu_pmus) {
		*err = -ENOMEM;
		goto out;
	}

	rewinddir(dir);
	while ((dent = readdir(dir))) {
		if (strstr(dent->d_name, "cxl_hmu") && idx < *nr_hmus) {
			cxl_hmu_pmus[idx] = perf_pmus__find(dent->d_name);
			if (cxl_hmu_pmus[idx])
				idx++;
		}
	}

out:
	closedir(dir);
	return cxl_hmu_pmus;
}

static struct perf_pmu *find_pmu_for_event(struct perf_pmu **pmus,
					   int pmu_nr, struct evsel *evsel)
{
	int i;

	if (!pmus)
		return NULL;

	for (i = 0; i < pmu_nr; i++) {
		if (evsel->core.attr.type == pmus[i]->type)
			return pmus[i];
	}

	return NULL;
}

struct auxtrace_record *auxtrace_record__init(struct evlist *evlist,
					      int *err)
{
	char buffer[64];
	int ret;
	struct perf_pmu **chmu_pmus = NULL;
	struct perf_pmu *found_chmu = NULL;
	struct evsel *evsel;
	int nr_chmus = 0;

	*err = 0;

	chmu_pmus = find_all_cxl_hmu_pmus(&nr_chmus, err);

	evlist__for_each_entry(evlist, evsel) {
		if (chmu_pmus && !found_chmu)
			found_chmu = find_pmu_for_event(chmu_pmus, nr_chmus, evsel);
	}
	free(chmu_pmus);

	if (found_chmu)
		return chmu_recording_init(err, found_chmu);

	ret = get_cpuid(buffer, sizeof(buffer));
	if (ret) {
		*err = ret;
		return NULL;
	}

	if (!strncmp(buffer, "GenuineIntel,", 13))
		return auxtrace_record__init_intel(evlist, err);

	return NULL;
}
