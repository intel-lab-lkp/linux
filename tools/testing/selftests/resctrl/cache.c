// SPDX-License-Identifier: GPL-2.0

#include <stdint.h>
#include "resctrl.h"

char llc_occup_path[1024];

void perf_event_attr_initialize(struct perf_event_attr *pea, __u64 config)
{
	memset(pea, 0, sizeof(*pea));
	pea->type = PERF_TYPE_HARDWARE;
	pea->size = sizeof(struct perf_event_attr);
	pea->read_format = PERF_FORMAT_GROUP;
	pea->exclude_kernel = 1;
	pea->exclude_hv = 1;
	pea->exclude_idle = 1;
	pea->exclude_callchain_kernel = 1;
	pea->inherit = 1;
	pea->exclude_guest = 1;
	pea->disabled = 1;
	pea->config = config;
}

void perf_event_initialize_read_format(struct perf_event_read *pe_read)
{
	memset(pe_read, 0, sizeof(*pe_read));
	pe_read->nr = 1;
}

int perf_event_reset_enable(struct perf_event_attr *pea, pid_t pid, int cpu_no)
{
	int pe_fd;

	pe_fd = perf_event_open(pea, pid, cpu_no, -1, PERF_FLAG_FD_CLOEXEC);
	if (pe_fd == -1) {
		perror("Error opening leader");
		ctrlc_handler(0, NULL, NULL);
		return -1;
	}

	/* Start counters to log values */
	ioctl(pe_fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(pe_fd, PERF_EVENT_IOC_ENABLE, 0);

	return pe_fd;
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
int perf_event_measure(int pe_fd, struct perf_event_read *pe_read,
		       struct resctrl_val_param *param, int bm_pid)
{
	int ret;

	/* Stop counters after one span to get miss rate */
	ioctl(pe_fd, PERF_EVENT_IOC_DISABLE, 0);

	ret = read(pe_fd, pe_read, sizeof(*pe_read));
	if (ret == -1) {
		perror("Could not get perf value");
		return -1;
	}

	return print_results_cache(param->filename, bm_pid, pe_read->values[0].value);
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
