// SPDX-License-Identifier: GPL-2.0
#include "builtin.h"
#include "color.h"
#include "util/debug.h"
#include "util/header.h"
#include <tools/config.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <subcmd/parse-options.h>

#if defined(__has_feature)
#define HAS_COMPILER_FEATURE(feature) __has_feature(feature)
#else
#define HAS_COMPILER_FEATURE(feature) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(ADDRESS_SANITIZER) || \
	HAS_COMPILER_FEATURE(address_sanitizer)
#define HAVE_SANITIZER_ADDRESS 1
#define HAVE_SANITIZER_LEAK 1
#elif defined(LEAK_SANITIZER) || HAS_COMPILER_FEATURE(leak_sanitizer)
#define HAVE_SANITIZER_ADDRESS 0
#define HAVE_SANITIZER_LEAK 1
#else
#define HAVE_SANITIZER_ADDRESS 0
#define HAVE_SANITIZER_LEAK 0
#endif

#if defined(MEMORY_SANITIZER) || HAS_COMPILER_FEATURE(memory_sanitizer)
#define HAVE_SANITIZER_MEMORY 1
#else
#define HAVE_SANITIZER_MEMORY 0
#endif

#if defined(THREAD_SANITIZER) || HAS_COMPILER_FEATURE(thread_sanitizer)
#define HAVE_SANITIZER_THREAD 1
#else
#define HAVE_SANITIZER_THREAD 0
#endif

#if defined(UNDEFINED_SANITIZER) || HAS_COMPILER_FEATURE(undefined_sanitizer)
#define HAVE_SANITIZER_UNDEFINED 1
#else
#define HAVE_SANITIZER_UNDEFINED 0
#endif

#if HAVE_SANITIZER_ADDRESS || HAVE_SANITIZER_LEAK || HAVE_SANITIZER_MEMORY || \
	HAVE_SANITIZER_THREAD || HAVE_SANITIZER_UNDEFINED
#define HAVE_SANITIZER 1
#else
#define HAVE_SANITIZER 0
#endif

static const char * const check_subcommands[] = { "feature", NULL };
static struct option check_options[] = {
	OPT_BOOLEAN('q', "quiet", &quiet, "do not show any warnings or messages"),
	OPT_END()
};
static struct option check_feature_options[] = { OPT_PARENT(check_options) };

static const char *check_usage[] = { NULL, NULL };
static const char *check_feature_usage[] = {
	"perf check feature <feature_list>",
	NULL
};

struct feature_status supported_features[] = {
	FEATURE_STATUS("aio", HAVE_AIO_SUPPORT),
	FEATURE_STATUS("bpf", HAVE_LIBBPF_SUPPORT),
	FEATURE_STATUS("bpf_skeletons", HAVE_BPF_SKEL),
	FEATURE_STATUS("debuginfod", HAVE_DEBUGINFOD_SUPPORT),
	FEATURE_STATUS("dwarf", HAVE_LIBDW_SUPPORT),
	FEATURE_STATUS("dwarf_getlocations", HAVE_LIBDW_SUPPORT),
	FEATURE_STATUS("dwarf-unwind", HAVE_DWARF_UNWIND_SUPPORT),
	FEATURE_STATUS("auxtrace", HAVE_AUXTRACE_SUPPORT),
	FEATURE_STATUS("libaudit", HAVE_LIBAUDIT_SUPPORT),
	FEATURE_STATUS("libbfd", HAVE_LIBBFD_SUPPORT),
	FEATURE_STATUS("libcapstone", HAVE_LIBCAPSTONE_SUPPORT),
	FEATURE_STATUS("libcrypto", HAVE_LIBCRYPTO_SUPPORT),
	FEATURE_STATUS("libdw-dwarf-unwind", HAVE_LIBDW_SUPPORT),
	FEATURE_STATUS("libelf", HAVE_LIBELF_SUPPORT),
	FEATURE_STATUS("libnuma", HAVE_LIBNUMA_SUPPORT),
	FEATURE_STATUS("libopencsd", HAVE_CSTRACE_SUPPORT),
	FEATURE_STATUS("libperl", HAVE_LIBPERL_SUPPORT),
	FEATURE_STATUS("libpfm4", HAVE_LIBPFM),
	FEATURE_STATUS("libpython", HAVE_LIBPYTHON_SUPPORT),
	FEATURE_STATUS("libslang", HAVE_SLANG_SUPPORT),
	FEATURE_STATUS("libtraceevent", HAVE_LIBTRACEEVENT),
	FEATURE_STATUS("libunwind", HAVE_LIBUNWIND_SUPPORT),
	FEATURE_STATUS("lzma", HAVE_LZMA_SUPPORT),
	FEATURE_STATUS("numa_num_possible_cpus", HAVE_LIBNUMA_SUPPORT),
	FEATURE_STATUS("sanitizer", HAVE_SANITIZER),
	FEATURE_STATUS("sanitizer_address", HAVE_SANITIZER_ADDRESS),
	FEATURE_STATUS("sanitizer_leak", HAVE_SANITIZER_LEAK),
	FEATURE_STATUS("sanitizer_memory", HAVE_SANITIZER_MEMORY),
	FEATURE_STATUS("sanitizer_thread", HAVE_SANITIZER_THREAD),
	FEATURE_STATUS("sanitizer_undefined", HAVE_SANITIZER_UNDEFINED),
	FEATURE_STATUS("syscall_table", HAVE_SYSCALL_TABLE_SUPPORT),
	FEATURE_STATUS("zlib", HAVE_ZLIB_SUPPORT),
	FEATURE_STATUS("zstd", HAVE_ZSTD_SUPPORT),

