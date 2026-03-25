// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "tests.h"
#include "debug.h"
#include "parse-events.h"
#include "pmu.h"
#include "pmus.h"
#include "evlist.h"
#include <string.h>

struct match_state {
	char *event1;
	char *event2;
};

static int event_cb(void *state, struct pmu_event_info *info)
{
	struct match_state *m = state;

	if (!m->event1) {
		m->event1 = strdup(info->name);
	} else if (!m->event2) {
		if (strcmp(m->event1, info->name)) {
			m->event2 = strdup(info->name);
			return 1;
		}
	}
	return 0;
}

static int test__uncore_event_sorting(struct test_suite *test __maybe_unused,
				      int subtest __maybe_unused)
{
	struct evlist *evlist;
	struct parse_events_error err;
	struct evsel *evsel;
	struct perf_pmu *pmu = NULL;
	char *pmu_prefix = NULL;
	struct match_state m = { NULL, NULL };
	char buf[1024];
	int ret;

	while ((pmu = perf_pmus__scan(pmu)) != NULL) {
		size_t len;
		struct perf_pmu *sibling;

		if (pmu->is_core)
			continue;

		len = pmu_name_len_no_suffix(pmu->name);
		if (len == strlen(pmu->name))
			continue;

		sibling = pmu;
		while ((sibling = perf_pmus__scan(sibling)) != NULL) {
			if (sibling->is_core)
				continue;
			if (pmu_name_len_no_suffix(sibling->name) == len &&
			    !strncmp(pmu->name, sibling->name, len))
				break;
		}

		if (!sibling)
			continue;

		m.event1 = m.event2 = NULL;
		perf_pmu__for_each_event(pmu, false, &m, event_cb);

		if (m.event1 && m.event2) {
			pmu_prefix = strndup(pmu->name, len);
			break;
		}
		free(m.event1);
	}

	if (!pmu_prefix) {
		pr_debug("No suitable uncore PMU found\n");
		return TEST_SKIP;
	}

	evlist = evlist__new();
	if (!evlist)
		return TEST_FAIL;

	snprintf(buf, sizeof(buf), "{%s/%s/,%s/%s/}",
		 pmu_prefix, m.event1, pmu_prefix, m.event2);
	pr_debug("Parsing: %s\n", buf);

	parse_events_error__init(&err);
	ret = parse_events(evlist, buf, &err);
	if (ret) {
		pr_debug("parse_events failed\n");
		goto out_err;
	}

	TEST_ASSERT_VAL("Number of events is > 0", evlist->core.nr_entries > 0);
	TEST_ASSERT_EQUAL("Number of events is a multiple of 2", evlist->core.nr_entries % 2, 0);

	evlist__for_each_entry(evlist, evsel) {
		if (evsel__is_group_leader(evsel)) {
			struct evsel *next = evsel__next(evsel);

			TEST_ASSERT_EQUAL("Group size is 2", evsel->core.nr_members, 2);
			TEST_ASSERT_VAL("PMU match", evsel->pmu == next->pmu);
			TEST_ASSERT_VAL("First event name", strstr(evsel->name, m.event1) != NULL);
			TEST_ASSERT_VAL("Second event name", strstr(next->name, m.event2) != NULL);
		}
	}

	evlist__delete(evlist);
	parse_events_error__exit(&err);
	free(pmu_prefix);
	free(m.event1);
	free(m.event2);
	return TEST_OK;

out_err:
	evlist__delete(evlist);
	parse_events_error__exit(&err);
	free(pmu_prefix);
	free(m.event1);
	free(m.event2);
	return TEST_FAIL;
}

DEFINE_SUITE("Uncore event sorting", uncore_event_sorting);
