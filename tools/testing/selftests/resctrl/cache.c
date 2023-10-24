// SPDX-License-Identifier: GPL-2.0

#include <stdint.h>
#include "resctrl.h"

struct perf_event_read {
	__u64 nr;			/* The number of events */
	struct {
		__u64 value;		/* The value of the event */
	} values[2];
};

static struct perf_event_attr pea_llc_miss;
static struct perf_event_read pe_read;
static int pe_fd;
char llc_occup_path[1024];

static void perf_event_attr_initialize(__u64 config)
{
	memset(&pea_llc_miss, 0, sizeof(struct perf_event_attr));
	pea_llc_miss.type = PERF_TYPE_HARDWARE;
	pea_llc_miss.size = sizeof(struct perf_event_attr);
	pea_llc_miss.read_format = PERF_FORMAT_GROUP;
	pea_llc_miss.exclude_kernel = 1;
	pea_llc_miss.exclude_hv = 1;
	pea_llc_miss.exclude_idle = 1;
	pea_llc_miss.exclude_callchain_kernel = 1;
	pea_llc_miss.inherit = 1;
	pea_llc_miss.exclude_guest = 1;
	pea_llc_miss.disabled = 1;
	pea_llc_miss.config = config;
}

static void perf_event_initialize_read_format(void)
{
	memset(&pe_read, 0, sizeof(struct perf_event_read));
	pe_read.nr = 1;
}

static int perf_event_reset_enable(pid_t pid, int cpu_no)
{
	pe_fd = perf_event_open(&pea_llc_miss, pid, cpu_no, -1, PERF_FLAG_FD_CLOEXEC);
	if (pe_fd == -1) {
		perror("Error opening leader");
		ctrlc_handler(0, NULL, NULL);
		return -1;
	}

	/* Start counters to log values */
	ioctl(pe_fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(pe_fd, PERF_EVENT_IOC_ENABLE, 0);

	return 0;
}

/*
 * Get LLC Occupancy as reported by RESCTRL FS
 * For CMT,
 * 1. If con_mon grp and mon grp given, then read from mon grp in
 * con_mon grp
 * 2. If only con_mon grp given, then read from con_mon grp
 * 3. If both not given, then read from root con_mon grp
 * For CAT,
 * 1. If con_mon grp given, then read from it
 * 2. If con_mon grp not given, then read from root con_mon grp
 *
 * Return: =0 on success.  <0 on failure.
 */
static int get_llc_occu_resctrl(unsigned long *llc_occupancy)
{
	FILE *fp;

	fp = fopen(llc_occup_path, "r");
	if (!fp) {
		perror("Failed to open results file");

		return errno;
	}
	if (fscanf(fp, "%lu", llc_occupancy) <= 0) {
		perror("Could not get llc occupancy");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	return 0;
}

/*
 * print_results_cache:	the cache results are stored in a file
 * @filename:		file that stores the results
 * @bm_pid:		child pid that runs benchmark
 * @llc_value:		perf miss value /
 *			llc occupancy value reported by resctrl FS
 *
 * Return:		0 on success. non-zero on failure.
 */
static int print_results_cache(char *filename, int bm_pid, __u64 llc_value)
{
	FILE *fp;

	if (strcmp(filename, "stdio") == 0 || strcmp(filename, "stderr") == 0) {
		printf("Pid: %d \t LLC_value: %llu\n", bm_pid, llc_value);
	} else {
		fp = fopen(filename, "a");
		if (!fp) {
			perror("Cannot open results file");

			return errno;
		}
		fprintf(fp, "Pid: %d \t llc_value: %llu\n", bm_pid, llc_value);
		fclose(fp);
	}

	return 0;
}

/*
 * perf_event_measure:	measure perf events
 * @bm_pid:	child pid that runs benchmark
 *
 * Measure things like cache misses from perf events.
 *
 * Return: =0 on success.  <0 on failure.
 */
static int perf_event_measure(struct resctrl_val_param *param, int bm_pid)
{
	int ret;

	/* Stop counters after one span to get miss rate */
	ioctl(pe_fd, PERF_EVENT_IOC_DISABLE, 0);

	ret = read(pe_fd, &pe_read, sizeof(struct perf_event_read));
	close(pe_fd);
	if (ret == -1) {
		perror("Could not get perf value");
		return -1;
	}

	return print_results_cache(param->filename, bm_pid, pe_read.values[0].value);
}

int measure_llc_resctrl(struct resctrl_val_param *param, int bm_pid)
{
	unsigned long llc_occu_resc = 0;
	int ret;

	/*
	 * Measure llc occupancy from resctrl.
	 */
	ret = get_llc_occu_resctrl(&llc_occu_resc);
	if (ret < 0)
		return ret;

	ret = print_results_cache(param->filename, bm_pid, llc_occu_resc);
	return ret;
}

/*
 * cache_val:		execute benchmark and measure LLC occupancy resctrl
 * and perf cache miss for the benchmark
 * @param:		parameters passed to cache_val()
 * @span:		buffer size for the benchmark
 *
 * Return:		0 on success. non-zero on failure.
 */
int cat_val(struct resctrl_val_param *param, size_t span)
{
	int memflush = 1, operation = 0, ret = 0;
	char *resctrl_val = param->resctrl_val;
	pid_t bm_pid;

	if (strcmp(param->filename, "") == 0)
		sprintf(param->filename, "stdio");

	bm_pid = getpid();

	/* Taskset benchmark to specified cpu */
	ret = taskset_benchmark(bm_pid, param->cpu_no);
	if (ret)
		return ret;

	/* Write benchmark to specified con_mon grp, mon_grp in resctrl FS*/
	ret = write_bm_pid_to_resctrl(bm_pid, param->ctrlgrp, param->mongrp,
				      resctrl_val);
	if (ret)
		return ret;

	perf_event_attr_initialize(PERF_COUNT_HW_CACHE_MISSES);
	perf_event_initialize_read_format();

	/* Test runs until the callback setup() tells the test to stop. */
	while (1) {
		ret = param->setup(param);
		if (ret == END_OF_TESTS) {
			ret = 0;
			break;
		}
		if (ret < 0)
			break;
		ret = perf_event_reset_enable(bm_pid, param->cpu_no);
		if (ret)
			break;

		if (run_fill_buf(span, memflush, operation, true)) {
			fprintf(stderr, "Error-running fill buffer\n");
			ret = -1;
			goto pe_close;
		}

		sleep(1);
		ret = perf_event_measure(param, bm_pid);
		if (ret)
			goto pe_close;
	}

	return ret;

pe_close:
	close(pe_fd);
	return ret;
}

/*
 * show_cache_info:	show generic cache test information
 * @no_of_bits:		number of bits
 * @avg_llc_val:	avg of LLC cache result data
 * @cache_span:		cache span
 * @lines:		cache span in lines or bytes
 */
void show_cache_info(int no_of_bits, __u64 avg_llc_val, size_t cache_span, bool lines)
{
	ksft_print_msg("Number of bits: %d\n", no_of_bits);
	ksft_print_msg("Average LLC val: %llu\n", avg_llc_val);
	ksft_print_msg("Cache span (%s): %zu\n", !lines ? "bytes" : "lines",
		       cache_span);
}