	/* this should remain at end, to know the array end */
	FEATURE_STATUS(NULL, _)
};

static void on_off_print(const char *status)
{
	printf("[ ");

	if (!strcmp(status, "OFF"))
		color_fprintf(stdout, PERF_COLOR_RED, "%-3s", status);
	else
		color_fprintf(stdout, PERF_COLOR_GREEN, "%-3s", status);

	printf(" ]");
}

/* Helper function to print status of a feature along with name/macro */
static void status_print(const char *name, const char *macro,
			 const char *status)
{
	printf("%22s: ", name);
	on_off_print(status);
	printf("  # %s\n", macro);
}

#define STATUS(feature)                                           \
do {                                                              \
	if (feature.is_builtin)                                   \
		status_print(feature.name, feature.macro, "on");  \
	else                                                      \
		status_print(feature.name, feature.macro, "OFF"); \
} while (0)

/**
 * check whether "feature" is built-in with perf
 *
 * returns:
 *    0: NOT built-in or Feature not known
 *    1: Built-in
 */
static int has_support(const char *feature)
{
	for (int i = 0; supported_features[i].name; ++i) {
		if ((strcasecmp(feature, supported_features[i].name) == 0) ||
		    (strcasecmp(feature, supported_features[i].macro) == 0)) {
			if (!quiet)
				STATUS(supported_features[i]);
			return supported_features[i].is_builtin;
		}
	}

	if (!quiet)
		pr_err("Unknown feature '%s', please use 'perf version --build-options' to see which ones are available.\n", feature);

	return 0;
}


/**
 * Usage: 'perf check feature <feature_list>'
 *
 * <feature_list> can be a single feature name/macro, or a comma-separated list
 * of feature names/macros
 * eg. argument can be "libtraceevent" or "libtraceevent,bpf" etc
 *
 * In case of a comma-separated list, feature_enabled will be 1, only if
 * all features passed in the string are supported
 *
 * Note that argv will get modified
 */
static int subcommand_feature(int argc, const char **argv)
{
	char *feature_list;
	char *feature_name;
	int feature_enabled;

	argc = parse_options(argc, argv, check_feature_options,
			check_feature_usage, 0);

	if (!argc)
		usage_with_options(check_feature_usage, check_feature_options);

	if (argc > 1) {
		pr_err("Too many arguments passed to 'perf check feature'\n");
		return -1;
	}

	feature_enabled = 1;
	/* feature_list is a non-const copy of 'argv[0]' */
	feature_list = strdup(argv[0]);
	if (!feature_list) {
		pr_err("ERROR: failed to allocate memory for feature list\n");
		return -1;
	}

	feature_name = strtok(feature_list, ",");

	while (feature_name) {
		feature_enabled &= has_support(feature_name);
		feature_name = strtok(NULL, ",");
	}

	free(feature_list);

	return !feature_enabled;
}

int cmd_check(int argc, const char **argv)
{
	argc = parse_options_subcommand(argc, argv, check_options,
			check_subcommands, check_usage, 0);

	if (!argc)
		usage_with_options(check_usage, check_options);

	if (strcmp(argv[0], "feature") == 0)
		return subcommand_feature(argc, argv);

	/* If no subcommand matched above, print usage help */
	pr_err("Unknown subcommand: %s\n", argv[0]);
	usage_with_options(check_usage, check_options);

	/* free usage string allocated by parse_options_subcommand */
	free((void *)check_usage[0]);

	return 0;
}
