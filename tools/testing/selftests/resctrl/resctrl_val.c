// SPDX-License-Identifier: GPL-2.0
/*
 * Memory bandwidth monitoring and allocation library
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include "resctrl.h"

#define UNCORE_IMC		"uncore_imc_"
#define READ_FILE_NAME		"cas_count_read"
#define DYN_PMU_PATH		"/sys/bus/event_source/devices"
#define SCALE			0.00006103515625
#define MAX_MCS			40
#define MAX_TOKENS		5

#define CON_MBM_LOCAL_BYTES_PATH		\
	"%s/%s/mon_data/mon_L3_%02d/mbm_local_bytes"

struct membw_read_format {
	__u64 value;         /* The value of the event */
	__u64 time_enabled;  /* if PERF_FORMAT_TOTAL_TIME_ENABLED */
	__u64 time_running;  /* if PERF_FORMAT_TOTAL_TIME_RUNNING */
	__u64 id;            /* if PERF_FORMAT_ID */
};

struct mc_counter_config {
	__u32 type;
	__u64 event;
	__u64 umask;
	struct perf_event_attr pe;
	int fd;
};

static char mbm_total_path[1024];
static int mcs;
static struct mc_counter_config mc_counters_config[MAX_MCS];
static const struct resctrl_test *current_test;

static void read_mem_bw_initialize_perf_event_attr(int i)
{
	memset(&mc_counters_config[i].pe, 0,
	       sizeof(struct perf_event_attr));
	mc_counters_config[i].pe.type = mc_counters_config[i].type;
	mc_counters_config[i].pe.size = sizeof(struct perf_event_attr);
	mc_counters_config[i].pe.disabled = 1;
	mc_counters_config[i].pe.inherit = 1;
	mc_counters_config[i].pe.exclude_guest = 0;
	mc_counters_config[i].pe.config =
		mc_counters_config[i].umask << 8 |
		mc_counters_config[i].event;
	mc_counters_config[i].pe.sample_type = PERF_SAMPLE_IDENTIFIER;
	mc_counters_config[i].pe.read_format =
		PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
}

