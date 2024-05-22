// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include "../../../kselftest.h"

#define WORKLOAD_NOTIFICATION_DELAY_ATTRIBUTE "/sys/bus/pci/devices/0000:00:04.0/workload_hint/notification_delay_ms"
#define WORKLOAD_ENABLE_ATTRIBUTE "/sys/bus/pci/devices/0000:00:04.0/workload_hint/workload_hint_enable"
#define WORKLOAD_TYPE_INDEX_ATTRIBUTE  "/sys/bus/pci/devices/0000:00:04.0/workload_hint/workload_type_index"

static const char * const workload_types[] = {
	"idle",
	"battery_life",
	"sustained",
	"bursty",
	NULL
};

#define WORKLOAD_TYPE_MAX_INDEX	3

void workload_hint_exit(int signum)
{
	int fd;

	/* Disable feature via sysfs knob */

	fd = open(WORKLOAD_ENABLE_ATTRIBUTE, O_RDWR);
	if (fd < 0)
		ksft_exit_fail_perror("Unable to open workload type feature enable file");

	if (write(fd, "0\n", 2) < 0)
		ksft_exit_fail_perror("Can' disable workload hints");

	ksft_print_msg("Disabled workload type prediction\n");

	close(fd);
}

int main(int argc, char **argv)
{
	struct pollfd ufd;
	char index_str[4];
	int fd, ret, index;
	char delay_str[64];
	int delay = 0;

	ksft_print_header();
	ksft_set_plan(1);

	ksft_print_msg("Usage: workload_hint_test [notification delay in milli seconds]\n");

	if (argc > 1) {
		ret = sscanf(argv[1], "%d", &delay);
		if (ret < 0)
			ksft_exit_fail_perror("Invalid delay");

		ksft_print_msg("Setting notification delay to %d ms\n", delay);
		if (delay < 0)
			ksft_exit_fail_msg("delay can never be negative\n");

		sprintf(delay_str, "%s\n", argv[1]);
		fd = open(WORKLOAD_NOTIFICATION_DELAY_ATTRIBUTE, O_RDWR);
		if (fd < 0)
			ksft_exit_fail_perror("Unable to open workload notification delay");

		if (write(fd, delay_str, strlen(delay_str)) < 0)
			ksft_exit_fail_perror("Can't set delay");

		close(fd);
	}

	if (signal(SIGINT, workload_hint_exit) == SIG_IGN)
		signal(SIGINT, SIG_IGN);
	if (signal(SIGHUP, workload_hint_exit) == SIG_IGN)
		signal(SIGHUP, SIG_IGN);
	if (signal(SIGTERM, workload_hint_exit) == SIG_IGN)
		signal(SIGTERM, SIG_IGN);

	/* Enable feature via sysfs knob */
	fd = open(WORKLOAD_ENABLE_ATTRIBUTE, O_RDWR);
	if (fd < 0)
		ksft_exit_fail_perror("Unable to open workload type feature enable file");

	if (write(fd, "1\n", 2) < 0)
		ksft_exit_fail_perror("Can' enable workload hints");

	close(fd);

	ksft_print_msg("Enabled workload type prediction\n");

	while (1) {
		fd = open(WORKLOAD_TYPE_INDEX_ATTRIBUTE, O_RDONLY);
		if (fd < 0)
			ksft_exit_fail_perror("Unable to open workload type file");

		if ((lseek(fd, 0L, SEEK_SET)) < 0)
			ksft_exit_fail_perror("Failed to set pointer to beginning");

		if (read(fd, index_str, sizeof(index_str)) < 0)
			ksft_exit_fail_perror("Failed to read from: workload_type_index");

		ufd.fd = fd;
		ufd.events = POLLPRI;

		ret = poll(&ufd, 1, -1);
		if (ret < 0) {
			ksft_exit_fail_perror("poll error");
		} else if (ret == 0) {
			ksft_print_msg("Poll Timeout\n");
		} else {
			if ((lseek(fd, 0L, SEEK_SET)) < 0)
				ksft_exit_fail_perror("Failed to set pointer to beginning");

			if (read(fd, index_str, sizeof(index_str)) < 0) {
				ksft_test_result_pass("Successfully read\n");
				ksft_finished();
			}

			ret = sscanf(index_str, "%d", &index);
			if (ret < 0)
				break;
			if (index > WORKLOAD_TYPE_MAX_INDEX)
				ksft_print_msg("Invalid workload type index\n");
			else
				ksft_print_msg("workload type:%s\n", workload_types[index]);
		}

		close(fd);
	}
}
