// SPDX-License-Identifier: GPL-2.0
/*
 * Resctrl PMU test
 *
 * Test program to verify the resctrl PMU functionality.
 * Walks resctrl filesystem and verifies only allowed monitoring files
 * can be used with the resctrl PMU via perf_event_open when pinned to
 * CPUs in the correct L3 domain. Also validates that PID-bound events
 * are rejected for all files.
 */

#include "resctrl.h"
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>

#define RESCTRL_PMU_NAME "resctrl"

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

/* Extract base filename from a path */
static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

/* Parse mon_L3_XX ID from a monitoring path. Returns true on success. */
static bool parse_l3_id_from_path(const char *path, int *l3_id)
{
	const char *needle = "mon_data/mon_L3_";
	const char *p = strstr(path, needle);
	char *endptr;
	long id;

	if (!p)
		return false;

	p += strlen(needle);

	if (!isdigit((unsigned char)*p))
		return false;

	errno = 0;
	id = strtol(p, &endptr, 10);
	if (errno || endptr == p)
		return false;

	/* Accept only non-negative IDs */
	if (id < 0)
		return false;

	*l3_id = (int)id;
	return true;
}

static int test_file_safety(int pmu_type, const char *filepath)
{
	struct perf_event_attr pe = { 0 };
	int fd, perf_fd;
	bool is_monitoring = false;
	int file_l3_id = -1;
	int ret = 0;
	const char *fname = base_name(filepath);

	/* Try to open the file */
	fd = open(filepath, O_RDONLY);
	if (fd < 0) {
		/* File couldn't be opened, skip it */
		return 0;
	}

	/* Determine if this is a monitoring file under mon_L3_XX and allowed */
	is_monitoring = (is_allowed_file(fname) && parse_l3_id_from_path(filepath, &file_l3_id));

	/* Setup perf event attributes */
	pe.type = pmu_type;
	pe.config = fd;
	pe.size = sizeof(pe);
	pe.disabled = 1;
	pe.exclude_kernel = 0;
	pe.exclude_hv = 0;

	/* PID-bound negative attempt: should fail for all files */
	perf_fd = perf_event_open(&pe, getpid(), -1, -1, 0);
	if (perf_fd >= 0) {
		ksft_print_msg("FAIL: pid-bound perf_event_open unexpectedly succeeded for %s\n",
			       filepath);
		close(perf_fd);
		close(fd);
		return -1;
	}

	int success_count = 0;
	cpu_set_t mask;
	int max_cpus, nconf;

	CPU_ZERO(&mask);
	if (sched_getaffinity(0, sizeof(mask), &mask)) {
		ksft_perror("sched_getaffinity failed");
		goto out;
	}

	nconf = (int)sysconf(_SC_NPROCESSORS_CONF);
	max_cpus = (nconf > 0 && nconf < CPU_SETSIZE) ? nconf : CPU_SETSIZE;

	for (int cpu = 0; cpu < max_cpus; cpu++) {
		int cpu_l3;

		if (!CPU_ISSET(cpu, &mask))
			continue;

		if (get_domain_id("L3", cpu, &cpu_l3) < 0) {
			ksft_print_msg("Failed to get L3 domain ID for CPU %d\n", cpu);
			ret = -1;
			break;
		}

		perf_fd = perf_event_open(&pe, -1, cpu, -1, 0);

		if (is_monitoring) {
			bool expected_ok = (cpu_l3 == file_l3_id);

			if (expected_ok) {
				if (perf_fd < 0) {
					ksft_print_msg("FAIL: %s CPU %d (L3=%d) expected success, got %s\n",
						       filepath, cpu, cpu_l3, strerror(errno));
					ret = -1;
					break;
				}
				success_count++;
				close(perf_fd);
			} else {
				if (perf_fd >= 0) {
					ksft_print_msg("FAIL: %s CPU %d (L3=%d) expected EINVAL fail, but opened\n",
						       filepath, cpu, cpu_l3);
					close(perf_fd);
					ret = -1;
					break;
				}
				if (errno != EINVAL) {
					ksft_print_msg("FAIL: %s CPU %d expected errno=EINVAL, got %d (%s)\n",
						       filepath, cpu, errno, strerror(errno));
					ret = -1;
					break;
				}
			}
		} else {
			/* Non-monitoring files must fail on all CPUs with EINVAL */
			if (perf_fd >= 0) {
				ksft_print_msg("FAIL: non-monitoring file %s CPU %d unexpectedly opened\n",
					       filepath, cpu);
				close(perf_fd);
				ret = -1;
				break;
			}
			if (errno != EINVAL) {
				ksft_print_msg("FAIL: non-monitoring file %s CPU %d expected errno=EINVAL, got %d (%s)\n",
					       filepath, cpu, errno, strerror(errno));
				ret = -1;
				break;
			}
		}
	}

	if (!ret && is_monitoring && success_count < 1) {
		ksft_print_msg("FAIL: monitoring file %s had no successful CPU opens\n",
			       filepath);
		ret = -1;
	}

	if (!ret) {
		if (is_monitoring)
			ksft_print_msg("PASS: monitoring %s: %d CPU(s) opened in-domain, others rejected\n",
				       filepath, success_count);
		else
			ksft_print_msg("PASS: non-monitoring %s: all CPU-bound opens rejected with EINVAL\n",
				       filepath);
	}

out:
	close(fd);
	return ret;
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
	pmu_type = resctrl_find_pmu_type(RESCTRL_PMU_NAME);
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
