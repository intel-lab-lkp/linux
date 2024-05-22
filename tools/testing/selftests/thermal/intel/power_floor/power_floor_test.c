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

#define POWER_FLOOR_ENABLE_ATTRIBUTE "/sys/bus/pci/devices/0000:00:04.0/power_limits/power_floor_enable"
#define POWER_FLOOR_STATUS_ATTRIBUTE  "/sys/bus/pci/devices/0000:00:04.0/power_limits/power_floor_status"

void power_floor_exit(int signum)
{
	int fd;

	/* Disable feature via sysfs knob */

	fd = open(POWER_FLOOR_ENABLE_ATTRIBUTE, O_RDWR);
	if (fd < 0)
		ksft_exit_fail_perror("Unable to open power floor enable file");

	if (write(fd, "0\n", 2) < 0)
		ksft_exit_fail_perror("Can' disable power floor notifications");

	ksft_print_msg("Disabled power floor notifications\n");

	close(fd);
}

int main(int argc, char **argv)
{
	struct pollfd ufd;
	char status_str[3];
	int fd, ret;

	ksft_print_header();
	ksft_set_plan(1);

	if (signal(SIGINT, power_floor_exit) == SIG_IGN)
		signal(SIGINT, SIG_IGN);
	if (signal(SIGHUP, power_floor_exit) == SIG_IGN)
		signal(SIGHUP, SIG_IGN);
	if (signal(SIGTERM, power_floor_exit) == SIG_IGN)
		signal(SIGTERM, SIG_IGN);

	/* Enable feature via sysfs knob */
	fd = open(POWER_FLOOR_ENABLE_ATTRIBUTE, O_RDWR);
	if (fd < 0)
		ksft_exit_fail_perror("Unable to open power floor enable file");

	if (write(fd, "1\n", 2) < 0)
		ksft_exit_fail_perror("Can' enable power floor notifications");

	close(fd);

	ksft_print_msg("Enabled power floor notifications\n");

	while (1) {
		fd = open(POWER_FLOOR_STATUS_ATTRIBUTE, O_RDONLY);
		if (fd < 0)
			ksft_exit_fail_perror("Unable to power floor status file");

		if ((lseek(fd, 0L, SEEK_SET)) < 0)
			ksft_exit_fail_perror("Failed to set pointer to beginning\n");

		if (read(fd, status_str, sizeof(status_str)) < 0) {
			ksft_exit_fail_perror(stderr, "Failed to read from: power_floor_status");

		ufd.fd = fd;
		ufd.events = POLLPRI;

		ret = poll(&ufd, 1, -1);
		if (ret < 0) {
			ksft_exit_fail_msg("Poll error\n");
		} else if (ret == 0) {
			ksft_print_msg("Poll Timeout\n");
		} else {
			if ((lseek(fd, 0L, SEEK_SET)) < 0)
				ksft_exit_fail_msg("Failed to set pointer to beginning\n");

			if (read(fd, status_str, sizeof(status_str)) < 0) {
				ksft_test_result_pass("Successfully read\n");
				ksft_finished();
			}

			ksft_print_msg("power floor status: %s\n", status_str);
		}

		close(fd);
	}
}