static void read_mem_bw_ioctl_perf_event_ioc_reset_enable(int i)
{
	ioctl(mc_counters_config[i].fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(mc_counters_config[i].fd, PERF_EVENT_IOC_ENABLE, 0);
}

static void read_mem_bw_ioctl_perf_event_ioc_disable(int i)
{
	ioctl(mc_counters_config[i].fd, PERF_EVENT_IOC_DISABLE, 0);
}

/*
 * get_read_event_and_umask:	Parse config into event and umask
 * @cas_count_cfg:	Config
 * @count:		MC number
 */
static void get_read_event_and_umask(char *cas_count_cfg, unsigned int count)
{
	char *token[MAX_TOKENS];
	int i = 0;

	token[0] = strtok(cas_count_cfg, "=,");

	for (i = 1; i < MAX_TOKENS; i++)
		token[i] = strtok(NULL, "=,");

	for (i = 0; i < MAX_TOKENS - 1; i++) {
		if (!token[i])
			break;
		if (strcmp(token[i], "event") == 0)
			mc_counters_config[count].event = strtol(token[i + 1], NULL, 16);
		if (strcmp(token[i], "umask") == 0)
			mc_counters_config[count].umask = strtol(token[i + 1], NULL, 16);
	}
}

static int open_perf_read_event(int i, int cpu_no)
{
	mc_counters_config[i].fd =
		perf_event_open(&mc_counters_config[i].pe, -1, cpu_no, -1,
				PERF_FLAG_FD_CLOEXEC);

	if (mc_counters_config[i].fd == -1) {
		fprintf(stderr, "Error opening leader %llx\n",
			mc_counters_config[i].pe.config);

		return -1;
	}

	return 0;
}

static int parse_mc_read_bw_events(char *mc_dir, unsigned int type,
				    unsigned int *count)
{
	char mc_events_dir[PATH_MAX], mc_counter_cfg[PATH_MAX];
	unsigned int orig_count = *count;
	char cas_count_cfg[1024];
	struct dirent *ep;
	int path_len;
	int ret = -1;
	int num_cfg;
	FILE *fp;
	DIR *dp;

	path_len = snprintf(mc_events_dir, sizeof(mc_events_dir), "%sevents",
			    mc_dir);
	if (path_len >= sizeof(mc_events_dir)) {
		ksft_print_msg("Unable to create path to %sevents\n", mc_dir);
		return -1;
	}

	dp = opendir(mc_events_dir);
	if (!dp) {
		ksft_perror("Unable to open PMU events directory");
		return -1;
	}

	while ((ep = readdir(dp))) {
		/*
		 * Parse all event files with READ_FILE_NAME prefix that
		 * contain the event number and umask. Skip files containing
		 * "." that contain unused properties of event.
		 */
		if (!strstr(ep->d_name, READ_FILE_NAME) ||
		    strchr(ep->d_name, '.'))
			continue;

		path_len = snprintf(mc_counter_cfg, sizeof(mc_counter_cfg),
				    "%s/%s", mc_events_dir, ep->d_name);
		if (path_len >= sizeof(mc_counter_cfg)) {
			ksft_print_msg("Unable to create path to %s/%s\n",
				       mc_events_dir, ep->d_name);
			goto out_close;
		}
		fp = fopen(mc_counter_cfg, "r");
		if (!fp) {
			ksft_perror("Failed to open MC config file");
			goto out_close;
		}
		num_cfg = fscanf(fp, "%1023s", cas_count_cfg);
		fclose(fp);
		if (num_cfg <= 0) {
			ksft_perror("Could not get MC cas count read");
			goto out_close;
		}
		if (*count >= MAX_MCS) {
			ksft_print_msg("Maximum MC count exceeded\n");
			goto out_close;
		}

		mc_counters_config[*count].type = type;
		get_read_event_and_umask(cas_count_cfg, *count);
		/* Do not fail after incrementing *count. */
		*count += 1;
	}
	if (*count == orig_count) {
		ksft_print_msg("Unable to find events in %s\n", mc_events_dir);
		goto out_close;
	}
	ret = 0;
out_close:
	closedir(dp);
	return ret;
}

/* Get type and config of an MC counter's read event. */
static int read_from_mc_dir(char *mc_dir, unsigned int *count)
{
	char mc_counter_type[PATH_MAX];
	unsigned int type;
	int path_len;
	FILE *fp;
	int ret;

	/* Get type of MC counter */
	path_len = snprintf(mc_counter_type, sizeof(mc_counter_type),
			    "%s%s", mc_dir, "type");
	if (path_len >= sizeof(mc_counter_type)) {
		ksft_print_msg("Unable to create path to %s%s\n",
			       mc_dir, "type");
		return -1;
	}
	fp = fopen(mc_counter_type, "r");
	if (!fp) {
		ksft_perror("Failed to open MC counter type file");

		return -1;
	}
	ret = fscanf(fp, "%u", &type);
	fclose(fp);
	if (ret <= 0) {
		ksft_perror("Could not get MC type");
		return -1;
	}
	ret = parse_mc_read_bw_events(mc_dir, type, count);
	if (ret) {
		ksft_print_msg("Unable to parse bandwidth event and umask\n");
		return ret;
	}

	return 0;
}

/*
 * A system can have 'n' number of MC (Memory Controller)
 * counters, get that 'n'. Discover the properties of the available
 * counters in support of needed performance measurement via perf.
 * For each MC counter get it's type and config. Also obtain each
 * counter's event and umask for the memory read events that will be
 * measured.
 *
 * Enumerate all these details into an array of structures.
 *
 * Return: >= 0 on success. < 0 on failure.
 */
static int num_of_mem_controllers(void)
{
	char mc_dir[512], *temp;
	unsigned int count = 0;
	struct dirent *ep;
	char *sysfs_name;
	int ret;
	DIR *dp;

	if (get_vendor() == ARCH_INTEL) {
		sysfs_name = UNCORE_IMC;
	} else {
		ksft_print_msg("Unsupported vendor\n");
		return -1;
	}

	dp = opendir(DYN_PMU_PATH);
	if (dp) {
		while ((ep = readdir(dp))) {
			temp = strstr(ep->d_name, sysfs_name);
			if (!temp)
				continue;

			/*
			 * mc counters are named as "uncore_imc_<n>", hence
			 * increment the pointer to point to <n>.
			 */
			temp = temp + strlen(sysfs_name);

			/*
			 * Some directories under "DYN_PMU_PATH" could have
			 * names like "uncore_imc_free_running", hence, check if
			 * first character is a numerical digit or not.
			 */
			if (temp[0] >= '0' && temp[0] <= '9') {
				sprintf(mc_dir, "%s/%s/", DYN_PMU_PATH,
					ep->d_name);
				ret = read_from_mc_dir(mc_dir, &count);
				if (ret) {
					closedir(dp);

					return ret;
				}
			}
		}
		closedir(dp);
		if (count == 0) {
			ksft_print_msg("Unable to find MC counters\n");

			return -1;
		}
	} else {
		ksft_perror("Unable to open PMU directory");

		return -1;
	}

	return count;
}

int initialize_read_mem_bw_mc(void)
{
	int mc;

	mcs = num_of_mem_controllers();
	if (mcs <= 0)
		return mcs;

	/* Initialize perf_event_attr structures for all MC's */
	for (mc = 0; mc < mcs; mc++)
		read_mem_bw_initialize_perf_event_attr(mc);

	return 0;
}

static void perf_close_mc_read_mem_bw(void)
{
	int mc;

	for (mc = 0; mc < mcs; mc++) {
		if (mc_counters_config[mc].fd != -1)
			close(mc_counters_config[mc].fd);
	}
}

/*
 * perf_open_mc_read_mem_bw - Open perf fds for MCs
 * @cpu_no: CPU number that the benchmark PID is bound to
 *
 * Return: = 0 on success. < 0 on failure.
 */
static int perf_open_mc_read_mem_bw(int cpu_no)
{
	int mc, ret;

	for (mc = 0; mc < mcs; mc++)
		mc_counters_config[mc].fd = -1;

	for (mc = 0; mc < mcs; mc++) {
		ret = open_perf_read_event(mc, cpu_no);
		if (ret)
			goto close_fds;
	}

	return 0;

close_fds:
	perf_close_mc_read_mem_bw();
	return -1;
}

/*
 * do_mc_read_mem_bw_test - Perform memory bandwidth test
 *
 * Runs memory bandwidth test over one second period. Also, handles starting
 * and stopping of the MC perf counters around the test.
 */
static void do_mc_read_mem_bw_test(void)
{
	int mc;

	for (mc = 0; mc < mcs; mc++)
		read_mem_bw_ioctl_perf_event_ioc_reset_enable(mc);

	sleep(1);

	/* Stop counters after a second to get results. */
	for (mc = 0; mc < mcs; mc++)
		read_mem_bw_ioctl_perf_event_ioc_disable(mc);
}

/*
 * get_read_mem_bw_mc - Memory read bandwidth as reported by MC counters
 *
 * Memory read bandwidth utilized by a process on a socket can be calculated
 * using MC counters' read events. Perf events are used to read these
 * counters.
 *
 * Return: = 0 on success. < 0 on failure.
 */
static int get_read_mem_bw_mc(float *bw_mc)
{
	float reads = 0, of_mul_read = 1;
	int mc;

	/*
	 * Log read event values from all MC counters into
	 * struct mc_counter_config.
	 * Take overflow into consideration before calculating total bandwidth.
	 */
	for (mc = 0; mc < mcs; mc++) {
		struct membw_read_format measurement;
		struct mc_counter_config *r =
			&mc_counters_config[mc];

		if (read(r->fd, &measurement, sizeof(measurement)) == -1) {
			ksft_perror("Couldn't get read bandwidth through MC");
			return -1;
		}

		__u64 r_time_enabled = measurement.time_enabled;
		__u64 r_time_running = measurement.time_running;

		if (r_time_enabled != r_time_running)
			of_mul_read = (float)r_time_enabled /
					(float)r_time_running;

		reads += measurement.value * of_mul_read * SCALE;
	}

	*bw_mc = reads;
	return 0;
}

/*
 * initialize_mem_bw_resctrl:	Appropriately populate "mbm_total_path"
 * @param:	Parameters passed to resctrl_val()
 * @domain_id:	Domain ID (cache ID; for MB, L3 cache ID)
 */
void initialize_mem_bw_resctrl(const struct resctrl_val_param *param,
			       int domain_id)
{
	sprintf(mbm_total_path, CON_MBM_LOCAL_BYTES_PATH, RESCTRL_PATH,
		param->ctrlgrp, domain_id);
}

/*
 * Open file to read MBM local bytes from resctrl FS
 */
static FILE *open_mem_bw_resctrl(const char *mbm_bw_file)
{
	FILE *fp;

	fp = fopen(mbm_bw_file, "r");
	if (!fp)
		ksft_perror("Failed to open total memory bandwidth file");

	return fp;
}

/*
 * Get MBM Local bytes as reported by resctrl FS
 */
static int get_mem_bw_resctrl(FILE *fp, unsigned long *mbm_total)
{
	if (fscanf(fp, "%lu\n", mbm_total) <= 0) {
		ksft_perror("Could not get MBM local bytes");
		return -1;
	}
	return 0;
}

static pid_t bm_pid;

void ctrlc_handler(int signum, siginfo_t *info, void *ptr)
{
	/* Only kill child after bm_pid is set after fork() */
	if (bm_pid)
		kill(bm_pid, SIGKILL);
	umount_resctrlfs();
	if (current_test && current_test->cleanup)
		current_test->cleanup();
	ksft_print_msg("Ending\n\n");

	exit(EXIT_SUCCESS);
}

/*
 * Register CTRL-C handler for parent, as it has to kill
 * child process before exiting.
 */
int signal_handler_register(const struct resctrl_test *test)
{
	struct sigaction sigact = {};
	int ret = 0;

	bm_pid = 0;

	current_test = test;
	sigact.sa_sigaction = ctrlc_handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = SA_SIGINFO;
	if (sigaction(SIGINT, &sigact, NULL) ||
	    sigaction(SIGTERM, &sigact, NULL) ||
	    sigaction(SIGHUP, &sigact, NULL)) {
		ksft_perror("sigaction");
		ret = -1;
	}
	return ret;
}

/*
 * Reset signal handler to SIG_DFL.
 * Non-Value return because the caller should keep
 * the error code of other path even if sigaction fails.
 */
void signal_handler_unregister(void)
{
	struct sigaction sigact = {};

	current_test = NULL;
	sigact.sa_handler = SIG_DFL;
	sigemptyset(&sigact.sa_mask);
	if (sigaction(SIGINT, &sigact, NULL) ||
	    sigaction(SIGTERM, &sigact, NULL) ||
	    sigaction(SIGHUP, &sigact, NULL)) {
		ksft_perror("sigaction");
	}
}

/*
 * print_results_bw:	the memory bandwidth results are stored in a file
 * @filename:		file that stores the results
 * @bm_pid:		child pid that runs benchmark
 * @bw_mc:		perf mc counter value
 * @bw_resc:		memory bandwidth value
 *
 * Return:		0 on success, < 0 on error.
 */
static int print_results_bw(char *filename, pid_t bm_pid, float bw_mc,
			    unsigned long bw_resc)
{
	unsigned long diff = fabs(bw_mc - bw_resc);
	FILE *fp;

	if (strcmp(filename, "stdio") == 0 || strcmp(filename, "stderr") == 0) {
		printf("Pid: %d \t Mem_BW_MC: %f \t ", (int)bm_pid, bw_mc);
		printf("Mem_BW_resc: %lu \t Difference: %lu\n", bw_resc, diff);
	} else {
		fp = fopen(filename, "a");
		if (!fp) {
			ksft_perror("Cannot open results file");

			return -1;
		}
		if (fprintf(fp, "Pid: %d \t Mem_BW_MC: %f \t Mem_BW_resc: %lu \t Difference: %lu\n",
			    (int)bm_pid, bw_mc, bw_resc, diff) <= 0) {
			ksft_print_msg("Could not log results\n");
			fclose(fp);

			return -1;
		}
		fclose(fp);
	}

	return 0;
}

/*
 * measure_read_mem_bw - Measures read memory bandwidth numbers while benchmark runs
 * @uparams:		User supplied parameters
 * @param:		Parameters passed to resctrl_val()
 * @bm_pid:		PID that runs the benchmark
 *
 * Measure memory bandwidth from resctrl and from another source which is
 * perf mc value or could be something else if perf mc event is not
 * available. Compare the two values to validate resctrl value. It takes
 * 1 sec to measure the data.
 * resctrl does not distinguish between read and write operations so
 * its data includes all memory operations.
 */
int measure_read_mem_bw(const struct user_params *uparams,
			struct resctrl_val_param *param, pid_t bm_pid)
{
	unsigned long bw_resc, bw_resc_start, bw_resc_end;
	FILE *mem_bw_fp;
	float bw_mc;
	int ret;

	mem_bw_fp = open_mem_bw_resctrl(mbm_total_path);
	if (!mem_bw_fp)
		return -1;

	ret = perf_open_mc_read_mem_bw(uparams->cpu);
	if (ret < 0)
		goto close_fp;

	ret = get_mem_bw_resctrl(mem_bw_fp, &bw_resc_start);
	if (ret < 0)
		goto close_mc;

	rewind(mem_bw_fp);

	do_mc_read_mem_bw_test();

	ret = get_mem_bw_resctrl(mem_bw_fp, &bw_resc_end);
	if (ret < 0)
		goto close_mc;

	ret = get_read_mem_bw_mc(&bw_mc);
	if (ret < 0)
		goto close_mc;

	perf_close_mc_read_mem_bw();
	fclose(mem_bw_fp);

	bw_resc = (bw_resc_end - bw_resc_start) / MB;

	return print_results_bw(param->filename, bm_pid, bw_mc, bw_resc);

close_mc:
	perf_close_mc_read_mem_bw();
close_fp:
	fclose(mem_bw_fp);
	return ret;
}

/*
 * resctrl_val:	execute benchmark and measure memory bandwidth on
 *			the benchmark
 * @test:		test information structure
 * @uparams:		user supplied parameters
 * @param:		parameters passed to resctrl_val()
 *
 * Return:		0 when the test was run, < 0 on error.
 */
int resctrl_val(const struct resctrl_test *test,
		const struct user_params *uparams,
		struct resctrl_val_param *param)
{
	unsigned char *buf = NULL;
	cpu_set_t old_affinity;
	int domain_id;
	int ret = 0;
	pid_t ppid;

	if (strcmp(param->filename, "") == 0)
		sprintf(param->filename, "stdio");

	ret = get_domain_id(test->resource, uparams->cpu, &domain_id);
	if (ret < 0) {
		ksft_print_msg("Could not get domain ID\n");
		return ret;
	}

	ppid = getpid();

	/* Taskset test to specified CPU. */
	ret = taskset_benchmark(ppid, uparams->cpu, &old_affinity);
	if (ret)
		return ret;

	/* Write test to specified control & monitoring group in resctrl FS. */
	ret = write_bm_pid_to_resctrl(ppid, param->ctrlgrp, param->mongrp);
	if (ret)
		goto reset_affinity;

	if (param->init) {
		ret = param->init(test, uparams, param, domain_id);
		if (ret)
			goto reset_affinity;
	}

	/*
	 * If not running user provided benchmark, run the default
	 * "fill_buf". First phase of "fill_buf" is to prepare the
	 * buffer that the benchmark will operate on. No measurements
	 * are needed during this phase and prepared memory will be
	 * passed to next part of benchmark via copy-on-write thus
	 * no impact on the benchmark that relies on reading from
	 * memory only.
	 */
	if (param->fill_buf) {
		buf = alloc_buffer(param->fill_buf->buf_size,
				   param->fill_buf->memflush);
		if (!buf) {
			ret = -ENOMEM;
			goto reset_affinity;
		}
	}

	fflush(stdout);
	bm_pid = fork();
	if (bm_pid == -1) {
		ret = -errno;
		ksft_perror("Unable to fork");
		goto free_buf;
	}

	/*
	 * What needs to be measured runs in separate process until
	 * terminated.
	 */
	if (bm_pid == 0) {
		if (param->fill_buf)
			fill_cache_read(buf, param->fill_buf->buf_size, false);
		else if (uparams->benchmark_cmd[0])
			execvp(uparams->benchmark_cmd[0], (char **)uparams->benchmark_cmd);
		exit(EXIT_SUCCESS);
	}

	ksft_print_msg("Benchmark PID: %d\n", (int)bm_pid);

	/* Give benchmark enough time to fully run. */
	sleep(1);

	/* Test runs until the callback setup() tells the test to stop. */
	while (1) {
		ret = param->setup(test, uparams, param);
		if (ret == END_OF_TESTS) {
			ret = 0;
			break;
		}
		if (ret < 0)
			break;

		ret = param->measure(uparams, param, bm_pid);
		if (ret)
			break;
	}

	kill(bm_pid, SIGKILL);
free_buf:
	free(buf);
reset_affinity:
	taskset_restore(ppid, &old_affinity);
	return ret;
}
