// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "nvme_pmu.h"

#include <errno.h>
#include <inttypes.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "debug.h"
#include "evlist.h"
#include "parse-events.h"
#include "pmus.h"
#include "tests.h"

#ifdef HAVE_LIBNVME_SUPPORT

static const struct test_event {
	const char *name;
	const char *alias;
	uint64_t config;
} test_events[] = {
	{
		"smart_temperature",
		"smart_temperature",
		NVME_SMART(2, temperature),
	},
	{
		"smart_data_units_read",
		"smart_data_units_read",
		NVME_SMART(16, data_units_read),
	},
	{
		"endurance_percent_used",
		"endurance_percent_used",
		NVME_ENDURANCE(1, percent_used),
	},
	{
		"fdp_hbmw",
		"fdp_hbmw",
		NVME_FDP(16, hbmw),
	},
	{
		"error_count",
		"error_count",
		NVME_ERROR(8, error_count),
	},
	{
		"zns_nrzid",
		"zns_nrzid",
		NVME_ZNS(2, nrzid),
	},
};

static int do_test(size_t i, bool with_pmu, bool with_alias)
{
	const char *test_event = with_alias ? test_events[i].alias : test_events[i].name;
	struct evlist *evlist = evlist__new();
	struct evsel *evsel;
	struct parse_events_error err;
	int ret;
	char str[128];
	bool found = false;

	if (!evlist) {
		pr_err("evlist allocation failed\n");
		return TEST_FAIL;
	}

	if (with_pmu)
		snprintf(str, sizeof(str), "nvme_nvme0/%s/", test_event);
	else
		strlcpy(str, test_event, sizeof(str));

	pr_debug("Testing '%s'\n", str);
	parse_events_error__init(&err);
	ret = parse_events(evlist, str, &err);
	if (ret) {
		pr_debug("FAILED %s:%d failed to parse event '%s', err %d\n",
			 __FILE__, __LINE__, str, ret);
		parse_events_error__print(&err, str);
		ret = TEST_FAIL;
		goto out;
	}

	ret = TEST_OK;
	if (with_pmu ? (evlist->core.nr_entries != 1) : (evlist->core.nr_entries < 1)) {
		pr_debug("FAILED %s:%d Unexpected number of events for '%s' of %d\n",
			 __FILE__, __LINE__, str, evlist->core.nr_entries);
		ret = TEST_FAIL;
		goto out;
	}

	evlist__for_each_entry(evlist, evsel) {
		if (!evsel->pmu || !evsel->pmu->name ||
		    strcmp(evsel->pmu->name, "nvme_nvme0"))
			continue;

		if (evsel->core.attr.config != test_events[i].config) {
			pr_debug("FAILED %s:%d Unexpected config for '%s', %"
				 PRIu64 " != %" PRIu64 "\n",
				 __FILE__, __LINE__, str,
				 (uint64_t)evsel->core.attr.config,
				 test_events[i].config);
			ret = TEST_FAIL;
			goto out;
		}
		found = true;
	}

	if (!found) {
		pr_debug("FAILED %s:%d Didn't find nvme event '%s' in parsed evsels\n",
			 __FILE__, __LINE__, str);
		ret = TEST_FAIL;
	}

out:
	parse_events_error__exit(&err);
	evlist__delete(evlist);
	return ret;
}

static int test__nvme_pmu(bool with_pmu)
{
	struct perf_pmu *pmu = perf_pmus__add_test_nvme_pmu("nvme0", "nvme0");
	int ret = TEST_OK;

	if (!pmu)
		return TEST_FAIL;

	for (size_t i = 0; i < ARRAY_SIZE(test_events); i++) {
		ret = do_test(i, with_pmu, /*with_alias=*/false);
		if (ret != TEST_OK)
			break;

		ret = do_test(i, with_pmu, /*with_alias=*/true);
		if (ret != TEST_OK)
			break;
	}

	list_del(&pmu->list);
	perf_pmu__delete(pmu);
	return ret;
}

static int test__nvme_pmu_without_pmu(struct test_suite *test __maybe_unused,
				      int subtest __maybe_unused)
{
	return test__nvme_pmu(/*with_pmu=*/false);
}

static int test__nvme_pmu_with_pmu(struct test_suite *test __maybe_unused,
				   int subtest __maybe_unused)
{
	return test__nvme_pmu(/*with_pmu=*/true);
}

static struct test_case tests__nvme_pmu[] = {
	TEST_CASE("Parsing without PMU name", nvme_pmu_without_pmu),
	TEST_CASE("Parsing with PMU name", nvme_pmu_with_pmu),
	{	.name = NULL, }
};

struct test_suite suite__nvme_pmu = {
	.desc = "NVMe PMU",
	.test_cases = tests__nvme_pmu,
};

#else

struct test_suite suite__nvme_pmu = {
	.desc = "NVMe PMU",
	.test_cases = NULL,
};

#endif
