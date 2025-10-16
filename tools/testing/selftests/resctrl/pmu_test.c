// SPDX-License-Identifier: GPL-2.0
/*
 * Resctrl PMU test
 *
 * Test program to verify the resctrl PMU functionality.
 * Walks resctrl filesystem and verifies only allowed files can be
 * used with the resctrl PMU via perf_event_open.
 */

#include "resctrl.h"
#include <fcntl.h>
#include <dirent.h>

#define RESCTRL_PMU_NAME "resctrl"

static int find_pmu_type(const char *pmu_name)
{
	char path[256];
	FILE *file;
	int type;

	snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type",
		 pmu_name);

	file = fopen(path, "r");
	if (!file) {
		ksft_print_msg("Failed to open %s: %s\n", path,
			       strerror(errno));
		return -1;
	}

	if (fscanf(file, "%d", &type) != 1) {
		ksft_print_msg("Failed to read PMU type from %s\n", path);
		fclose(file);
		return -1;
	}

	fclose(file);
	return type;
}

static bool is_allowed_file(const char *filename)
{
	const char *base;

	/* Only exact llc_occupancy and mbm files (no *_config) are allowed */
	base = strrchr(filename, '/');
	base = base ? base + 1 : filename;

	return (!strcmp(base, "llc_occupancy") ||
		!strcmp(base, "mbm_total_bytes") ||
		!strcmp(base, "mbm_local_bytes"));
}

static int test_file_safety(int pmu_type, const char *filepath)
{
	struct perf_event_attr pe = { 0 };
	int fd, perf_fd;
	bool should_succeed;

	/* Try to open the file */
	fd = open(filepath, O_RDONLY);
	if (fd < 0) {
		/* File couldn't be opened, skip it */
		return 0;
	}

	should_succeed = is_allowed_file(filepath);

	/* Setup perf event attributes */
	pe.type = pmu_type;
	pe.config = fd;
	pe.size = sizeof(pe);
	pe.disabled = 1;
	pe.exclude_kernel = 0;
	pe.exclude_hv = 0;

	/* Try to open the perf event */
	perf_fd = perf_event_open(&pe, -1, 0, -1, 0);

	if (should_succeed) {
		if (perf_fd < 0) {
			ksft_print_msg("FAIL: unexpected - perf_event_open failed for %s: %s\n",
				       filepath, strerror(errno));
			close(fd);
			return -1;
		}
		ksft_print_msg("PASS: Allowed file %s successfully opened perf event\n",
			       filepath);
		close(perf_fd);
	} else {
		if (perf_fd >= 0) {
			ksft_print_msg("FAIL: unexpected - perf_event_open succeeded for %s\n",
				       filepath);
			close(perf_fd);
			close(fd);
			return -1;
		}
		ksft_print_msg("PASS: Blocked file %s correctly failed perf_event_open: %s\n",
			       filepath, strerror(errno));
	}

out:
	close(fd);
	return 0;
}

static int walk_directory_recursive(int pmu_type, const char *dir_path)
{
	DIR *dir;
	struct dirent *entry;
	char full_path[1024];
	struct stat statbuf;
	int ret = 0;

	dir = opendir(dir_path);
	if (!dir) {
		ksft_print_msg("Failed to open directory %s: %s\n", dir_path,
			       strerror(errno));
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		/* Skip . and .. */
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;

		snprintf(full_path, sizeof(full_path), "%s/%s", dir_path,
			 entry->d_name);

		if (stat(full_path, &statbuf) != 0) {
			ksft_print_msg("Failed to stat %s: %s\n", full_path,
				       strerror(errno));
			continue;
		}

		if (S_ISDIR(statbuf.st_mode)) {
			/* Recursively walk subdirectories */
			if (walk_directory_recursive(pmu_type, full_path) != 0)
				ret = -1;
		} else if (S_ISREG(statbuf.st_mode)) {
			/* Test regular files */
			if (test_file_safety(pmu_type, full_path) != 0)
				ret = -1;
		}
	}

	closedir(dir);
	return ret;
}

static int test_resctrl_pmu_safety(int pmu_type)
{
	ksft_print_msg("Testing resctrl PMU safety - walking all files in %s\n",
		       RESCTRL_PATH);

	/* Walk through all files and directories in /sys/fs/resctrl */
	return walk_directory_recursive(pmu_type, RESCTRL_PATH);
}

static bool pmu_feature_check(const struct resctrl_test *test)
{
	return resctrl_mon_feature_exists("L3_MON", "llc_occupancy");
}

static int pmu_run_test(const struct resctrl_test *test,
			const struct user_params *uparams)
{
	int pmu_type, ret;

	ksft_print_msg("Testing resctrl PMU file access safety\n");

	/* Find the resctrl PMU type */
	pmu_type = find_pmu_type(RESCTRL_PMU_NAME);
	if (pmu_type < 0) {
		ksft_print_msg("Resctrl PMU not found - PMU is not registered?\n");
		return -1;
	}

	ksft_print_msg("Found resctrl PMU with type: %d\n", pmu_type);

	/* Run the safety test to ensure only appropriate files work */
	ret = test_resctrl_pmu_safety(pmu_type);

	if (ret == 0)
		ksft_print_msg("Resctrl PMU safety test completed successfully\n");
	else
		ksft_print_msg("Resctrl PMU safety test failed\n");

	return ret;
}

struct resctrl_test pmu_test = {
	.name = "PMU",
	.group = "pmu",
	.resource = "L3",
	.vendor_specific = 0,
	.feature_check = pmu_feature_check,
	.run_test = pmu_run_test,
	.cleanup = NULL,
};
