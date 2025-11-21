// SPDX-License-Identifier: GPL-2.0-only
/*
 * POEG - example userspace application
 * Copyright (C) 2023 Biju Das
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <poll.h>

#include <linux/poeg.h>

int main(int argc, char *argv[])
{
	struct poeg_cmd cmd;
	unsigned int val;
	long cmd_val;
	char *p;
	int i;

	cmd_val = strtol(argv[1], &p, 10);
	if (*p != '\0' || errno != 0)
		return 1; // In main(), returning non-zero means failure

	fd = open("/dev/poeg3", O_RDWR);
	if (fd < 0)
		perror("open");
	else
		printf("[POEG]open\n");

	cmd.val = cmd_val;
	cmd.channel = 4;
	if (cmd.val == RZG2L_POEG_OUTPUT_DISABLE_USR_ENABLE_CMD)
		printf("[POEG] user control pin output disable enabled\n");
	else
		printf("[POEG] user control pin output disable disabled\n");

	ret = write(fd, &cmd, sizeof(cmd));
	if (ret == -1) {
		perror("Failed to write cmd data");
		return 1;
	}

	if (close(fd) != 0)
		perror("close");
	else
		printf("[POEG]close\n");

	return 0;
}
