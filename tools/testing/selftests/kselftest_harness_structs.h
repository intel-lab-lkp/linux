#ifndef __KSELFTEST_HARNESS_STRUCTS_H
#define __KSELFTEST_HARNESS_STRUCTS_H

#include <stdbool.h>
#include <sys/types.h>

struct __test_metadata;
struct __fixture_variant_metadata;

/* Contains all the information about a fixture. */
struct __fixture_metadata {
	const char *name;
	struct __test_metadata *tests;
	struct __fixture_variant_metadata *variant;
	struct __fixture_metadata *prev, *next;
};

struct __test_xfail {
	struct __fixture_metadata *fixture;
	struct __fixture_variant_metadata *variant;
	struct __test_metadata *test;
	struct __test_xfail *prev, *next;
};

struct __fixture_variant_metadata {
	const char *name;
	const void *data;
	struct __test_xfail *xfails;
	struct __fixture_variant_metadata *prev, *next;
};

/* Contains all the information for test execution and status checking. */
struct __test_metadata {
	const char *name;
	void (*fn)(struct __test_metadata *,
		   struct __fixture_variant_metadata *);
	pid_t pid;	/* pid of test when being run */
	struct __fixture_metadata *fixture;
	void (*teardown_fn)(bool in_parent, struct __test_metadata *_metadata,
			    void *self, const void *variant);
	int termsig;
	int exit_code;
	int trigger; /* extra handler after the evaluation */
	int timeout;	/* seconds to wait for test timeout */
	bool aborted;	/* stopped test due to failed ASSERT */
	bool *no_teardown; /* fixture needs teardown */
	void *self;
	const void *variant;
	struct __test_results *results;
	struct __test_metadata *prev, *next;
};

#endif
